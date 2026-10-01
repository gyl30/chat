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

struct conversation_cursor
{
    std::int64_t activity = 0;
    std::int64_t id = 0;
};

struct [[= simdjson::deny_unknown_fields]] get_conversations_params
{
    std::optional<conversation_cursor> before;
};

simdjson::error_code parse_get_conversations_params(json_rpc_params& params, get_conversations_params& value)
{
    if (!params.present)
    {
        return simdjson::SUCCESS;
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

    if (value.before && (value.before->activity <= 0 || value.before->id <= 0))
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_conversations(json_rpc_request& request,
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

    get_conversations_params params{};
    auto params_error = parse_get_conversations_params(request.params, params);
    if (params_error)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto query_result = co_await lease.connection().execute_scalar(
            R"SQL(
        WITH page AS (
            SELECT c.*, own.last_read_message_id, own.joined_message_id,
                   CASE WHEN c.direct_user_low=$1::bigint THEN c.direct_user_high ELSE c.direct_user_low END AS peer
            FROM conversations c JOIN conversation_members own ON own.conversation_id=c.id
            WHERE own.user_id=$1::bigint AND (c.activity,c.id)<($2::bigint,$3::bigint)
              AND (c.kind='group' OR EXISTS(SELECT 1 FROM messages WHERE conversation_id=c.id))
            ORDER BY c.activity DESC,c.id DESC LIMIT 51
        ), visible AS (SELECT * FROM page ORDER BY activity DESC,id DESC LIMIT 50)
        SELECT json_build_object(
            'conversations',COALESCE((SELECT json_agg(json_build_object(
                'id',c.id,'kind',c.kind,'user',c.peer,'username',COALESCE(c.title,u.username),
                'avatar_revision',COALESCE(u.avatar_revision,0),'has_avatar',EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id),
                'activity',c.activity,'member_count',(SELECT count(*) FROM conversation_members WHERE conversation_id=c.id),
                'last',(SELECT json_build_object('id',m.id,'conversation',m.conversation_id,'from',m.sender_id,
                      'avatar_revision',author.avatar_revision,'has_avatar',EXISTS(SELECT 1 FROM user_avatars WHERE user_id=author.id),
                      'username',author.username,'timestamp',(extract(epoch FROM m.created_at)*1000)::bigint,'text',m.body,'deleted',m.deleted,'edited_at',(extract(epoch FROM m.edited_at)*1000)::bigint,'reply',CASE WHEN r.id IS NULL THEN NULL ELSE json_build_object('id',r.id,'from',r.sender_id,'username',ra.username,'text',left(r.body,160),'edited_at',(extract(epoch FROM r.edited_at)*1000)::bigint,'deleted',r.deleted) END,
                      'attachment',(SELECT json_build_object('filename',filename,'media_type',media_type,'size',size)
                                    FROM message_attachments WHERE message_id=m.id AND NOT m.deleted))
                      FROM messages m JOIN users author ON author.id=m.sender_id LEFT JOIN messages r ON r.id=m.reply_to_id LEFT JOIN users ra ON ra.id=r.sender_id
                      WHERE m.conversation_id=c.id ORDER BY m.id DESC LIMIT 1),
                'unread',(SELECT count(*) FROM messages m WHERE m.conversation_id=c.id
                          AND m.id>GREATEST(c.last_read_message_id,c.joined_message_id) AND NOT m.deleted
                          AND (m.sender_id<>$1::bigint OR c.direct_user_low=c.direct_user_high))
            ) ORDER BY c.activity DESC,c.id DESC) FROM visible c LEFT JOIN users u ON u.id=c.peer),'[]'::json),
            'next',CASE WHEN (SELECT count(*) FROM page)>50 THEN
                (SELECT json_build_object('activity',activity,'id',id) FROM visible ORDER BY activity,id LIMIT 1)
                ELSE NULL END
        )::text
    )SQL",
            {std::to_string(*user_id_),
             std::to_string(params.before ? params.before->activity : std::numeric_limits<std::int64_t>::max()),
             std::to_string(params.before ? params.before->id : std::numeric_limits<std::int64_t>::max())});
        auto& [ec, result] = query_result;
        if (ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
    co_return serialize_json_rpc_success(result, std::move(request.id), response);
    }
