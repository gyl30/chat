#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"
#include "password_hash.hpp"
#include "user_store.hpp"

namespace
{
constexpr std::string_view kDummyPasswordHash = "$2b$12$N9qo8uLOickgx2ZMRZoMyeIjZAgcfl7p92ldGxad68LJZdL17lhWy";

struct[[= simdjson::deny_unknown_fields]] authenticate_params
{
    std::string username;
    std::string password;
    std::optional<std::string> device_secret;
    std::optional<std::string> device_name;
    std::optional<std::string> client_type;
};

struct[[= simdjson::deny_unknown_fields]] resume_params
{
    std::string token;
    std::optional<std::string> device_secret;
};

struct[[= simdjson::deny_unknown_fields]] revoke_params
{
    std::int64_t session = 0;
};

struct[[= simdjson::deny_unknown_fields]] password_params
{
    std::string current_password;
    std::string new_password;
};

struct authenticate_result
{
    bool authenticated = false;
    std::int64_t user = 0;
    std::int64_t avatar_revision = 0;
    bool has_avatar = false;
    std::int64_t session = 0;
    std::string token;
};

template <class T>
bool parse_params(json_rpc_request& request, T& value)
{
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    return request.params.present && !parser.iterate(request.params.json).get(document) && !document.get(value) && document.at_end();
}

auto password_queue_error(blocking_worker::unavailable) { return std::make_error_code(std::errc::resource_unavailable_try_again); }
}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_authenticate(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    auto fail = [&](int code, std::string_view message) { return serialize_json_rpc_error(code, message, std::move(request.id), response); };
    auto invalid = [&] { return serialize_json_rpc_invalid_params(std::move(request.id), response); };
    if (user_id_)
    {
        co_return fail(-32003, "Already authenticated");
    }

    std::expected<std::optional<active_login>, sqlite_error> login;
    std::string installation;
    std::string token;
    if (request.method == "resume_session")
    {
        resume_params params;
        if (!parse_params(request, params) || !valid_login_token(params.token) || (params.device_secret && !valid_login_token(*params.device_secret)))
        {
            co_return invalid();
        }
        installation = params.device_secret ? login_digest(*params.device_secret) : std::string{};
        auto digest = login_digest(params.token);
        login = co_await database_.run([&](sqlite_database& db) { return find_login(db, digest, installation, epoch_milliseconds()); });
    }
    else
    {
        authenticate_params params;
        if (!parse_params(request, params) || params.username.empty() || !valid_password(params.password) ||
            (params.device_secret && !valid_login_token(*params.device_secret)) || (params.device_name && params.device_name->size() > 128) ||
            (params.client_type && params.client_type->size() > 48))
        {
            co_return invalid();
        }

        auto account = co_await database_.run([&](sqlite_database& db) { return find_account(db, params.username); });
        if (!account)
        {
            co_return fail(-32000, "Server error");
        }
        std::string_view const hash = *account ? std::string_view((*account)->password_hash) : kDummyPasswordHash;
        auto matches = co_await passwords_.run([&] { return check_password(params.password, hash); }, password_queue_error);
        if (!matches)
        {
            co_return fail(-32000, "Server error");
        }
        if (!*account || !*matches)
        {
            co_return serialize_json_rpc_value(authenticate_result{}, std::move(request.id), response);
        }
        auto generated = new_login_token();
        if (!generated)
        {
            co_return fail(-32000, "Server error");
        }
        token = std::move(*generated);
        auto digest = login_digest(token);
        installation = params.device_secret ? login_digest(*params.device_secret) : std::string{};
        login = co_await database_.run(
            [&](sqlite_database& db)
            {
                return create_login(db,
                                    (*account)->user,
                                    hash,
                                    digest,
                                    installation,
                                    params.device_name.value_or("Unknown device"),
                                    params.client_type.value_or("CLI"),
                                    epoch_milliseconds());
            });
    }

    if (!login)
    {
        co_return fail(-32000, "Server error");
    }
    if (!*login)
    {
        if (request.method == "resume_session")
        {
            co_return fail(chat::login_session_invalid, "Login session no longer valid");
        }
        co_return serialize_json_rpc_value(authenticate_result{}, std::move(request.id), response);
    }

    auto const& active = **login;
    auto const first = users_.sessions(active.user.id).empty();
    login_id_ = active.id;
    login_expires_ = active.created_at + kLoginLifetimeMs;
    user_id_ = active.user.id;
    username_ = active.user.username;
    device_ = std::move(installation);
    users_.add(*user_id_, *this);

    // A revocation may have committed while the login result was crossing back to the socket.
    auto checked = co_await run_db([id = login_id_](sqlite_database& db) { return touch_login(db, id, epoch_milliseconds()); });
    if (!checked || revoked_)
    {
        invalidate_login();
        co_return simdjson::SUCCESS;
    }
    next_login_touch_ = epoch_milliseconds() + kLoginActivityIntervalMs;
    if (first)
    {
        co_await publish_presence(true);
    }
    if (!token.empty() && !revoked_)
    {
        publish_login_changes({});
    }

    co_return serialize_json_rpc_value(
        authenticate_result{true, active.user.id, active.user.avatar.revision, active.user.avatar.present, active.id, std::move(token)},
        std::move(request.id),
        response);
}

