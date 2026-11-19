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
    auto const setting_admin = request.method == "set_group_admin";
    auto const renaming = request.method == "rename_group";
    auto const inviting = request.method == "invite_group_members";
    auto const transferring = request.method == "transfer_group_owner";
    auto const removing = request.method == "remove_group_member";
    auto const leaving = request.method == "leave_group";
    auto const pinning = request.method == "pin_group_message";
    auto const unpinning = request.method == "unpin_group_message";
    auto const announcing = request.method == "set_group_announcement";
    std::int64_t conversation = 0;
    std::int64_t user = 0;
    std::int64_t pinned_message = 0;
    bool admin = false;
    std::string title;
    std::optional<std::string> announcement;
    std::vector<std::int64_t> members;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    simdjson::error_code parse_error;
    if (setting_admin)
    {
        struct[[= simdjson::deny_unknown_fields]] set_admin_params
        {
            std::int64_t conversation = 0;
            std::int64_t user = 0;
            bool admin = false;
        };
        set_admin_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        user = params.user;
        admin = params.admin;
    }
    else if (transferring || removing)
    {
        struct[[= simdjson::deny_unknown_fields]] member_action_params
        {
            std::int64_t conversation = 0;
            std::int64_t user = 0;
        };
        member_action_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        user = params.user;
    }
    else if (pinning)
    {
        struct[[= simdjson::deny_unknown_fields]] pin_message_params
        {
            std::int64_t conversation = 0;
            std::int64_t message = 0;
        };
        pin_message_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        pinned_message = params.message;
    }
    else if (renaming)
    {
        struct[[= simdjson::deny_unknown_fields]] rename_group_params
        {
            std::int64_t conversation = 0;
            std::string title;
        };
        rename_group_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        title = std::move(params.title);
    }
    else if (announcing)
    {
        struct[[= simdjson::deny_unknown_fields]] announcement_params
        {
            std::int64_t conversation = 0;
            std::optional<std::string> text;
        };
        announcement_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        announcement = std::move(params.text);
    }
    else if (inviting)
    {
        struct[[= simdjson::deny_unknown_fields]] invite_members_params
        {
            std::int64_t conversation = 0;
            std::vector<std::int64_t> members;
        };
        invite_members_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        members = std::move(params.members);
        std::sort(members.begin(), members.end());
    }
    else
    {
        struct[[= simdjson::deny_unknown_fields]] conversation_action_params
        {
            std::int64_t conversation = 0;
        };
        conversation_action_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
    }
    if (parse_error || !document.at_end() || conversation <= 0 || ((setting_admin || transferring || removing) && user <= 0) ||
        (pinning && pinned_message <= 0) || (renaming && !chat::valid_group_title(title)) ||
        (announcing && (!announcement || announcement->size() > 4096 || announcement->find('\0') != std::string::npos)) ||
        (inviting && (members.empty() || members.front() <= 0 || std::adjacent_find(members.begin(), members.end()) != members.end() ||
                      std::binary_search(members.begin(), members.end(), *user_id_))))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    if (announcing && chat::text_is_blank(*announcement))
    {
        announcement->clear();
    }
    using result = std::expected<std::variant<bool, group_member_rejection>, sqlite_error>;
    auto changed = co_await database_.run(
        [&, actor = *user_id_](sqlite_database& db) -> result
        {
            if (renaming || announcing)
            {
                return set_group_text(
                    db, conversation, actor, renaming ? group_text_field::title : group_text_field::announcement, renaming ? title : *announcement);
            }
            if (pinning || unpinning)
            {
                return set_group_pin(db, conversation, actor, pinning ? std::optional(pinned_message) : std::nullopt);
            }
            if (inviting)
            {
                return invite_group_members(db, conversation, actor, members);
            }
            auto action = group_member_action::leave;
            if (setting_admin)
            {
                action = admin ? group_member_action::promote : group_member_action::demote;
            }
            else if (transferring)
            {
                action = group_member_action::transfer;
            }
            else if (removing)
            {
                action = group_member_action::remove;
            }
            return change_group_member(db, conversation, actor, action, user);
        });
    if (!changed)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto const* rejected = std::get_if<group_member_rejection>(&*changed))
    {
        int code = -32000;
        std::string_view message = "Server error";
        switch (*rejected)
        {
            case group_member_rejection::unavailable:
                code = -32006;
                message = "Group unavailable";
                break;
            case group_member_rejection::permission_denied:
                code = -32009;
                message = leaving ? "The owner cannot leave without transferring ownership" : "Group permission denied";
                break;
            case group_member_rejection::invalid_target:
                code = pinning ? -32007 : -32602;
                message = pinning         ? "Message unavailable"
                          : setting_admin ? "The owner cannot be an administrator"
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
        co_return serialize_json_rpc_error(code, message, std::move(request.id), response);
    }
    bool const updated = std::get<bool>(*changed);
    if (updated)
    {
        std::string notification = R"({"jsonrpc":"2.0","method":"conversation","params":{"conversation":)" + std::to_string(conversation);
        if (leaving || removing)
        {
            auto* departed = leaving ? this : users_.find(user);
            if (departed)
            {
                if (departed->upload_ && departed->upload_->conversation == conversation)
                {
                    departed->upload_.reset();
                }
                departed->enqueue_message(notification + R"(,"removed":true}})");
            }
        }
        co_await publish_conversation(conversation, notification + "}}");
    }
    co_return serialize_json_rpc_success(updated ? "{\"changed\":true}" : "{\"changed\":false}", std::move(request.id), response);
}
