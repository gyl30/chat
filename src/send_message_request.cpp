#include <charconv>
#include <cstdint>
#include <limits>
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
    std::int64_t user = 0;
    std::string text;
};

struct send_message_result
{
    std::int64_t message = 0;
    std::int64_t timestamp = 0;
    bool realtime = false;
};

struct message_params
{
    std::int64_t id = 0;
    std::int64_t from = 0;
    std::int64_t timestamp = 0;
    std::string text;
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

    if (value.user <= 0 || value.text.empty())
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_send_message_result(
    std::int64_t message, std::int64_t timestamp, bool realtime, json_rpc_id id, std::string& response)
{
    send_message_result result{};
    result.message = message;
    result.timestamp = timestamp;
    result.realtime = realtime;

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
    notification.params.from = *user_id_;
    notification.params.timestamp = std::numeric_limits<std::int64_t>::max();
    notification.params.text = std::move(params.text);

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
        parameters.emplace_back(std::to_string(params.user));
        parameters.emplace_back(notification.params.text);
        auto query_result = co_await lease.connection().execute_row(
            "INSERT INTO messages (sender_id, recipient_id, body) "
            "SELECT $1::bigint, id, $3 FROM users WHERE id = $2::bigint "
            "RETURNING id::text, ((extract(epoch from created_at) * 1000)::bigint)::text",
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
                co_return serialize_json_rpc_error(-32005, "User unavailable", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        if (row->size() != 2)
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        id_text = std::move(row->at(0));
        timestamp_text = std::move(row->at(1));
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

    auto* recipient = users_.find(params.user);
    auto const realtime = recipient && recipient->enqueue_message(std::move(notification_json));

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    co_return serialize_send_message_result(
        notification.params.id, notification.params.timestamp, realtime, std::move(request.id), response);
}
