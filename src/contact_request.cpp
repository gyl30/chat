#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <string_view>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

struct [[= simdjson::deny_unknown_fields]] contact_params
{
    std::int64_t user = 0;
};

simdjson::error_code parse_contact_params(json_rpc_params& params, contact_params& value)
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

    if (value.user <= 0)
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_add_contact_result(std::string_view user, json_rpc_id id, std::string& response)
{
    std::string result = R"({"user":)";
    result.append(user);
    result.push_back('}');
    return serialize_json_rpc_success(result, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_contact_change(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    contact_params params{};
    auto params_error = parse_contact_params(request.params, params);
    if (params_error || params.user == *user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    std::string user_json;
    {
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }

        std::vector<std::string> parameters;
        parameters.emplace_back(std::to_string(*user_id_));
        parameters.emplace_back(std::to_string(params.user));
        if (request.method == "remove_contact")
        {
            auto remove_result = co_await lease.connection().execute_scalar(
                "WITH removed AS (DELETE FROM contacts WHERE owner_id=$1::bigint AND contact_id=$2::bigint "
                "RETURNING 1) SELECT (count(*)>0)::text FROM removed", std::move(parameters));
            auto& [ec, removed] = remove_result;
            if (ec)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return serialize_json_rpc_success("{\"removed\":" + removed + "}", std::move(request.id), response);
        }
        auto query_result = co_await lease.connection().execute_scalar(
            "WITH target AS ("
            "SELECT id, username,avatar_revision,EXISTS(SELECT 1 FROM user_avatars WHERE user_id=users.id) AS has_avatar "
            "FROM users WHERE id = $2::bigint AND id <> $1::bigint"
            "), inserted AS ("
            "INSERT INTO contacts (owner_id, contact_id) "
            "SELECT $1::bigint, id FROM target "
            "ON CONFLICT DO NOTHING "
            "RETURNING contact_id"
            ") "
            "SELECT COALESCE((SELECT row_to_json(target)::text FROM target), '')",
            std::move(parameters));
        auto& [query_ec, query_json] = query_result;
        if (query_ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (query_json.empty())
        {
            co_return serialize_json_rpc_error(-32004, "User not found", std::move(request.id), response);
        }
        user_json = std::move(query_json);
    }

    co_return serialize_add_contact_result(user_json, std::move(request.id), response);
}
