#include <charconv>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <string_view>
#include <system_error>

#include <simdjson.h>
#include <boost/http/bcrypt.hpp>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

constexpr std::string_view kDummyPasswordHash = "$2b$12$N9qo8uLOickgx2ZMRZoMyeIjZAgcfl7p92ldGxad68LJZdL17lhWy";

struct [[= simdjson::deny_unknown_fields]] authenticate_params
{
    std::string username;
    std::string password;
};

struct authenticate_result
{
    bool authenticated = false;
};

simdjson::error_code parse_authenticate_params(json_rpc_params& params, authenticate_params& value)
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

    if (value.username.empty() || value.password.empty() || value.password.size() > 72)
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_authenticate_result(bool authenticated, json_rpc_id id, std::string& response)
{
    authenticate_result result{};
    result.authenticated = authenticated;

    std::string result_json;
    auto error = simdjson::builder::to_json_string(result).get(result_json);
    if (error)
    {
        return error;
    }

    return serialize_json_rpc_success(result_json, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_authenticate(json_rpc_request& request, std::string& response)
{
    if (user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32003, "Already authenticated", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    authenticate_params params{};
    auto params_error = parse_authenticate_params(request.params, params);
    if (params_error)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    std::optional<std::vector<std::string>> row;
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

        auto query_result = co_await lease.connection().execute_row(
            "SELECT id::text, password_hash FROM users WHERE username = $1", {params.username});
        auto& [query_ec, query_row] = query_result;
        if (query_ec)
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        row = std::move(query_row);
    }

    std::string_view password_hash = kDummyPasswordHash;
    if (row)
    {
        if (row->size() != 2)
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }
        password_hash = (*row)[1];
    }

    bool password_matches = false;
    try
    {
        password_matches = co_await boost::http::bcrypt::compare_async(params.password, password_hash);
    }
    catch (std::exception const&)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    bool authenticated = false;
    if (row && password_matches)
    {
        std::int64_t user_id = 0;
        auto const& id = (*row)[0];
        auto const [ptr, ec] = std::from_chars(id.data(), id.data() + id.size(), user_id);
        if (ec != std::errc{} || ptr != id.data() + id.size())
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }

        if (!users_.add(user_id, *this))
        {
            if (request.id.present)
            {
                co_return serialize_json_rpc_error(-32004, "User already online", std::move(request.id), response);
            }
            co_return simdjson::SUCCESS;
        }

        user_id_ = user_id;
        authenticated = true;
        co_await publish_presence(true);
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    co_return serialize_authenticate_result(authenticated, std::move(request.id), response);
}
