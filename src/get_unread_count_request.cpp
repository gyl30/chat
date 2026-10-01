#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

struct [[= simdjson::deny_unknown_fields]] get_unread_count_params
{
    std::int64_t conversation = 0;
};

simdjson::error_code parse_get_unread_count_params(json_rpc_params& params, get_unread_count_params& value)
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

    if (value.conversation <= 0)
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_get_unread_count_result(std::string_view count, json_rpc_id id, std::string& response)
{
    std::string result = R"({"count":)";
    result.append(count);
    result.push_back('}');
    return serialize_json_rpc_success(result, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_unread_count(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    get_unread_count_params params{};
    auto params_error = parse_get_unread_count_params(request.params, params);
    if (params_error)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    std::string count;
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

        auto query_result = co_await lease.connection().execute_row(
            "SELECT (SELECT count(*) FROM messages m WHERE m.conversation_id=c.id "
            "AND m.id>GREATEST(own.last_read_message_id,own.joined_message_id) AND NOT m.deleted "
            "AND (m.sender_id<>$1::bigint OR c.direct_user_low=c.direct_user_high))::text "
            "FROM conversation_members own JOIN conversations c ON c.id=own.conversation_id "
            "WHERE own.user_id=$1::bigint AND own.conversation_id=$2::bigint",
            std::move(parameters));
        auto& [query_ec, query_count] = query_result;
        if (query_ec)
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        if (!query_count)
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        count = std::move(query_count->front());
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    co_return serialize_get_unread_count_result(count, std::move(request.id), response);
}
