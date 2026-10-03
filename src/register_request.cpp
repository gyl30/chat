#include <charconv>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>
#include <system_error>

#include <simdjson.h>
#include <boost/http/bcrypt.hpp>
#include <chat/text.hpp>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

constexpr unsigned kBcryptCost = 12;

struct [[= simdjson::deny_unknown_fields]] register_params
{
    std::string username;
    std::string password;
};

struct register_result
{
    std::int64_t user = 0;
};

simdjson::error_code parse_register_params(json_rpc_params& params, register_params& value)
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

    if (!chat::valid_username(value.username) || value.password.empty() || value.password.size() > 72)
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_register_result(std::int64_t user_id, json_rpc_id id, std::string& response)
{
    register_result result{};
    result.user = user_id;

    std::string result_json;
    auto error = simdjson::builder::to_json_string(result).get(result_json);
    if (error)
    {
        return error;
    }

    return serialize_json_rpc_success(result_json, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_register(json_rpc_request& request, std::string& response)
{
    register_params params{};
    auto params_error = parse_register_params(request.params, params);
    if (params_error)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    boost::http::bcrypt::result password_hash;
    try
    {
        password_hash = co_await boost::http::bcrypt::hash_async(params.password, kBcryptCost, boost::http::bcrypt::version::v2b);
    }
    catch (std::exception const&)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

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
    parameters.emplace_back(std::move(params.username));
    parameters.emplace_back(password_hash.data(), password_hash.size());
    auto query_result = co_await lease.connection().execute_row(
        "INSERT INTO users (username, password_hash) VALUES ($1, $2) "
        "ON CONFLICT (username) DO NOTHING RETURNING id::text",
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
            co_return serialize_json_rpc_error(-32002, "Username already exists", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    if (row->size() != 1)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    std::int64_t user_id = 0;
    auto const& id = row->front();
    auto const [ptr, ec] = std::from_chars(id.data(), id.data() + id.size(), user_id);
    if (ec != std::errc{} || ptr != id.data() + id.size())
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    co_return serialize_register_result(user_id, std::move(request.id), response);
}
