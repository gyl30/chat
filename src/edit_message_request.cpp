#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"
#include "message_payload.hpp"
#include "pg_connection_pool.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_edit_message(json_rpc_request& request,
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
    struct [[= simdjson::deny_unknown_fields]] edit_params
    {
        std::int64_t conversation = 0;
        std::int64_t message = 0;
        std::string text;
    };
    edit_params params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end() || params.conversation <= 0 || params.message <= 0 || params.text.empty())
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto original = co_await lease.connection().execute_row(
        "SELECT json_build_object('id',m.id,'conversation',m.conversation_id,'from',m.sender_id,'username',u.username,"
        "'timestamp',(extract(epoch FROM m.created_at)*1000)::bigint,'text',m.body,'edited_at',(extract(epoch FROM "
        "m.edited_at)*1000)::bigint,"
        "'reply',CASE WHEN r.id IS NULL THEN NULL ELSE "
        "json_build_object('id',r.id,'from',r.sender_id,'username',ra.username,'text',left(r.body,160),'edited_at',("
        "extract(epoch FROM r.edited_at)*1000)::bigint) END)::text "
        "FROM messages m JOIN users u ON u.id=m.sender_id JOIN conversation_members own ON "
        "own.conversation_id=m.conversation_id "
        "LEFT JOIN messages r ON r.id=m.reply_to_id LEFT JOIN users ra ON ra.id=r.sender_id "
        "WHERE m.id=$3::bigint AND m.conversation_id=$2::bigint AND m.sender_id=$1::bigint AND own.user_id=$1::bigint",
        {std::to_string(*user_id_), std::to_string(params.conversation), std::to_string(params.message)});
    auto& [read_ec, row] = original;
    if (read_ec)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!row)
    {
        co_return serialize_json_rpc_error(-32007, "Message unavailable", std::move(request.id), response);
    }
    message_payload value;
    simdjson::padded_string json(row->front());
    simdjson::ondemand::parser message_parser;
    simdjson::ondemand::document message_document;
    if (message_parser.iterate(json).get(message_document) || message_document.get(value))
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    value.text = std::move(params.text);
    value.edited_at = std::numeric_limits<std::int64_t>::max();
    std::string payload;
    auto error = simdjson::builder::to_json_string(value).get(payload);
    if (error)
    {
        co_return error;
    }
    constexpr std::string_view notification_prefix = R"({"jsonrpc":"2.0","method":"message_updated","params":)";
    if (notification_prefix.size() + payload.size() + 1 > 64 * 1024)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto updated = co_await lease.connection().execute_row(
        "UPDATE messages SET "
        "body=$4,edited_at=greatest(clock_timestamp(),coalesce(edited_at,'epoch'::timestamptz)+interval '1 "
        "millisecond') "
        "WHERE id=$3::bigint AND conversation_id=$2::bigint AND sender_id=$1::bigint "
        "RETURNING ((extract(epoch FROM edited_at)*1000)::bigint)::text",
        {std::to_string(*user_id_), std::to_string(params.conversation), std::to_string(params.message), value.text});
    auto& [write_ec, result] = updated;
    if (write_ec)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!result)
    {
        co_return serialize_json_rpc_error(-32007, "Message unavailable", std::move(request.id), response);
    }
    value.edited_at = std::stoll(result->front());
    payload.clear();
    error = simdjson::builder::to_json_string(value).get(payload);
    if (error)
    {
        co_return error;
    }
    lease = {};
    co_await publish_conversation(params.conversation, std::string(notification_prefix) + payload + "}");
    co_return serialize_json_rpc_success(payload, std::move(request.id), response);
}
