#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"
#include "message_payload.hpp"
#include "message_mentions.hpp"
#include "pg_connection_pool.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_update_message(json_rpc_request& request,
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
    struct [[= simdjson::deny_unknown_fields]] update_params
    {
        std::int64_t conversation = 0;
        std::int64_t message = 0;
        std::optional<std::string> text;
    };
    update_params params;
    auto const deleting = request.method == "delete_message";
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end() || params.conversation <= 0 || params.message <= 0 ||
        (deleting ? params.text.has_value() : (!params.text || params.text->empty() || params.text->find('\0') != std::string::npos)))
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
    if (!deleting)
    {
        auto allowed = co_await check_conversation_send(connection, params.conversation);
        if (!allowed || !*allowed)
        {
            auto rolled_back = co_await connection.execute_row("ROLLBACK");
            if (std::get<0>(rolled_back)) { connection.close(); }
            co_return serialize_json_rpc_error(allowed ? -32006 : -32000,
                allowed ? "Communication not allowed" : "Server error", std::move(request.id), response);
        }
    }
    auto original = co_await lease.connection().execute_row(
        "SELECT json_build_object('id',m.id,'conversation',m.conversation_id,'from',m.sender_id,'username',u.username,"
        "'avatar_revision',u.avatar_revision,'has_avatar',EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id),"
        "'mentions',(SELECT coalesce(json_agg(json_build_object('user',mentioned.id,'username',mentioned.username) "
        "ORDER BY mentioned.id),'[]'::json) FROM message_mentions mm JOIN users mentioned ON mentioned.id=mm.user_id "
        "WHERE mm.message_id=m.id),"
        "'reaction_revision',m.reaction_revision,'reactions',(SELECT coalesce(json_agg(json_build_object("
        "'emoji',emoji,'users',users) ORDER BY emoji),'[]'::json) FROM (SELECT emoji,json_agg(user_id ORDER BY user_id) "
        "AS users FROM message_reactions WHERE message_id=m.id GROUP BY emoji) reactions),"
        "'timestamp',(extract(epoch FROM "
        "m.created_at)*1000)::bigint,'text',m.body,'deleted',m.deleted,'edited_at',(extract(epoch FROM "
        "m.edited_at)*1000)::bigint,'attachment',(SELECT json_build_object('filename',filename,'media_type',media_type,"
        "'size',size) FROM message_attachments WHERE message_id=m.id AND NOT m.deleted),"
        "'reply',CASE WHEN r.id IS NULL THEN NULL ELSE "
        "json_build_object('id',r.id,'from',r.sender_id,'username',ra.username,'text',left(r.body,160),'edited_at',("
        "extract(epoch FROM r.edited_at)*1000)::bigint,'deleted',r.deleted) END)::text "
        "FROM messages m JOIN users u ON u.id=m.sender_id JOIN conversation_members own ON "
        "own.conversation_id=m.conversation_id "
        "LEFT JOIN messages r ON r.id=m.reply_to_id LEFT JOIN users ra ON ra.id=r.sender_id "
        "WHERE m.id=$3::bigint AND m.conversation_id=$2::bigint AND m.sender_id=$1::bigint AND own.user_id=$1::bigint "
        "AND (NOT m.deleted OR $4::boolean) AND ($4::boolean OR NOT EXISTS(SELECT 1 FROM message_attachments "
        "WHERE message_id=m.id))",
        {std::to_string(*user_id_), std::to_string(params.conversation), std::to_string(params.message),
         deleting ? "true" : "false"});
    auto& [read_ec, row] = original;
    if (read_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!row)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32007, "Message unavailable", std::move(request.id), response);
    }
    message_payload value;
    simdjson::padded_string json(row->front());
    simdjson::ondemand::parser message_parser;
    simdjson::ondemand::document message_document;
    if (message_parser.iterate(json).get(message_document) || message_document.get(value))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    value.text = deleting ? std::string{} : std::move(*params.text);
    value.mentions.clear();
    value.deleted = deleting;
    if (deleting)
    {
        value.attachment.reset();
        value.reactions.clear();
    }
    if (!deleting)
    {
        value.edited_at = std::numeric_limits<std::int64_t>::max();
    }
    std::string payload;
    auto error = simdjson::builder::to_json_string(value).get(payload);
    if (error)
    {
        connection.close();
        co_return error;
    }
    constexpr std::string_view notification_prefix = R"({"jsonrpc":"2.0","method":"message_updated","params":)";
    if (notification_prefix.size() + payload.size() + 1 > 64 * 1024)
    {
        connection.close();
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto updated = co_await lease.connection().execute_row(
        "WITH updated AS (UPDATE messages SET body=$4,deleted=$5::boolean,"
        "reaction_revision=reaction_revision+CASE WHEN $5::boolean AND NOT deleted THEN 1 ELSE 0 END,"
        "edited_at=CASE WHEN $5::boolean THEN edited_at ELSE "
        "greatest(clock_timestamp(),coalesce(edited_at,'epoch'::timestamptz)+interval '1 millisecond') END "
        "WHERE id=$3::bigint AND conversation_id=$2::bigint AND sender_id=$1::bigint AND (NOT deleted OR $5::boolean) "
        "AND EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=$2::bigint AND user_id=$1::bigint) "
        "RETURNING edited_at,reaction_revision), cleared AS (DELETE FROM message_attachments WHERE message_id=$3::bigint "
        "AND $5::boolean AND EXISTS(SELECT 1 FROM updated) RETURNING message_id) "
        ",cleared_reactions AS (DELETE FROM message_reactions WHERE message_id=$3::bigint AND $5::boolean "
        "AND EXISTS(SELECT 1 FROM updated) RETURNING message_id) "
        ",cleared_pin AS (UPDATE conversations SET pinned_message_id=NULL WHERE id=$2::bigint "
        "AND pinned_message_id=$3::bigint AND $5::boolean AND EXISTS(SELECT 1 FROM updated) RETURNING id) "
        "SELECT COALESCE(((extract(epoch FROM edited_at)*1000)::bigint)::text,''),reaction_revision::text,"
        "EXISTS(SELECT 1 FROM cleared_pin)::text FROM updated",
        {std::to_string(*user_id_), std::to_string(params.conversation), std::to_string(params.message), value.text,
         deleting ? "true" : "false"});
    auto& [write_ec, result] = updated;
    if (write_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!result)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32007, "Message unavailable", std::move(request.id), response);
    }
    value.edited_at = result->front().empty() ? std::nullopt : std::optional<std::int64_t>(std::stoll(result->front()));
    value.reaction_revision = std::stoll(result->at(1));
    auto const pin_cleared = result->at(2) == "true";
    auto mentioned = co_await refresh_message_mentions(connection, params.conversation, params.message, value.text);
    auto& [mention_ec, mentions] = mentioned;
    if (mention_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    value.mentions = std::move(mentions);

    payload.clear();
    error = simdjson::builder::to_json_string(value).get(payload);
    if (error)
    {
        connection.close();
        co_return error;
    }
    if (notification_prefix.size() + payload.size() + 1 > 64 * 1024)
    {
        connection.close();
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto committed = co_await connection.execute_row("COMMIT");
    if (std::get<0>(committed))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    lease = {};
    co_await publish_conversation(params.conversation, std::string(notification_prefix) + payload + "}");
    if (pin_cleared)
    {
        co_await publish_conversation(params.conversation, "{\"jsonrpc\":\"2.0\",\"method\":\"conversation\",\"params\":{\"conversation\":" +
            std::to_string(params.conversation) + "}}");
    }
    co_return serialize_json_rpc_success(payload, std::move(request.id), response);
}
