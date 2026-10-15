#ifndef CHAT_SRC_MESSAGE_QUERY_HPP
#define CHAT_SRC_MESSAGE_QUERY_HPP

#include <string_view>

// One message row as the client parses it; $1 is the requesting user, who alone sees their own client_id.
inline constexpr std::string_view kMessageRow = R"SQL(
            SELECT m.id,m.conversation_id AS conversation,m.sender_id AS "from",u.username,
                   u.avatar_revision,EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id) AS has_avatar,
                   (SELECT coalesce(json_agg(json_build_object('user',mentioned.id,'username',mentioned.username)
                       ORDER BY mentioned.id),'[]'::json) FROM message_mentions mm JOIN users mentioned ON mentioned.id=mm.user_id
                       WHERE mm.message_id=m.id) AS mentions,
                   m.reaction_revision,(SELECT coalesce(json_agg(json_build_object('emoji',emoji,'users',users) ORDER BY emoji),'[]'::json)
                     FROM (SELECT emoji,json_agg(user_id ORDER BY user_id) AS users FROM message_reactions
                           WHERE message_id=m.id GROUP BY emoji) reactions) AS reactions,
                   (extract(epoch FROM m.created_at)*1000)::bigint AS timestamp,m.body AS text, m.deleted, (extract(epoch FROM m.edited_at)*1000)::bigint AS edited_at, CASE WHEN r.id IS NULL THEN NULL ELSE json_build_object('id',r.id,'from',r.sender_id,'username',ra.username,'text',left(r.body,160),'edited_at',(extract(epoch FROM r.edited_at)*1000)::bigint,'deleted',r.deleted) END AS reply,
                   (SELECT json_build_object('filename',filename,'media_type',media_type,'size',size)
                    FROM message_attachments WHERE message_id=m.id AND NOT m.deleted) AS attachment,
                   CASE WHEN m.sender_id=$1::bigint THEN m.client_id END AS client_id
            FROM messages m JOIN users u ON u.id=m.sender_id LEFT JOIN messages r ON r.id=m.reply_to_id LEFT JOIN users ra ON ra.id=r.sender_id
            WHERE )SQL";

#endif
