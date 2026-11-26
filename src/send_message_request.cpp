#include <cstdint>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <simdjson.h>
#include <spdlog/spdlog.h>

#include "chat_session.hpp"
#include <chat/error.hpp>
#include "message_payload.hpp"
#include "s3_objects.hpp"
#include "upload_store.hpp"
#include "database.hpp"
#include "message_history.hpp"
#include "message_store.hpp"
#include <chat/text.hpp>
#include <set>
#include <sqlite3.h>

namespace
{

constexpr std::size_t kMaxMessageSize = 64 * 1024;

struct[[= simdjson::deny_unknown_fields]] send_message_params
{
    std::int64_t conversation = 0;
    std::optional<std::string> text;
    std::optional<std::int64_t> reply_to;
    std::optional<std::string> object_key;
    std::optional<std::int64_t> forward;
    std::optional<std::vector<std::int64_t>> bundle;
    std::optional<std::vector<std::string>> poll_options;
    std::optional<chat::poll_settings> poll_settings;
    [[= simdjson::default_value]] bool markdown = false;
    [[= simdjson::default_value]] bool mention_all = false;
    std::optional<std::int64_t> upload;
    std::optional<std::int64_t> client_id;
};

bool read_send_params(json_rpc_params& params, send_message_params& value, bool attaching)
{
    if (attaching)
    {
        struct[[= simdjson::deny_unknown_fields]] finish_params
        {
            std::int64_t upload = 0;
            std::optional<std::int64_t> reply_to;
        };
        finish_params finish;
        if (!params.read(finish) || finish.upload <= 0 || (finish.reply_to && *finish.reply_to <= 0))
        {
            return false;
        }
        value.upload = finish.upload;
        value.reply_to = finish.reply_to;
        return true;
    }
    if (!params.read(value) || value.upload || value.conversation <= 0 || !value.client_id || *value.client_id <= 0 ||
        (value.reply_to && *value.reply_to <= 0))
    {
        return false;
    }
    if (value.poll_settings && !value.poll_options)
    {
        return false;
    }
    if (value.poll_options)
    {
        auto valid = [](std::string_view s, std::size_t max)
        {
            return !s.contains('\0') && !chat::text_is_blank(s) &&
                   std::ranges::count_if(s, [](unsigned char c) { return (c & 0xc0) != 0x80; }) <= static_cast<std::ptrdiff_t>(max);
        };
        if (!value.text || !valid(*value.text, 256) || value.bundle || value.forward || value.object_key || value.markdown || value.mention_all ||
            value.reply_to)
        {
            return false;
        }
        auto const& options = *value.poll_options;
        if (options.size() < 2 || options.size() > 10)
        {
            return false;
        }
        if (value.poll_settings)
        {
            auto const& settings = *value.poll_settings;
            if (settings.correct < 0 || settings.correct > static_cast<std::int64_t>(options.size()) || (settings.correct && settings.multiple) ||
                (!settings.explanation.empty() && (!settings.correct || !valid(settings.explanation, 200))))
            {
                return false;
            }
        }
        std::set<std::string_view> unique;
        return std::ranges::all_of(options, [&](auto const& label) { return valid(label, 80) && unique.insert(label).second; });
    }
    if (value.bundle)
    {
        auto& ids = *value.bundle;
        if (ids.size() < 2 || ids.size() > 50 || value.text || value.forward || value.object_key || value.reply_to || value.markdown ||
            value.mention_all)
        {
            return false;
        }
        std::ranges::sort(ids);
        if (ids.front() <= 0 || std::ranges::adjacent_find(ids) != ids.end())
        {
            return false;
        }
        value.text = "聊天记录";
        return true;
    }
    if (value.forward)
    {
        return *value.forward > 0 && !value.text && !value.object_key && !value.reply_to && !value.markdown && !value.mention_all;
    }
    if (value.object_key)
    {
        return !value.text && !value.object_key->empty() && !value.markdown && !value.mention_all;
    }
    return value.text && !value.text->empty() && !value.text->contains('\0');
}

struct delivery
{
    std::string acknowledgement;
    message_broadcast notification;
};

struct hidden_receipt
{
    std::int64_t id, conversation, from, client_id;
    bool hidden = true;
};

using send_outcome = std::variant<delivery, message_write_rejection>;

std::expected<send_outcome, sqlite_error> format_delivery(sqlite_database& db,
                                                          message_to_write const& input,
                                                          message_write_outcome const& outcome,
                                                          json_rpc_id const& request_id)
{
    if (auto const* rejected = std::get_if<message_write_rejection>(&outcome))
    {
        return *rejected;
    }
    auto const& sent = std::get<message_write>(outcome);
    auto message = read_message(db, input.sender, input.conversation, sent.id);
    if (!message)
    {
        return std::unexpected(std::move(message.error()));
    }
    delivery result;
    if (!*message)
    {
        if (sent.inserted)
        {
            return message_write_rejection::unavailable;
        }
        hidden_receipt receipt{sent.id, input.conversation, input.sender, input.client_id};
        if (request_id.present && serialize_json_rpc_value(receipt, request_id, result.acknowledgement))
        {
            return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize stored receipt"});
        }
        return result;
    }
    auto value = make_message_payload(**message, true);
    if (request_id.present && serialize_json_rpc_value(value, request_id, result.acknowledgement))
    {
        return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize message acknowledgement"});
    }
    if (sent.inserted)
    {
        if (result.notification.serialize("message", std::move(value)))
        {
            return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize message notification"});
        }
        if (result.notification.json.size() > kMaxMessageSize)
        {
            return std::unexpected(sqlite_error{SQLITE_TOOBIG, "message exceeds the frame limit"});
        }
    }
    return result;
}

std::expected<send_outcome, sqlite_error> persist_delivery(
    sqlite_database& db, message_to_write const& message, json_rpc_id const& request_id, std::string_view key = {}, std::int64_t upload = 0)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    if (upload)
    {
        auto registered = register_uploaded_file(db, upload);
        if (!registered)
        {
            return std::unexpected(registered.error());
        }
        if (!*registered)
        {
            return message_write_rejection::unavailable;
        }
    }
    auto saved = insert_message(db, message, key);
    if (!saved)
    {
        return std::unexpected(std::move(saved.error()));
    }
    auto formatted = format_delivery(db, message, *saved, request_id);
    if (!formatted || std::holds_alternative<message_write_rejection>(*formatted))
    {
        return formatted;
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return formatted;
}

std::expected<std::optional<send_outcome>, sqlite_error> check_attachment_delivery(sqlite_database& db,
                                                                                   message_to_write const& message,
                                                                                   json_rpc_id const& id)
{
    auto snapshot = sqlite_transaction::begin_read(db);
    if (!snapshot)
    {
        return std::unexpected(std::move(snapshot.error()));
    }
    auto checked = check_message_write(db, message, true);
    if (!checked)
    {
        return std::unexpected(std::move(checked.error()));
    }
    if (!*checked)
    {
        return std::nullopt;
    }
    return format_delivery(db, message, **checked, id);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_send_message(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    send_message_params params{};
    bool const attaching = request.method == "finish_attachment";
    if (!read_send_params(request.params, params, attaching))
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }
    auto const user = *user_id_;
    std::expected<send_outcome, sqlite_error> delivered;
    if (attaching)
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        auto task = co_await run_db([&](sqlite_database& db) { return read_upload(db, user, device_, *params.upload); });
        if (!task)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!*task)
        {
            auto sent = co_await find_sent_message(*params.upload);
            if (!sent)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            if (*sent && (**sent).member && (**sent).attachment)
            {
                co_return serialize_json_rpc_success((**sent).json, std::move(request.id), response);
            }
        }
        if (!*task || (**task).state != "active" || (**task).parts.empty() || (**task).parts.back().etag.empty())
        {
            co_return serialize_json_rpc_error(-32008, "Attachment upload incomplete", std::move(request.id), response);
        }
        auto const& t = **task;
        params.conversation = t.conversation;
        message_to_write const message{user, t.conversation, t.client_id, t.filename, params.reply_to};
        auto preflight = co_await run_db([&](sqlite_database& db) { return check_attachment_delivery(db, message, request.id); });
        if (!preflight)
        {
            delivered = std::unexpected(preflight.error());
        }
        else if (*preflight)
        {
            delivered = std::move(**preflight);
        }
        else
        {
            if (!(co_await objects_.complete(t)))
            {
                co_return serialize_json_rpc_error(-32000, "Multipart completion failed", std::move(request.id), response);
            }

            delivered = co_await run_db([&](sqlite_database& db) { return persist_delivery(db, message, request.id, t.key, t.id); });
        }
    }
    else
    {
        delivered = co_await run_db(
            [&](sqlite_database& db) -> std::expected<send_outcome, sqlite_error>
            {
                return persist_delivery(db,
                                        {user,
                                         params.conversation,
                                         *params.client_id,
                                         params.text.value_or(""),
                                         params.reply_to,
                                         params.forward,
                                         params.markdown,
                                         params.mention_all,
                                         params.bundle ? std::span<const std::int64_t>(*params.bundle) : std::span<const std::int64_t>{},
                                         params.poll_options ? std::span<const std::string>(*params.poll_options) : std::span<const std::string>{},
                                         params.poll_settings.value_or(chat::poll_settings{})},
                                        request.id,
                                        params.object_key.value_or(""));
            });
    }
    if (!delivered)
    {
        if (attaching)
        {
            spdlog::warn("attachment publication failed (SQLite {})", delivered.error().code);
        }
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        co_return delivered.error().code == SQLITE_TOOBIG ? serialize_json_rpc_invalid_params(std::move(request.id), response)
                                                          : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto const* rejected = std::get_if<message_write_rejection>(&*delivered))
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        if (*rejected == message_write_rejection::muted)
        {
            co_return serialize_json_rpc_error(chat::group_speech_muted, "Group speech is muted", std::move(request.id), response);
        }
        co_return *rejected == message_write_rejection::unavailable
            ? serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response)
            : serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto& result = std::get<delivery>(*delivered);
    if (!result.notification.json.empty())
    {
        co_await publish_conversation(
            params.conversation, std::move(result.notification.json), false, result.notification.id, &result.notification.quote);
    }
    response = std::move(result.acknowledgement);
    co_return simdjson::SUCCESS;
}

