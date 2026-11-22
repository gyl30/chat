#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>
#include <chat/text.hpp>

#include "chat_session.hpp"
#include "database.hpp"
#include "conversation_store.hpp"
#include "timestamp.hpp"

namespace
{
struct group_change
{
    std::int64_t conversation = 0;
    std::int64_t user = 0;
    std::int64_t pinned_message = 0;
    bool admin = false;
    std::int64_t mute_until = 0;
    std::string text;
    std::vector<std::int64_t> members;
};

std::optional<group_change> parse_group_change(json_rpc_request& request, std::int64_t actor)
{
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document))
    {
        return std::nullopt;
    }
    group_change change;
    if (request.method == "set_group_mute")
    {
        struct[[= simdjson::deny_unknown_fields]] mute_params
        {
            std::int64_t conversation = 0;
            std::optional<bool> muted;
        };
        mute_params params;
        if (document.get(params) || !params.muted)
        {
            return std::nullopt;
        }
        change.conversation = params.conversation;
        change.admin = *params.muted;
    }
    else if (request.method == "set_group_member_mute")
    {
        struct[[= simdjson::deny_unknown_fields]] mute_params
        {
            std::int64_t conversation = 0, user = 0;
            std::optional<std::int64_t> seconds;
        };
        mute_params params;
        if (document.get(params) || params.user <= 0 || !params.seconds || *params.seconds < -1 || *params.seconds > 365 * 24 * 60 * 60)
        {
            return std::nullopt;
        }
        change.conversation = params.conversation;
        change.user = params.user;
        change.mute_until = *params.seconds > 0 ? epoch_milliseconds() + *params.seconds * 1000 : *params.seconds;
    }
    else if (request.method == "set_group_admin")
    {
        struct[[= simdjson::deny_unknown_fields]] set_admin_params
        {
            std::int64_t conversation = 0;
            std::int64_t user = 0;
            bool admin = false;
        };
        set_admin_params params;
        if (document.get(params) || params.user <= 0)
        {
            return std::nullopt;
        }
        change.conversation = params.conversation;
        change.user = params.user;
        change.admin = params.admin;
    }
    else if (request.method == "transfer_group_owner" || request.method == "remove_group_member")
    {
        struct[[= simdjson::deny_unknown_fields]] member_action_params
        {
            std::int64_t conversation = 0;
            std::int64_t user = 0;
        };
        member_action_params params;
        if (document.get(params) || params.user <= 0)
        {
            return std::nullopt;
        }
        change.conversation = params.conversation;
        change.user = params.user;
    }
    else if (request.method == "pin_group_message")
    {
        struct[[= simdjson::deny_unknown_fields]] pin_message_params
        {
            std::int64_t conversation = 0;
            std::int64_t message = 0;
        };
        pin_message_params params;
        if (document.get(params) || params.message <= 0)
        {
            return std::nullopt;
        }
        change.conversation = params.conversation;
        change.pinned_message = params.message;
    }
    else if (request.method == "rename_group")
    {
        struct[[= simdjson::deny_unknown_fields]] rename_group_params
        {
            std::int64_t conversation = 0;
            std::string title;
        };
        rename_group_params params;
        if (document.get(params) || !chat::valid_group_title(params.title))
        {
            return std::nullopt;
        }
        change.conversation = params.conversation;
        change.text = std::move(params.title);
    }
    else if (request.method == "set_group_announcement")
    {
        struct[[= simdjson::deny_unknown_fields]] announcement_params
        {
            std::int64_t conversation = 0;
            std::optional<std::string> text;
        };
        announcement_params params;
        if (document.get(params) || !params.text || params.text->size() > 4096 || params.text->find('\0') != std::string::npos)
        {
            return std::nullopt;
        }
        change.conversation = params.conversation;
        if (!chat::text_is_blank(*params.text))
        {
            change.text = std::move(*params.text);
        }
    }
    else if (request.method == "invite_group_members")
    {
        struct[[= simdjson::deny_unknown_fields]] invite_members_params
        {
            std::int64_t conversation = 0;
            std::vector<std::int64_t> members;
        };
        invite_members_params params;
        if (document.get(params))
        {
            return std::nullopt;
        }
        change.conversation = params.conversation;
        change.members = std::move(params.members);
        auto& members = change.members;
        std::sort(members.begin(), members.end());
        if (members.empty() || members.front() <= 0 || std::adjacent_find(members.begin(), members.end()) != members.end() ||
            std::binary_search(members.begin(), members.end(), actor))
        {
            return std::nullopt;
        }
    }
    else
    {
        struct[[= simdjson::deny_unknown_fields]] conversation_action_params
        {
            std::int64_t conversation = 0;
        };
        conversation_action_params params;
        if (document.get(params))
        {
            return std::nullopt;
        }
        change.conversation = params.conversation;
    }
    if (!document.at_end() || change.conversation <= 0)
    {
        return std::nullopt;
    }
    return change;
}

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> apply_group_change(sqlite_database& db,
                                                                                           std::int64_t actor,
                                                                                           std::string_view method,
                                                                                           group_change const& change)
{
    if (method == "set_group_mute" || method == "set_group_member_mute")
    {
        return set_group_speech(db, change.conversation, actor, change.user, change.mute_until, change.admin);
    }
    if (method == "rename_group" || method == "set_group_announcement")
    {
        return set_group_text(
            db, change.conversation, actor, method == "rename_group" ? group_text_field::title : group_text_field::announcement, change.text);
    }
    if (method == "pin_group_message" || method == "unpin_group_message")
    {
        return set_group_pin(db, change.conversation, actor, method == "pin_group_message" ? std::optional(change.pinned_message) : std::nullopt);
    }
    if (method == "invite_group_members")
    {
        return invite_group_members(db, change.conversation, actor, change.members);
    }
    auto action = group_member_action::leave;
    if (method == "set_group_admin")
    {
        action = change.admin ? group_member_action::promote : group_member_action::demote;
    }
    else if (method == "transfer_group_owner")
    {
        action = group_member_action::transfer;
    }
    else if (method == "remove_group_member")
    {
        action = group_member_action::remove;
    }
    return change_group_member(db, change.conversation, actor, action, change.user);
}

