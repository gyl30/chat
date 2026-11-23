#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <string_view>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "user_store.hpp"
#include "user_payload.hpp"

namespace
{

struct[[= simdjson::deny_unknown_fields]] search_users_params
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

    auto users = co_await run_db([&, user = *user_id_](sqlite_database& db) { return search_accounts(db, user, params.query); });
    if (!users)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    struct result
    {
        std::vector<user_payload> users;
    };
    result payload;
    payload.users.reserve(users->size());
    for (auto const& user : *users)
    {
        payload.users.emplace_back(user);
    }
    co_return serialize_json_rpc_value(payload, std::move(request.id), response);
}
