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
#include "pg_connection_pool.hpp"

namespace
{

constexpr std::size_t kMaxMessageSize = 64 * 1024;

struct [[= simdjson::deny_unknown_fields]] send_message_params
{
    std::int64_t conversation = 0;
    std::string text;
    std::optional<std::int64_t> reply_to;
    std::optional<std::int64_t> upload;
};

struct send_message_result
{
    std::int64_t message = 0;
    std::int64_t timestamp = 0;
    bool realtime = false;
    std::optional<quoted_message_payload> reply;
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

simdjson::error_code serialize_send_message_result(std::int64_t message, std::int64_t timestamp, bool realtime,
                                                   std::optional<quoted_message_payload> reply, json_rpc_id id,
                                                   std::string& response)
{
    send_message_result result{};
    result.message = message;
    result.timestamp = timestamp;
    result.realtime = realtime;
    result.reply = std::move(reply);

    std::string result_json;
    auto error = simdjson::builder::to_json_string(result).get(result_json);
    if (error)
    {
        return error;
    }

    return serialize_json_rpc_success(result_json, std::move(id), response);
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
        : (params.upload || params.conversation <= 0 || params.text.empty() || params.text.find('\0') != std::string::npos)))
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }
    if (attaching)
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        if (!upload_ || upload_->id != *params.upload ||
            upload_->content.size() != static_cast<std::size_t>(upload_->size))
        {
            co_return serialize_json_rpc_error(-32008, "Attachment upload incomplete", std::move(request.id), response);
        }
        params.conversation = upload_->conversation;
        params.text = upload_->filename;
    }

    message_notification notification{};
    notification.params.id = std::numeric_limits<std::int64_t>::max();
    notification.params.conversation = params.conversation;
    notification.params.from = *user_id_;
    notification.params.username = username_;
    notification.params.timestamp = std::numeric_limits<std::int64_t>::max();
    notification.params.text = std::move(params.text);
    if (attaching)
    {
        std::string media_type = "application/octet-stream";
        std::string_view bytes(upload_->content);
        if (bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)))
        {
            media_type = "image/png";
        }
        else if (bytes.starts_with(std::string_view("\xff\xd8\xff", 3)))
        {
            media_type = "image/jpeg";
        }
        notification.params.attachment = chat::attachment_info{upload_->filename, std::move(media_type), upload_->size};
    }

    if (params.reply_to)
    {
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto query_result = co_await lease.connection().execute_row(
            "SELECT r.id::text,r.sender_id::text,u.username,left(r.body,160),COALESCE(((extract(epoch FROM "
            "r.edited_at)*1000)::bigint)::text,''),r.deleted::text FROM messages r "
            "JOIN users u ON u.id=r.sender_id JOIN conversation_members own ON own.conversation_id=r.conversation_id "
            "WHERE r.id=$3::bigint AND r.conversation_id=$2::bigint AND own.user_id=$1::bigint",
            {std::to_string(*user_id_), std::to_string(params.conversation), std::to_string(*params.reply_to)});
        auto& [ec, row] = query_result;
        if (ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!row)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        notification.params.reply = quoted_message_payload{
            std::stoll(row->at(0)),
            std::stoll(row->at(1)),
            row->at(2),
            row->at(3),
            row->at(4).empty() ? std::nullopt : std::optional<std::int64_t>(std::stoll(row->at(4))),
            row->at(5) == "true"};
    }

    std::string notification_json;
    auto error = simdjson::builder::to_json_string(notification).get(notification_json);
    if (error)
    {
        co_return error;
    }
    if (notification_json.size() > kMaxMessageSize)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    std::string id_text;
    std::string timestamp_text;
    {
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }

        std::vector<std::string> parameters;
        parameters.emplace_back(std::to_string(*user_id_));
        parameters.emplace_back(std::to_string(params.conversation));
        parameters.emplace_back(notification.params.text);
        parameters.emplace_back(params.reply_to ? std::to_string(*params.reply_to) : "");
        parameters.emplace_back(attaching ? upload_->filename : "");
        parameters.emplace_back(attaching ? notification.params.attachment->media_type : "");
        parameters.emplace_back(attaching ? std::to_string(upload_->size) : "0");
        parameters.emplace_back(attaching ? chat::detail::encode_base64(upload_->content) : "");
        auto query_result = co_await lease.connection().execute_row(
            "WITH locked AS (UPDATE conversations SET activity=(extract(epoch FROM clock_timestamp())*1000)::bigint "
            "WHERE id=$2::bigint AND EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=$2::bigint AND "
            "user_id=$1::bigint) RETURNING id), "
            "inserted AS (INSERT INTO messages(sender_id,conversation_id,body,reply_to_id) SELECT "
            "$1::bigint,id,$3,NULLIF($4,'')::bigint FROM locked "
            "RETURNING id,created_at), "
            "attached AS (INSERT INTO message_attachments(message_id,filename,media_type,size,data) "
            "SELECT id,$5,$6,$7::bigint,decode($8,'base64') FROM inserted WHERE $5<>'' RETURNING message_id) "
            "SELECT inserted.id::text,((extract(epoch FROM "
            "created_at)*1000)::bigint)::text,u.username "
            "FROM inserted JOIN users u ON u.id=$1::bigint",
            std::move(parameters));
        auto& [query_ec, row] = query_result;
        if (query_ec)
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        if (!row)
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        if (row->size() != 3)
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        id_text = std::move(row->at(0));
        timestamp_text = std::move(row->at(1));
        notification.params.username = std::move(row->at(2));
    }
    if (attaching)
    {
        upload_.reset();
    }

    auto const* first = id_text.data();
    auto const* last = first + id_text.size();
    auto [end, parse_error] = std::from_chars(first, last, notification.params.id);
    if (parse_error != std::errc{} || end != last)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    first = timestamp_text.data();
    last = first + timestamp_text.size();
    auto [timestamp_end, timestamp_error] = std::from_chars(first, last, notification.params.timestamp);
    if (timestamp_error != std::errc{} || timestamp_end != last)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    notification_json.clear();
    error = simdjson::builder::to_json_string(notification).get(notification_json);
    if (error)
    {
        co_return error;
    }

    auto const realtime = co_await publish_conversation(params.conversation, std::move(notification_json));

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (attaching)
    {
        std::string payload;
        error = simdjson::builder::to_json_string(notification.params).get(payload);
        if (error)
        {
            co_return error;
        }
        co_return serialize_json_rpc_success(payload, std::move(request.id), response);
    }

    co_return serialize_send_message_result(notification.params.id, notification.params.timestamp, realtime,
                                            std::move(notification.params.reply), std::move(request.id), response);
}
