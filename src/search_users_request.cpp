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

struct [[= simdjson::deny_unknown_fields]] search_users_params
{
    std::string query;
};

simdjson::error_code parse_search_users_params(json_rpc_params& params, search_users_params& value)
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

    if (value.query.empty())
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_search_users_result(std::string_view users, json_rpc_id id, std::string& response)
{
    std::string result = R"({"users":)";
    result.append(users);
    result.push_back('}');
    return serialize_json_rpc_success(result, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_search_users(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    search_users_params params{};
    auto params_error = parse_search_users_params(request.params, params);
    if (params_error)
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

    std::string users_json;
    {
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }

        std::vector<std::string> parameters;
        parameters.emplace_back(std::to_string(*user_id_));
        parameters.emplace_back(std::move(params.query));
        auto query_result = co_await lease.connection().execute_scalar(
            "SELECT COALESCE("
            "array_to_json(array_agg(row_to_json(found) ORDER BY found.username, found.id)), "
            "'[]'::json"
            ")::text "
            "FROM ("
            "SELECT u.id, u.username,u.avatar_revision,EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id) AS has_avatar FROM users u "
            "WHERE u.id <> $1::bigint "
            "AND starts_with(lower(u.username), lower($2)) "
            "AND NOT EXISTS ("
            "SELECT 1 FROM contacts c "
            "WHERE c.owner_id = $1::bigint AND c.contact_id = u.id"
            ") "
            "ORDER BY u.username, u.id LIMIT 20"
            ") AS found",
            std::move(parameters));
        auto& [query_ec, query_json] = query_result;
        if (query_ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        users_json = std::move(query_json);
    }

    co_return serialize_search_users_result(users_json, std::move(request.id), response);
}
