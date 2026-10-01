#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

constexpr std::size_t kMaxMessageSize = 64 * 1024;

struct [[= simdjson::deny_unknown_fields]] send_message_params
{
    std::int64_t conversation = 0;
    std::string text;
    std::optional<std::int64_t> reply_to;
};

struct quoted_message
{
    std::int64_t id = 0;
    std::int64_t from = 0;
    std::string username;
    std::string text;
};

struct send_message_result
{
    std::int64_t message = 0;
    std::int64_t timestamp = 0;
    bool realtime = false;
    std::optional<quoted_message> reply;
};

struct message_params
{
    std::int64_t id = 0;
    std::int64_t conversation = 0;
    std::int64_t from = 0;
    std::string username;
    std::int64_t timestamp = 0;
    std::string text;
    std::optional<quoted_message> reply;
};

struct message_notification
{
    std::string jsonrpc = "2.0";
    std::string method = "message";
    message_params params;
};

simdjson::error_code parse_send_message_params(json_rpc_params& params, send_message_params& value)
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

    error = document.get(value);
    if (error)
    {
        return error;
    }

    if (!document.at_end())
    {
        return simdjson::TRAILING_CONTENT;
    }

    if (value.conversation <= 0 || value.text.empty() || (value.reply_to && *value.reply_to <= 0))
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_send_message_result(std::int64_t message, std::int64_t timestamp, bool realtime,
                                                   std::optional<quoted_message> reply, json_rpc_id id,
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
    auto params_error = parse_send_message_params(request.params, params);
    if (params_error)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    message_notification notification{};
    notification.params.id = std::numeric_limits<std::int64_t>::max();
    notification.params.conversation = params.conversation;
    notification.params.from = *user_id_;
    notification.params.username = username_;
    notification.params.timestamp = std::numeric_limits<std::int64_t>::max();
    notification.params.text = std::move(params.text);

    if (params.reply_to)
    {
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto query_result = co_await lease.connection().execute_row(
            "SELECT r.id::text,r.sender_id::text,u.username,left(r.body,160) FROM messages r "
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
        notification.params.reply =
            quoted_message{std::stoll(row->at(0)), std::stoll(row->at(1)), row->at(2), row->at(3)};
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
        auto query_result = co_await lease.connection().execute_row(
            "WITH locked AS (UPDATE conversations SET activity=(extract(epoch FROM clock_timestamp())*1000)::bigint "
            "WHERE id=$2::bigint AND EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=$2::bigint AND "
            "user_id=$1::bigint) RETURNING id), "
            "inserted AS (INSERT INTO messages(sender_id,conversation_id,body,reply_to_id) SELECT "
            "$1::bigint,id,$3,NULLIF($4,'')::bigint FROM locked "
            "RETURNING id,created_at) SELECT inserted.id::text,((extract(epoch FROM "
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

    co_return serialize_send_message_result(notification.params.id, notification.params.timestamp, realtime,
                                            std::move(notification.params.reply), std::move(request.id), response);
}
