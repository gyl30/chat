#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_group_management(json_rpc_request& request,
                                                                          std::string& response)
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
        struct [[= simdjson::deny_unknown_fields]] set_admin_params
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
        struct [[= simdjson::deny_unknown_fields]] member_action_params
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
        struct [[= simdjson::deny_unknown_fields]] pin_message_params
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
        struct [[= simdjson::deny_unknown_fields]] rename_group_params
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
        struct [[= simdjson::deny_unknown_fields]] announcement_params
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
        struct [[= simdjson::deny_unknown_fields]] invite_members_params
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
        struct [[= simdjson::deny_unknown_fields]] conversation_action_params
        {
            std::int64_t conversation = 0;
        };
        conversation_action_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
    }
    if (parse_error || !document.at_end() || conversation <= 0 || ((setting_admin || transferring || removing) && user <= 0) ||
        (pinning && pinned_message <= 0) ||
        (renaming && (title.empty() || title.size() > 256 || title.find('\0') != std::string::npos)) ||
        (announcing && (!announcement || announcement->size() > 4096 || announcement->find('\0') != std::string::npos)) ||
        (inviting && (members.empty() || members.front() <= 0 ||
            std::adjacent_find(members.begin(), members.end()) != members.end() ||
            std::binary_search(members.begin(), members.end(), *user_id_))))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto& connection = lease.connection();
    auto begun = co_await connection.execute_row("BEGIN");
    if (std::get<0>(begun))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto locked = co_await connection.execute_row(
        "SELECT kind,coalesce(owner_id,0)::text,coalesce(title,'') FROM conversations WHERE id=$1::bigint FOR UPDATE",
        {std::to_string(conversation)});
    auto& [lock_ec, group] = locked;
    if (lock_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    // 等待会话锁之后重新查询成员，避免使用等待期间已经失效的角色或阅读水位。
    auto actor_result = co_await connection.execute_row(
        "SELECT is_admin::text FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
        {std::to_string(conversation), std::to_string(*user_id_)});
    auto& [actor_ec, actor] = actor_result;
    if (actor_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto const owner = group && (*group)[1] == std::to_string(*user_id_);
    int error = 0;
    std::string message;
    bool changed = false;
    if (!group || (*group)[0] != "group" || !actor)
    {
        error = -32006;
        message = "Group unavailable";
    }
    else if (((setting_admin || transferring) && !owner) ||
             ((renaming || inviting || removing || pinning || unpinning || announcing) && !owner && (*actor)[0] != "true") || (leaving && owner))
    {
        error = -32009;
        message = leaving && owner ? "The owner cannot leave without transferring ownership" : "Group permission denied";
    }
    else if (pinning || unpinning)
    {
        if (pinning)
        {
            auto target = co_await connection.execute_row(
                "SELECT id::text FROM messages WHERE conversation_id=$1::bigint AND id=$2::bigint AND NOT deleted",
                {std::to_string(conversation), std::to_string(pinned_message)});
            if (std::get<0>(target))
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            if (!std::get<1>(target))
            {
                error = -32007;
                message = "Message unavailable";
            }
        }
        if (!error)
        {
            auto updated = co_await connection.execute_row(
                "UPDATE conversations SET pinned_message_id=NULLIF($2::bigint,0) WHERE id=$1::bigint "
                "AND pinned_message_id IS DISTINCT FROM NULLIF($2::bigint,0) RETURNING id::text",
                {std::to_string(conversation), std::to_string(pinned_message)});
            if (std::get<0>(updated))
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            changed = std::get<1>(updated).has_value();
        }
    }
    else if (announcing)
    {
        auto updated = co_await connection.execute_row(
            "UPDATE conversations SET announcement=$2 WHERE id=$1::bigint "
            "AND announcement IS DISTINCT FROM $2 RETURNING id::text",
            {std::to_string(conversation), *announcement});
        if (std::get<0>(updated))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        changed = std::get<1>(updated).has_value();
    }
    else if (transferring || removing)
    {
        auto target_result = co_await connection.execute_row(
            "SELECT is_admin::text FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
            {std::to_string(conversation), std::to_string(user)});
        auto& [target_ec, target] = target_result;
        if (target_ec)
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (user == *user_id_)
        {
            error = -32602;
            message = "Use leave_group to leave; ownership must be transferred to another administrator";
        }
        else if (!target)
        {
            error = -32005;
            message = "Member unavailable";
        }
        else if ((transferring && (*target)[0] != "true") ||
                 (removing && ((*group)[1] == std::to_string(user) || (!owner && (*target)[0] == "true"))))
        {
            error = -32009;
            message = "Group permission denied";
        }
        else if (transferring)
        {
            auto roles = co_await connection.execute_row(
                "UPDATE conversation_members SET is_admin=(user_id=$2::bigint) "
                "WHERE conversation_id=$1::bigint AND user_id IN ($2::bigint,$3::bigint)",
                {std::to_string(conversation), std::to_string(*user_id_), std::to_string(user)});
            auto transferred = co_await connection.execute_row(
                "UPDATE conversations SET owner_id=$2::bigint WHERE id=$1::bigint",
                {std::to_string(conversation), std::to_string(user)});
            if (std::get<0>(roles) || std::get<0>(transferred))
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            changed = true;
        }
        else
        {
            auto removed = co_await connection.execute_row(
                "DELETE FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
                {std::to_string(conversation), std::to_string(user)});
            if (std::get<0>(removed))
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            changed = true;
        }
    }
    else if (setting_admin)
    {
        if (user == *user_id_)
        {
            error = -32602;
            message = "The owner cannot be an administrator";
        }
        else
        {
            auto target_result = co_await connection.execute_row(
                "SELECT is_admin::text,(SELECT count(*) FROM conversation_members WHERE conversation_id=$1::bigint "
                "AND is_admin)::text FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
                {std::to_string(conversation), std::to_string(user)});
            auto& [target_ec, target] = target_result;
            if (target_ec)
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            if (!target)
            {
                error = -32005;
                message = "Member unavailable";
            }
            else if (((*target)[0] == "true") != admin)
            {
                if (admin && std::stoi((*target)[1]) >= 3)
                {
                    error = -32010;
                    message = "At most three administrators are allowed";
                }
                else
                {
                    auto updated = co_await connection.execute_row(
                        "UPDATE conversation_members SET is_admin=$3::boolean "
                        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING user_id::text",
                        {std::to_string(conversation), std::to_string(user), admin ? "true" : "false"});
                    if (std::get<0>(updated))
                    {
                        connection.close();
                        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
                    }
                    changed = true;
                }
            }
        }
    }
    else if (renaming && (*group)[2] != title)
    {
        auto updated = co_await connection.execute_row("UPDATE conversations SET title=$2 WHERE id=$1::bigint RETURNING id::text",
            {std::to_string(conversation), title});
        if (std::get<0>(updated))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        changed = true;
    }
    else if (inviting)
    {
        std::string ids = "{";
        for (auto id : members)
        {
            if (ids.size() > 1)
            {
                ids += ',';
            }
            ids += std::to_string(id);
        }
        ids += '}';
        auto targets = co_await connection.execute_scalar(
            "SELECT count(*)::text FROM contacts WHERE owner_id=$1::bigint AND contact_id=ANY($2::bigint[])",
            {std::to_string(*user_id_), ids});
        if (std::get<0>(targets))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (std::get<1>(targets) != std::to_string(members.size()))
        {
            error = -32005;
            message = "Invitees must be your contacts";
        }
        else
        {
            auto inserted = co_await connection.execute_scalar(
                "WITH added AS (INSERT INTO conversation_members(conversation_id,user_id,joined_message_id) "
                "SELECT $1::bigint,id,(SELECT coalesce(max(id),0) FROM messages WHERE conversation_id=$1::bigint) "
                "FROM unnest($2::bigint[]) AS invited(id) ON CONFLICT DO NOTHING RETURNING user_id) "
                "SELECT count(*)::text FROM added", {std::to_string(conversation), ids});
            if (std::get<0>(inserted))
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            changed = std::get<1>(inserted) != "0";
        }
    }
    else if (leaving)
    {
        auto removed = co_await connection.execute_row(
            "DELETE FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING user_id::text",
            {std::to_string(conversation), std::to_string(*user_id_)});
        if (std::get<0>(removed))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        changed = true;
    }
    auto ended = co_await connection.execute_row(error ? "ROLLBACK" : "COMMIT");
    if (std::get<0>(ended))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    lease = {};
    if (error)
    {
        co_return serialize_json_rpc_error(error, message, std::move(request.id), response);
    }
    if (changed)
    {
        std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":\"conversation\",\"params\":{\"conversation\":" +
            std::to_string(conversation) + "}}";
        if (leaving || removing)
        {
            auto* departed = leaving ? this : users_.find(user);
            if (departed)
            {
                if (departed->upload_ && departed->upload_->conversation == conversation)
                {
                    departed->upload_.reset();
                }
                departed->enqueue_message("{\"jsonrpc\":\"2.0\",\"method\":\"conversation\",\"params\":{\"conversation\":" +
                    std::to_string(conversation) + ",\"removed\":true}}");
            }
        }
        co_await publish_conversation(conversation, std::move(notification));
    }
    co_return serialize_json_rpc_success(changed ? "{\"changed\":true}" : "{\"changed\":false}",
                                         std::move(request.id), response);
}