simdjson::error_code serialize_group_rejection(group_member_rejection rejected, json_rpc_request& request, std::string& response)
{
    int code = -32000;
    std::string_view message = "Server error";
    switch (rejected)
    {
        case group_member_rejection::unavailable:
            code = -32006;
            message = "Group unavailable";
            break;
        case group_member_rejection::permission_denied:
            code = -32009;
            message = request.method == "leave_group" ? "The owner cannot leave without transferring ownership" : "Group permission denied";
            break;
        case group_member_rejection::invalid_target:
            code = request.method == "pin_group_message" ? -32007 : -32602;
            message = request.method == "pin_group_message" ? "Message unavailable"
                      : request.method == "set_group_admin" ? "The owner cannot be an administrator"
                                                            : "Use leave_group to leave; ownership must be transferred to another administrator";
            break;
        case group_member_rejection::member_not_found:
            code = -32005;
            message = "Member unavailable";
            break;
        case group_member_rejection::contact_required:
            code = -32005;
            message = "Invitees must be your contacts";
            break;
        case group_member_rejection::admin_limit:
            code = -32010;
            message = "At most three administrators are allowed";
            break;
    }
    return serialize_json_rpc_error(code, message, std::move(request.id), response);
}
}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_group_management(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    auto const change = parse_group_change(request, *user_id_);
    if (!change)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    if (request.method == "dissolve_group")
    {
        auto result = co_await database_.run([&, actor = *user_id_](sqlite_database& db) { return dissolve_group(db, change->conversation, actor); });
        if (!result)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (auto const* rejected = std::get_if<group_member_rejection>(&*result))
        {
            co_return serialize_group_rejection(*rejected, request, response);
        }
        auto const& former = std::get<std::vector<std::int64_t>>(*result);
        auto const event = R"({"jsonrpc":"2.0","method":"conversation","params":{"conversation":)" + std::to_string(change->conversation) +
                           R"(,"removed":true,"dissolved":true}})";
        for (auto user : former)
        {
            for (auto* session : users_.sessions(user))
            {
                session->enqueue_message(event);
            }
        }
        co_return serialize_json_rpc_success(former.empty() ? "{\"changed\":false}" : "{\"changed\":true}", std::move(request.id), response);
    }
    auto changed =
        co_await database_.run([&, actor = *user_id_](sqlite_database& db) { return apply_group_change(db, actor, request.method, *change); });
    if (!changed)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto const* rejected = std::get_if<group_member_rejection>(&*changed))
    {
        co_return serialize_group_rejection(*rejected, request, response);
    }
    bool const updated = std::get<bool>(*changed);
    if (updated)
    {
        std::string notification = R"({"jsonrpc":"2.0","method":"conversation","params":{"conversation":)" + std::to_string(change->conversation);
        if (request.method == "leave_group" || request.method == "remove_group_member")
        {
            for (auto* departed : users_.sessions(request.method == "leave_group" ? *user_id_ : change->user))
            {
                departed->enqueue_message(notification + R"(,"removed":true}})");
            }
        }
        co_await publish_conversation(change->conversation, notification + "}}");
    }
    co_return serialize_json_rpc_success(updated ? "{\"changed\":true}" : "{\"changed\":false}", std::move(request.id), response);
}
