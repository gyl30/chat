#include <cstdint>
#include <string>
#include <utility>
#include <system_error>

#include <simdjson.h>
#include <chat/text.hpp>

#include "chat_session.hpp"
#include "blocking_worker.hpp"
#include "password_hash.hpp"
#include "database.hpp"
#include "user_store.hpp"

namespace
{

struct[[= simdjson::deny_unknown_fields]] register_params
{
    std::string username;
    std::string password;
};

struct register_result
{
    std::int64_t user = 0;
};

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_register(json_rpc_request& request, std::string& response)
{
    register_params params{};
    if (!request.params.read(params) || !chat::valid_username(params.username) || !valid_password(params.password))
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    auto password_hash =
        co_await passwords_.run([&] { return hash_password(params.password); },
                                [](blocking_worker::unavailable) { return std::make_error_code(std::errc::resource_unavailable_try_again); });
    if (!password_hash)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    auto created = co_await database_.run([&](sqlite_database& db) { return register_account(db, params.username, *password_hash); });
    if (!created || !*created)
    {
        if (request.id.present)
        {
            co_return created ? serialize_json_rpc_error(-32002, "Username already exists", std::move(request.id), response)
                              : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    co_return serialize_json_rpc_value(register_result{**created}, std::move(request.id), response);
}
