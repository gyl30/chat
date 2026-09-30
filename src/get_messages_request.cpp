#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

struct [[= simdjson::deny_unknown_fields]] get_messages_params
{
    std::int64_t user = 0;
    std::optional<std::int64_t> before;
};

simdjson::error_code parse_get_messages_params(json_rpc_params& params, get_messages_params& value)
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

    if (value.user <= 0 || (value.before && *value.before <= 0))
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_get_messages_result(std::string_view messages, json_rpc_id id, std::string& response)
{
    std::string result = R"({"messages":)";
    result.append(messages);
    result.push_back('}');
    return serialize_json_rpc_success(result, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_messages(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    get_messages_params params{};
    auto params_error = parse_get_messages_params(request.params, params);
    if (params_error)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    std::string messages_json;
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
        parameters.emplace_back(std::to_string(params.before.value_or(std::numeric_limits<std::int64_t>::max())));

        auto query_result = co_await lease.connection().execute_scalar(
            "SELECT COALESCE("
            "array_to_json(array_agg(row_to_json(page) ORDER BY id ASC)), "
            "'[]'::json"
            ")::text "
            "FROM ("
            "SELECT id, sender_id AS \"from\", "
            "(extract(epoch from created_at) * 1000)::bigint AS timestamp, body AS text FROM messages "
            "WHERE ((sender_id = $1::bigint AND recipient_id = $2::bigint) "
            "OR (sender_id = $2::bigint AND recipient_id = $1::bigint)) "
            "AND id < $3::bigint "
            "ORDER BY id DESC LIMIT 50"
            ") AS page",
            std::move(parameters));
        auto& [query_ec, query_json] = query_result;
        if (query_ec)
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        messages_json = std::move(query_json);
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    co_return serialize_get_messages_result(messages_json, std::move(request.id), response);
}
