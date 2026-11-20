#include <cstdint>
#include <openssl/sha.h>
#include <array>
#include <string>
#include <utility>
#include <string_view>
#include <system_error>

#include <simdjson.h>

#include "chat_session.hpp"
#include "blocking_worker.hpp"
#include "password_hash.hpp"
#include "database.hpp"
#include "user_store.hpp"

namespace
{

constexpr std::string_view kDummyPasswordHash = "$2b$12$N9qo8uLOickgx2ZMRZoMyeIjZAgcfl7p92ldGxad68LJZdL17lhWy";

struct[[= simdjson::deny_unknown_fields]] authenticate_params
{
    std::string username;
    std::string password;
    std::optional<std::string> device_secret;
};

struct authenticate_result
{
    bool authenticated = false;
    std::int64_t user = 0;
    std::int64_t avatar_revision = 0;
    bool has_avatar = false;
};

bool parse_authenticate_params(json_rpc_params& params, authenticate_params& value)
{
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    return params.present && !parser.iterate(params.json).get(document) && !document.get(value) && document.at_end() && !value.username.empty() &&
           !value.password.empty() && value.password.size() <= 72 &&
           (!value.device_secret ||
            (value.device_secret->size() == 64 && value.device_secret->find_first_not_of("0123456789abcdef") == std::string::npos));
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
    if (!parse_authenticate_params(request.params, params))
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    auto account = co_await database_.run([&](sqlite_database& db) { return find_account(db, params.username); });
    if (!account)
    {
        co_return request.id.present ? serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response) : simdjson::SUCCESS;
    }
    std::string_view const password_hash = *account ? std::string_view((*account)->password_hash) : kDummyPasswordHash;
    auto password_matches =
        co_await passwords_.run([&] { return check_password(params.password, password_hash); },
                                [](blocking_worker::unavailable) { return std::make_error_code(std::errc::resource_unavailable_try_again); });
    if (!password_matches)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    bool authenticated = false;
    if (*account && *password_matches)
    {
        auto const user_id = (*account)->user.id;
        bool const first = users_.sessions(user_id).empty();
        users_.add(user_id, *this);
        if (params.device_secret)
        {
            std::array<unsigned char, SHA256_DIGEST_LENGTH> hash;
            SHA256(reinterpret_cast<unsigned char const*>(params.device_secret->data()), params.device_secret->size(), hash.data());
            constexpr std::string_view hex = "0123456789abcdef";
            for (auto c : hash)
            {
                device_.push_back(hex[c >> 4]);
                device_.push_back(hex[c & 15]);
            }
        }
        user_id_ = user_id;
        username_ = params.username;
        authenticated = true;
        if (first)
        {
            co_await publish_presence(true);
        }
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    auto const avatar = authenticated ? (*account)->user.avatar : chat::avatar_state{};
    co_return serialize_json_rpc_value(
        authenticate_result{authenticated, user_id_.value_or(0), avatar.revision, avatar.present}, std::move(request.id), response);
}
