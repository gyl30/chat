#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>
#include <chat/text.hpp>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

struct [[= simdjson::deny_unknown_fields]] create_conversation_params
{
    [[= simdjson::default_value]] std::int64_t user = 0;
    [[= simdjson::default_value]] std::string title;
    [[= simdjson::default_value]] std::vector<std::int64_t> members;
};

}

boost::capy::task<std::expected<bool, std::error_code>> chat_session::check_conversation_send(
    pg_connection& connection, std::int64_t conversation)
{
    auto result = co_await connection.execute_row(
        "SELECT own.user_id::text FROM conversation_members own JOIN conversations c ON c.id=own.conversation_id "
        "WHERE c.id=$2::bigint AND own.user_id=$1::bigint AND (c.kind='group' OR EXISTS(SELECT 1 FROM contacts "
        "WHERE owner_id=$1::bigint AND contact_id=CASE WHEN c.direct_user_low=$1::bigint "
        "THEN c.direct_user_high ELSE c.direct_user_low END FOR KEY SHARE))",
        {std::to_string(*user_id_), std::to_string(conversation)});
    auto& [ec, row] = result;
    if (ec) { co_return std::unexpected(ec); }
    co_return row.has_value();
}

boost::capy::task<simdjson::error_code> chat_session::handle_create_conversation(json_rpc_request& request,
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
    create_conversation_params params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end())
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto const group = request.method == "create_group";
    std::sort(params.members.begin(), params.members.end());
    if ((!group && (params.user <= 0 || !params.title.empty() || !params.members.empty())) ||
        (group && (params.user != 0 || !chat::valid_group_title(params.title) || params.members.empty() ||
                   params.members.front() <= 0 ||
                   std::adjacent_find(params.members.begin(), params.members.end()) != params.members.end() ||
                   std::binary_search(params.members.begin(), params.members.end(), *user_id_))))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    std::string query;
    std::vector<std::string> values{std::to_string(*user_id_)};
    if (group)
    {
        std::string members = "{";
        for (auto const member : params.members)
        {
            if (members.size() > 1)
            {
                members.push_back(',');
            }
            members.append(std::to_string(member));
        }
        members.push_back('}');
        values.push_back(std::move(members));
        values.push_back(params.title);
        query = "WITH targets AS (SELECT contact_id AS id FROM contacts WHERE owner_id=$1::bigint AND "
                "contact_id=ANY($2::bigint[])), "
                "created AS (INSERT INTO conversations(kind,title,owner_id) SELECT 'group',$3,$1::bigint "
                "WHERE (SELECT count(*) FROM targets)=cardinality($2::bigint[]) RETURNING id), "
                "members AS (INSERT INTO conversation_members(conversation_id,user_id) "
                "SELECT id,$1::bigint FROM created UNION ALL SELECT created.id,targets.id FROM created CROSS JOIN "
                "targets RETURNING user_id) "
                "SELECT id::text FROM created";
    }
    else
    {
        auto begun = co_await lease.connection().execute_row("BEGIN");
        if (std::get<0>(begun))
        {
            lease.connection().close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        values.push_back(std::to_string(params.user));
        query = "WITH created AS (INSERT INTO conversations(kind,direct_user_low,direct_user_high) "
                "SELECT 'direct',least($1::bigint,id),greatest($1::bigint,id) FROM users WHERE id=$2::bigint "
                "ON CONFLICT(direct_user_low,direct_user_high) WHERE kind='direct' DO UPDATE SET kind='direct' "
                "RETURNING id), "
                "members AS (INSERT INTO conversation_members(conversation_id,user_id) "
                "SELECT id,$1::bigint FROM created UNION SELECT id,$2::bigint FROM created ON CONFLICT DO NOTHING "
                "RETURNING user_id) "
                "SELECT id::text FROM created";
    }
    auto query_result = co_await lease.connection().execute_row(std::move(query), std::move(values));
    auto& [ec, row] = query_result;
    if (ec)
    {
        if (!group) { lease.connection().close(); }
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!row)
    {
        if (!group)
        {
            auto rolled_back = co_await lease.connection().execute_row("ROLLBACK");
            if (std::get<0>(rolled_back)) { lease.connection().close(); }
        }
        co_return serialize_json_rpc_error(-32005, "User unavailable", std::move(request.id), response);
    }
    auto const id = std::stoll(row->front());
    if (!group)
    {
        auto allowed = co_await check_conversation_send(lease.connection(), id);
        if (!allowed)
        {
            lease.connection().close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto ended = co_await lease.connection().execute_row(*allowed ? "COMMIT" : "ROLLBACK");
        if (std::get<0>(ended))
        {
            lease.connection().close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!*allowed)
        {
            co_return serialize_json_rpc_error(-32005, "Contact required", std::move(request.id), response);
        }
    }
    lease = {};
    if (group)
    {
        co_await publish_conversation(
            id, "{\"jsonrpc\":\"2.0\",\"method\":\"conversation\",\"params\":{\"conversation\":" + row->front() + "}}");
    }
    co_return serialize_json_rpc_success("{\"conversation\":" + row->front() +
        (group ? "}" : ",\"can_send\":true}"), std::move(request.id), response);
}

boost::capy::task<simdjson::error_code> chat_session::handle_get_members(json_rpc_request& request,
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
    };
    params_type params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end() || params.conversation <= 0)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto query_result = co_await lease.connection().execute_row(
        "SELECT json_build_object('members',(SELECT json_agg(json_build_object('id',u.id,'username',u.username,"
        "'avatar_revision',u.avatar_revision,'has_avatar',EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id),"
        "'role',CASE WHEN u.id=c.owner_id THEN 'owner' WHEN m.is_admin THEN 'admin' ELSE 'member' END) ORDER BY "
        "u.id) "
        "FROM conversation_members m JOIN users u ON u.id=m.user_id JOIN conversations c ON c.id=m.conversation_id "
        "WHERE m.conversation_id=$2::bigint))::text "
        "FROM conversation_members WHERE conversation_id=$2::bigint AND user_id=$1::bigint",
        {std::to_string(*user_id_), std::to_string(params.conversation)});
    auto& [ec, row] = query_result;
    if (ec)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!row)
    {
        co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
    }
    co_return serialize_json_rpc_success(row->front(), std::move(request.id), response);
}
