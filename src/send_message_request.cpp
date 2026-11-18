#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <simdjson.h>
#include <chat/detail/base64.hpp>

#include "chat_session.hpp"
#include "message_payload.hpp"
#include "attachment_io.hpp"
#include "database.hpp"
#include "message_history.hpp"
#include "message_store.hpp"
#include <sqlite3.h>

namespace
{

constexpr std::size_t kMaxMessageSize = 64 * 1024;

struct [[= simdjson::deny_unknown_fields]] send_message_params
{
    std::int64_t conversation = 0;
    std::string text;
    std::optional<std::int64_t> reply_to;
    std::optional<std::int64_t> upload;
    std::optional<std::int64_t> client_id;
};

struct message_notification
{
    std::string jsonrpc = "2.0";
    std::string method = "message";
    message_payload params;
};

simdjson::error_code parse_send_message_params(json_rpc_params& params, send_message_params& value, bool attaching)
{
    if (!params.present)
    {
        return simdjson::NO_SUCH_FIELD;
    }

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    auto error = parser.iterate(params.json).get(document);
    if (error)
    {
        return error;
    }

    if (attaching)
    {
        struct [[= simdjson::deny_unknown_fields]] finish_params
        {
            std::int64_t upload = 0;
            std::optional<std::int64_t> reply_to;
        };
        finish_params finish;
        error = document.get(finish);
        value.upload = finish.upload;
        value.reply_to = finish.reply_to;
    }
    else
    {
        error = document.get(value);
    }
    if (error)
    {
        return error;
    }

    if (!document.at_end())
    {
        return simdjson::TRAILING_CONTENT;
    }

    if (value.reply_to && *value.reply_to <= 0)
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

struct delivery
{
    std::string acknowledgement;
    std::string notification;
};

using send_outcome = std::variant<delivery, message_write_rejection>;

std::expected<send_outcome, sqlite_error> format_delivery(sqlite_database& db, std::int64_t user,
    std::int64_t conversation, message_write_outcome const& outcome)
{
    if (auto const* rejected = std::get_if<message_write_rejection>(&outcome))
    {
        return *rejected;
    }
    auto const& sent = std::get<message_write>(outcome);
    auto message = read_message(db, user, conversation, sent.id);
    if (!message)
    {
        return std::unexpected(std::move(message.error()));
    }
    if (!*message)
    {
        return message_write_rejection::unavailable;
    }
    delivery result;
    auto value = make_message_payload(**message, true);
    if (simdjson::builder::to_json_string(value).get(result.acknowledgement))
    {
        return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize message acknowledgement"});
    }
    if (sent.inserted)
    {
        value.client_id.reset();
        if (simdjson::builder::to_json_string(message_notification{"2.0", "message", std::move(value)}).get(result.notification))
        {
            return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize message notification"});
        }
        if (result.notification.size() > kMaxMessageSize)
        {
            return std::unexpected(sqlite_error{SQLITE_TOOBIG, "message exceeds the frame limit"});
        }
    }
    return result;
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
    auto params_error = parse_send_message_params(request.params, params, attaching);
    if (params_error || (attaching ? (!params.upload || *params.upload <= 0 || params.conversation != 0 || !params.text.empty())
        : (params.upload || params.conversation <= 0 || params.text.empty() || params.text.find('\0') != std::string::npos ||
           !params.client_id || *params.client_id <= 0)))
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }
    auto const user = *user_id_;
    auto send_text = [&](sqlite_database& db) -> std::expected<send_outcome, sqlite_error> {
        auto tx = sqlite_transaction::begin_immediate(db);
        if (!tx)
        {
            return std::unexpected(std::move(tx.error()));
        }
        auto result = insert_message(db, {user, params.conversation, *params.client_id, params.text, params.reply_to});
        if (!result)
        {
            return std::unexpected(std::move(result.error()));
        }
        auto formatted = format_delivery(db, user, params.conversation, *result);
        if (!formatted)
        {
            return formatted;
        }
        if (auto error = tx->commit(); error)
        {
            return std::unexpected(std::move(error));
        }
        return formatted;
    };

    std::expected<send_outcome, sqlite_error> delivered;
    if (attaching)
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        if (!upload_ || upload_->id != *params.upload || upload_->content.size() != static_cast<std::size_t>(upload_->size))
        {
            co_return serialize_json_rpc_error(-32008, "Attachment upload incomplete", std::move(request.id), response);
        }
        params.conversation = upload_->conversation;
        std::string media_type = "application/octet-stream";
        if (upload_->content.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)))
        {
            media_type = "image/png";
        }
        else if (upload_->content.starts_with(std::string_view("\xff\xd8\xff", 3)))
        {
            media_type = "image/jpeg";
        }
        completed_attachment_upload contents{user, params.conversation, upload_->client_id, params.reply_to,
            std::move(upload_->filename), std::move(media_type), std::move(upload_->content)};
        upload_.reset();
        auto published = co_await publish_attachment(database_, objects_, std::move(contents));
        if (!published)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        delivered = co_await database_.run([&](sqlite_database& db) {
            auto tx = sqlite_transaction::begin_read(db);
            if (!tx)
            {
                return std::expected<send_outcome, sqlite_error>{std::unexpected(std::move(tx.error()))};
            }
            return format_delivery(db, user, params.conversation, *published);
        });
    }
    else
    {
        delivered = co_await database_.run(send_text);
    }
    if (!delivered)
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        co_return delivered.error().code == SQLITE_TOOBIG
            ? serialize_json_rpc_invalid_params(std::move(request.id), response)
            : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto const* rejected = std::get_if<message_write_rejection>(&*delivered))
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        co_return *rejected == message_write_rejection::unavailable
            ? serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response)
            : serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto& result = std::get<delivery>(*delivered);
    if (!result.notification.empty())
    {
        co_await publish_conversation(params.conversation, std::move(result.notification));
    }
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    co_return serialize_json_rpc_success(result.acknowledgement, std::move(request.id), response);
}

boost::capy::task<std::expected<std::optional<chat_session::sent_message>, sqlite_error>>
chat_session::find_sent_message(std::int64_t client_id)
{
    co_return co_await database_.run([=, user=*user_id_](sqlite_database& db)
        -> std::expected<std::optional<sent_message>, sqlite_error> {
        auto snapshot = sqlite_transaction::begin_read(db);
        if (!snapshot)
        {
            return std::unexpected(std::move(snapshot.error()));
        }
        auto query = db.prepare(R"SQL(
            SELECT id,conversation_id,deleted,
                EXISTS(SELECT 1 FROM message_attachments WHERE message_id=m.id)
            FROM messages m WHERE sender_id=?1 AND client_id=?2
        )SQL", user, client_id);
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
        sent_message result{query->column_int64(1), query->column_int64(2) != 0, query->column_int64(3) != 0, false, {}};
        auto current = read_message(db, user, result.conversation, query->column_int64(0));
        if (!current)
        {
            return std::unexpected(std::move(current.error()));
        }
        if (*current)
        {
            result.member = true;
            auto value = make_message_payload(**current, true);
            if (simdjson::builder::to_json_string(value).get(result.json))
            {
                return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize stored message"});
            }
        }
        return result;
    });
}