boost::capy::task<simdjson::error_code> chat_session::handle_login_management(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    auto fail = [&](int code, std::string_view message) { return serialize_json_rpc_error(code, message, std::move(request.id), response); };
    if (!user_id_)
    {
        co_return fail(-32001, "Authentication required");
    }
    if (request.method == "list_login_sessions")
    {
        auto sessions = co_await run_db([this](sqlite_database& db) { return list_logins(db, *user_id_, login_id_, epoch_milliseconds()); });
        if (!sessions)
        {
            co_return fail(-32000, "Server error");
        }
        struct result
        {
            std::vector<chat::login_session> sessions;
        };
        co_return serialize_json_rpc_value(result{std::move(*sessions)}, std::move(request.id), response);
    }
    if (request.method == "change_password")
    {
        password_params params;
        if (!parse_params(request, params) || !valid_password(params.current_password) || !valid_password(params.new_password))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        auto account = co_await run_db([this](sqlite_database& db) { return find_account(db, username_); });
        if (!account || !*account)
        {
            co_return fail(-32000, "Server error");
        }
        auto const& previous = (*account)->password_hash;
        auto matches = co_await passwords_.run([&] { return check_password(params.current_password, previous); }, password_queue_error);
        if (!matches)
        {
            co_return fail(-32000, "Server error");
        }
        if (!*matches)
        {
            co_return fail(chat::current_password_incorrect, "Current password is incorrect");
        }
        auto replacement = co_await passwords_.run([&] { return hash_password(params.new_password); }, password_queue_error);
        if (!replacement)
        {
            co_return fail(-32000, "Server error");
        }
        auto changed = co_await run_db([&](sqlite_database& db)
                                       { return change_login_password(db, *user_id_, login_id_, previous, *replacement, epoch_milliseconds()); });
        if (!changed)
        {
            co_return fail(-32000, "Server error");
        }
        if (!*changed)
        {
            co_return fail(chat::current_password_incorrect, "Current password is incorrect");
        }
        publish_login_changes(**changed);
        co_return serialize_json_rpc_success("{\"changed\":true}", std::move(request.id), response);
    }

    std::optional<std::int64_t> target;
    if (request.method == "revoke_login_session")
    {
        revoke_params params;
        if (!parse_params(request, params) || params.session <= 0 || params.session == login_id_)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        target = params.session;
    }
    else if (request.method == "logout_session")
    {
        target = login_id_;
    }
    auto revoked = co_await run_db([&](sqlite_database& db) { return revoke_logins(db, *user_id_, login_id_, target, epoch_milliseconds()); });
    if (!revoked)
    {
        co_return fail(-32000, "Server error");
    }
    if (!revoked->empty())
    {
        publish_login_changes(*revoked);
    }
    co_return serialize_json_rpc_success(revoked->empty() ? "{\"changed\":false}" : "{\"changed\":true}", std::move(request.id), response);
}
