#include "conversation_store.hpp"

#include <limits>
#include <utility>

std::expected<std::string, sqlite_error> read_conversation_list(sqlite_database& db,
                                                                std::int64_t user,
                                                                std::optional<chat::conversation_cursor> before)
{
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    auto query = db.prepare(R"SQL(
        WITH page AS (
            SELECT c.*, own.pinned, own.muted, own.last_read_message_id, own.joined_message_id,
                   CASE WHEN c.direct_user_low=?1 THEN c.direct_user_high ELSE c.direct_user_low END AS peer
            FROM conversation_members own JOIN conversations c ON c.id=own.conversation_id
            WHERE own.user_id=?1
              AND (c.kind='group' OR EXISTS(SELECT 1 FROM contacts WHERE owner_id=?1 AND contact_id=
                  CASE WHEN c.direct_user_low=?1 THEN c.direct_user_high ELSE c.direct_user_low END))
              AND (own.pinned,c.activity,c.id)<(?2,?3,?4)
            ORDER BY own.pinned DESC,c.activity DESC,c.id DESC LIMIT 51
        ), visible AS (
            SELECT * FROM page ORDER BY pinned DESC,activity DESC,id DESC LIMIT 50
        )
        SELECT json_object(
            'conversations', json((SELECT json_group_array(json_object(
                'id',c.id,'kind',c.kind,'user',c.peer,'username',coalesce(c.title,u.username),
                'avatar_revision',coalesce(u.avatar_revision,0),
                'has_avatar',json(CASE WHEN avatar.user_id IS NULL THEN 'false' ELSE 'true' END),
                'activity',c.activity,'muted',json(CASE WHEN c.muted THEN 'true' ELSE 'false' END),
                'pinned',json(CASE WHEN c.pinned THEN 'true' ELSE 'false' END),
                'member_count',(SELECT count(*) FROM conversation_members WHERE conversation_id=c.id),
                'can_send',json('true'), 'announcement',c.announcement,
                'join_approval',json(CASE WHEN c.join_approval THEN 'true' ELSE 'false' END),
                'pinned_message',json((SELECT json_object('id',pm.id,'from',pm.sender_id,'username',pu.username,
                    'text',substr(pm.body,1,160),'edited_at',pm.edited_at,'deleted',json('false'))
                    FROM messages pm JOIN users pu ON pu.id=pm.sender_id
                    WHERE pm.id=c.pinned_message_id AND pm.conversation_id=c.id AND pm.deleted=0)),
                'last',json((SELECT json_object('id',m.id,'conversation',m.conversation_id,'from',m.sender_id,
                    'username',author.username,'timestamp',m.created_at,'text',m.body,'edited_at',m.edited_at,
                    'deleted',json(CASE WHEN m.deleted THEN 'true' ELSE 'false' END),
                    'avatar_revision',author.avatar_revision,
                    'has_avatar',json(CASE WHEN sender_avatar.user_id IS NULL THEN 'false' ELSE 'true' END),
                    'reaction_revision',m.reaction_revision,
                    'reactions',json((SELECT json_group_array(json_object('emoji',emoji,'users',json(users)))
                        FROM (SELECT emoji,json_group_array(user_id) AS users
                              FROM (SELECT emoji,user_id FROM message_reactions WHERE message_id=m.id
                                    ORDER BY user_id)
                              GROUP BY emoji ORDER BY emoji))),
                    'mentions',json((SELECT json_group_array(json_object('user',mentioned.id,'username',mentioned.username))
                        FROM (SELECT u.id,u.username FROM message_mentions mm JOIN users u ON u.id=mm.user_id
                              WHERE mm.message_id=m.id ORDER BY u.id) mentioned)),
                    'reply',CASE WHEN r.id IS NULL THEN NULL ELSE json_object(
                        'id',r.id,'from',r.sender_id,'username',ra.username,'text',substr(r.body,1,160),'edited_at',r.edited_at,
                        'deleted',json(CASE WHEN r.deleted THEN 'true' ELSE 'false' END)) END,
                    'attachment',CASE WHEN a.message_id IS NULL THEN NULL ELSE json_object(
                        'filename',a.filename,'media_type',a.media_type,'size',a.size) END)
                    FROM messages m JOIN users author ON author.id=m.sender_id
                    LEFT JOIN messages r ON r.id=m.reply_to_id LEFT JOIN users ra ON ra.id=r.sender_id
                    LEFT JOIN user_avatars sender_avatar ON sender_avatar.user_id=author.id
                    LEFT JOIN message_attachments a ON a.message_id=m.id AND m.deleted=0
                    WHERE m.conversation_id=c.id ORDER BY m.id DESC LIMIT 1)),
                'unread',(SELECT count(*) FROM messages m WHERE m.conversation_id=c.id AND m.deleted=0
                      AND m.id>max(c.last_read_message_id,c.joined_message_id)
                      AND m.sender_id<>?1)
            )) FROM visible c LEFT JOIN users u ON u.id=c.peer
                LEFT JOIN user_avatars avatar ON avatar.user_id=u.id)),
            'next',json(CASE WHEN (SELECT count(*) FROM page)>50 THEN
                (SELECT json_object('pinned',json(CASE WHEN pinned THEN 'true' ELSE 'false' END),
                                    'activity',activity,'id',id)
                 FROM visible ORDER BY pinned,activity,id LIMIT 1) END))
    )SQL",
                            user,
                            before ? static_cast<int>(before->pinned) : 2,
                            before ? before->activity : max,
                            before ? before->id : max);
    if (!query)
    {
        return std::unexpected(std::move(query.error()));
    }
    auto row = query->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    return std::string(query->column_text(0));
}
