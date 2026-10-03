#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <chat/reaction.hpp>
#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{
struct [[= simdjson::deny_unknown_fields]] reaction_params
{
    std::int64_t conversation = 0;
    std::int64_t message = 0;
    std::optional<std::string> emoji;
};
}

boost::capy::task<simdjson::error_code> chat_session::handle_message_reaction(json_rpc_request& request,
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
    reaction_params params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end() || params.conversation <= 0 || params.message <= 0 || !params.emoji ||
        (!params.emoji->empty() && std::ranges::find(chat::reaction_choices, *params.emoji) == chat::reaction_choices.end()))
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
    auto locked = co_await connection.execute_row("SELECT id::text FROM conversations WHERE id=$1::bigint FOR UPDATE",
                                                 {std::to_string(params.conversation)});
    if (std::get<0>(begun) || std::get<0>(locked))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto original = co_await connection.execute_row(
        "SELECT coalesce((SELECT emoji FROM message_reactions WHERE message_id=m.id AND user_id=$1::bigint),'') "
        "FROM messages m JOIN conversation_members own ON own.conversation_id=m.conversation_id "
        "WHERE m.conversation_id=$2::bigint AND m.id=$3::bigint AND own.user_id=$1::bigint AND NOT m.deleted",
        {std::to_string(*user_id_), std::to_string(params.conversation), std::to_string(params.message)});
    auto& [read_ec, row] = original;
    if (read_ec || !row)
    {
        connection.close();
        co_return serialize_json_rpc_error(read_ec ? -32000 : -32007,
                                          read_ec ? "Server error" : "Message unavailable", std::move(request.id), response);
    }
    auto allowed = co_await check_conversation_send(connection, params.conversation);
    if (!allowed || !*allowed)
    {
        auto rolled_back = co_await connection.execute_row("ROLLBACK");
        if (std::get<0>(rolled_back)) { connection.close(); }
        co_return serialize_json_rpc_error(allowed ? -32006 : -32000,
            allowed ? "Communication not allowed" : "Server error", std::move(request.id), response);
    }
    auto const changed = row->front() != *params.emoji;
    if (changed)
    {
        auto written = params.emoji->empty()
            ? co_await connection.execute_row("DELETE FROM message_reactions WHERE message_id=$1::bigint AND user_id=$2::bigint",
                                               {std::to_string(params.message), std::to_string(*user_id_)})
            : co_await connection.execute_row(
                "INSERT INTO message_reactions(message_id,user_id,emoji) VALUES($1::bigint,$2::bigint,$3) "
                "ON CONFLICT(message_id,user_id) DO UPDATE SET emoji=excluded.emoji",
                {std::to_string(params.message), std::to_string(*user_id_), *params.emoji});
        if (std::get<0>(written))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto advanced = co_await connection.execute_row(
            "UPDATE messages SET reaction_revision=reaction_revision+1 WHERE id=$1::bigint",
            {std::to_string(params.message)});
        if (std::get<0>(advanced))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
    }
    auto snapshot = co_await connection.execute_scalar(
        "SELECT json_build_object('conversation',conversation_id,'message',id,'reaction_revision',reaction_revision,"
        "'reactions',coalesce((SELECT json_agg(json_build_object('emoji',emoji,'users',users) ORDER BY emoji) FROM "
        "(SELECT emoji,json_agg(user_id ORDER BY user_id) AS users FROM message_reactions WHERE message_id=m.id "
        "GROUP BY emoji) r),'[]'::json))::text FROM messages m WHERE id=$1::bigint",
        {std::to_string(params.message)});
    auto& [snapshot_ec, payload] = snapshot;
    if (snapshot_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto committed = co_await connection.execute_row("COMMIT");
    if (std::get<0>(committed))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    lease = {};
    if (changed)
    {
        co_await publish_conversation(params.conversation,
            R"({"jsonrpc":"2.0","method":"reaction","params":)" + payload + "}");
    }
    co_return serialize_json_rpc_success(payload, std::move(request.id), response);
}
