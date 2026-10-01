#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

struct [[= simdjson::deny_unknown_fields]] get_messages_params
{
    std::int64_t conversation = 0;
    std::optional<std::int64_t> before;
    std::optional<std::int64_t> after;
    std::optional<std::string> query;
};

simdjson::error_code parse_get_messages_params(json_rpc_params& params, get_messages_params& value)
{
    if (!params.present)
    {
        return simdjson::NO_SUCH_FIELD;
    }

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    auto error = parser.iterate(params.json).get(document);
    if (error)
    {
        return error;
    }

    error = document.get(value);
    if (error)
    {
        return error;
    }

    if (!document.at_end())
    {
        return simdjson::TRAILING_CONTENT;
    }

    if (value.conversation <= 0 || (value.before && *value.before <= 0) || (value.after && *value.after < 0) ||
        (value.before && value.after))
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

} // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_messages(json_rpc_request& request,
                                                                          std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    get_messages_params params{};
    auto params_error = parse_get_messages_params(request.params, params);
    bool const searching = request.method == "search_messages";
    if (params_error || (searching && (!params.query || params.query->empty() || params.query->size() > 1024
        || params.query->find('\0') != std::string::npos || params.after))
        || (!searching && params.query))
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    std::string query = R"SQL(
        WITH page AS (
            SELECT m.id,m.conversation_id AS conversation,m.sender_id AS "from",u.username,
                   (extract(epoch FROM m.created_at)*1000)::bigint AS timestamp,m.body AS text, m.deleted, (extract(epoch FROM m.edited_at)*1000)::bigint AS edited_at, CASE WHEN r.id IS NULL THEN NULL ELSE json_build_object('id',r.id,'from',r.sender_id,'username',ra.username,'text',left(r.body,160),'edited_at',(extract(epoch FROM r.edited_at)*1000)::bigint,'deleted',r.deleted) END AS reply
            FROM messages m JOIN users u ON u.id=m.sender_id LEFT JOIN messages r ON r.id=m.reply_to_id LEFT JOIN users ra ON ra.id=r.sender_id
            WHERE m.conversation_id=$2::bigint AND )SQL";
    if (searching)
    {
        query += "NOT m.deleted AND strpos(lower(m.body),lower($4))>0 AND ";
    }
    query += params.after ? "m.id>$3::bigint ORDER BY m.id ASC" : "m.id<$3::bigint ORDER BY m.id DESC";
    query += " LIMIT 51), visible AS (SELECT * FROM page ORDER BY id ";
    query += params.after ? "ASC" : "DESC";
    query += R"SQL( LIMIT 50)
        SELECT json_build_object('messages',COALESCE((SELECT json_agg(row_to_json(v) ORDER BY id) FROM visible v),'[]'::json),
            'read_positions',(SELECT json_agg(json_build_object('user',user_id,'message',last_read_message_id) ORDER BY user_id)
                FROM conversation_members WHERE conversation_id=$2::bigint),
            'has_more',(SELECT count(*) FROM page)>50)::text
        FROM conversation_members WHERE conversation_id=$2::bigint AND user_id=$1::bigint
    )SQL";
    std::vector<std::string> parameters{
        std::to_string(*user_id_), std::to_string(params.conversation),
        std::to_string(params.after.value_or(params.before.value_or(std::numeric_limits<std::int64_t>::max())))};
    if (searching)
    {
        parameters.push_back(std::move(*params.query));
    }
    auto query_result = co_await lease.connection().execute_row(std::move(query), std::move(parameters));
    auto& [ec, row] = query_result;
    if (ec)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!row)
    {
        co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
    }
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    co_return serialize_json_rpc_success(row->front(), std::move(request.id), response);
}
