#include <cstdint>
#include <string>
#include <utility>

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
    struct [[= simdjson::deny_unknown_fields]] params_type
    {
        std::int64_t conversation = 0;
        std::int64_t user = 0;
        bool admin = false;
    };
    params_type params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end() || params.conversation <= 0 || params.user <= 0)
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
        "SELECT kind,coalesce(owner_id,0)::text FROM conversations WHERE id=$1::bigint FOR UPDATE",
        {std::to_string(params.conversation)});
    auto& [lock_ec, group] = locked;
    if (lock_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    int error = 0;
    std::string message;
    bool changed = false;
    if (!group || (*group)[0] != "group")
    {
        error = -32006;
        message = "Group unavailable";
    }
    else if ((*group)[1] != std::to_string(*user_id_))
    {
        error = -32009;
        message = "Only the group owner can manage administrators";
    }
    else if (params.user == *user_id_)
    {
        error = -32602;
        message = "The owner cannot be an administrator";
    }
    else
    {
        auto target_result = co_await connection.execute_row(
            "SELECT is_admin::text,(SELECT count(*) FROM conversation_members WHERE conversation_id=$1::bigint "
            "AND is_admin)::text FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
            {std::to_string(params.conversation), std::to_string(params.user)});
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
        else if (((*target)[0] == "true") != params.admin)
        {
            if (params.admin && std::stoi((*target)[1]) >= 3)
            {
                error = -32010;
                message = "At most three administrators are allowed";
            }
            else
            {
                auto updated = co_await connection.execute_row(
                    "UPDATE conversation_members SET is_admin=$3::boolean "
                    "WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING user_id::text",
                    {std::to_string(params.conversation), std::to_string(params.user), params.admin ? "true" : "false"});
                if (std::get<0>(updated))
                {
                    connection.close();
                    co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
                }
                changed = true;
            }
        }
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
        co_await publish_conversation(params.conversation,
            "{\"jsonrpc\":\"2.0\",\"method\":\"conversation\",\"params\":{\"conversation\":" +
            std::to_string(params.conversation) + "}}");
    }
    co_return serialize_json_rpc_success(changed ? "{\"changed\":true}" : "{\"changed\":false}",
                                         std::move(request.id), response);
}