boost::capy::task<std::expected<std::optional<chat_session::sent_message>, sqlite_error>> chat_session::find_sent_message(std::int64_t client_id)
{
    co_return co_await run_db(
        [=, user = *user_id_](sqlite_database& db) -> std::expected<std::optional<sent_message>, sqlite_error>
        {
            auto snapshot = sqlite_transaction::begin_read(db);
            if (!snapshot)
            {
                return std::unexpected(std::move(snapshot.error()));
            }
            auto query = db.prepare(R"SQL(
            SELECT id,conversation_id,deleted,
                EXISTS(SELECT 1 FROM message_attachments WHERE message_id=m.id),
                EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=m.conversation_id AND user_id=?1)
            FROM messages m WHERE sender_id=?1 AND client_id=?2
        )SQL",
                                    user,
                                    client_id);
            if (!query)
            {
                return std::unexpected(std::move(query.error()));
            }
            auto row = query->step();
            if (!row)
            {
                return std::unexpected(std::move(row.error()));
            }
            if (!*row)
            {
                return std::nullopt;
            }
            sent_message result{query->column_int64(1), query->column_int64(2) != 0, query->column_int64(3) != 0, query->column_int64(4) != 0, {}};
            auto current = read_message(db, user, result.conversation, query->column_int64(0));
            if (!current)
            {
                return std::unexpected(std::move(current.error()));
            }
            if (*current)
            {
                auto value = make_message_payload(**current, true);
                if (simdjson::builder::to_json_string(value).get(result.json))
                {
                    return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize stored message"});
                }
            }
            else if (result.member)
            {
                hidden_receipt receipt{query->column_int64(0), result.conversation, user, client_id};
                if (simdjson::builder::to_json_string(receipt).get(result.json))
                {
                    return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize stored receipt"});
                }
            }
            return result;
        });
}
