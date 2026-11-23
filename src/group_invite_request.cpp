#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <openssl/rand.h>
#include <simdjson.h>

#include <chat/invite.hpp>

#include "chat_session.hpp"
#include "invite_attempts.hpp"
#include "database.hpp"
#include "group_access_store.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_group_invite(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    auto const joining = request.method == "join_group";
    auto const creating = request.method == "create_group_invite";
    auto const revoking = request.method == "revoke_group_invite";
    std::int64_t conversation = 0;
    std::string token;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    simdjson::error_code parse_error;
    if (joining)
    {
        struct[[= simdjson::deny_unknown_fields]] join_group_params
        {
            std::string token;
        };
        join_group_params params;
        parse_error = document.get(params);
        token = std::move(params.token);
    }
    else
    {
        struct[[= simdjson::deny_unknown_fields]] group_invite_params
        {
            std::int64_t conversation = 0;
        };
        group_invite_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
    }
    if (parse_error || !document.at_end() || (!joining && conversation <= 0) || (joining && !chat::valid_invite_token(token)))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    if (joining && invite_attempts_.blocked(*user_id_))
    {
        co_return serialize_json_rpc_error(-32015, "Too many invite attempts", std::move(request.id), response);
    }
    auto const user = *user_id_;
    auto const notification = [](std::int64_t id)
    { return R"({"jsonrpc":"2.0","method":"conversation","params":{"conversation":)" + std::to_string(id) + "}}"; };
    if (joining)
    {
        auto joined = co_await run_db([&](sqlite_database& db) { return join_with_invite(db, user, token); });
        if (!joined)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!*joined)
        {
            invite_attempts_.record_failure(user);
            co_return serialize_json_rpc_error(-32014, "Invite unavailable", std::move(request.id), response);
        }
        auto& group = **joined;
        bool const pending = group.result.state == chat::group_join_state::pending;
        bool const added = group.result.state == chat::group_join_state::joined;
        if (group.request_changed)
        {
            co_await publish_join_request(group.result.conversation, user, pending ? "pending" : "accepted");
        }
        if (added)
        {
            co_await publish_conversation(group.result.conversation, notification(group.result.conversation));
        }
        struct result
        {
            std::int64_t conversation;
            std::string title;
            std::uint64_t member_count;
            std::string_view state;
        };
        co_return serialize_json_rpc_value(result{group.result.conversation,
                                                  std::move(group.result.title),
                                                  group.result.member_count,
                                                  pending ? "pending"
                                                  : added ? "joined"
                                                          : "member"},
                                           std::move(request.id),
                                           response);
    }
    auto const action = creating ? invite_action::create : revoking ? invite_action::revoke : invite_action::inspect;
    auto changed = co_await run_db([=](sqlite_database& db) { return change_group_invite(db, conversation, user, action); });
    if (!changed)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto const* rejected = std::get_if<group_member_rejection>(&*changed))
    {
        bool const unavailable = *rejected == group_member_rejection::unavailable;
        co_return serialize_json_rpc_error(
            unavailable ? -32006 : -32009, unavailable ? "Group unavailable" : "Group permission denied", std::move(request.id), response);
    }
    auto& invite = std::get<group_invite_change>(*changed);
    if (invite.changed)
    {
        co_await publish_conversation(conversation, notification(conversation));
    }
    struct result
    {
        std::optional<std::string> token;
    };
    co_return serialize_json_rpc_value(result{std::move(invite.token)}, std::move(request.id), response);
}
