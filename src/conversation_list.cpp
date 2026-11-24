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
            SELECT c.*, own.folder_id, own.is_admin, own.pinned, own.notification_level, own.mute_until, own.unread_mark_message_id, own.last_read_message_id, own.visible_after,
                   CASE WHEN c.direct_user_low=?1 THEN c.direct_user_high ELSE c.direct_user_low END AS peer
            FROM listed_conversation_members own JOIN conversations c ON c.id=own.conversation_id
            WHERE own.user_id=?1
              AND (own.pinned,c.activity,c.id)<(?2,?3,?4)
            ORDER BY own.pinned DESC,c.activity DESC,c.id DESC LIMIT 51
        ), visible AS (
            SELECT * FROM page ORDER BY pinned DESC,activity DESC,id DESC LIMIT 50
        )
        SELECT json_object(
            'folders',json((SELECT json_group_array(json_object('id',id,'name',name)) FROM
                (SELECT id,name FROM conversation_folders WHERE owner_id=?1 AND (SELECT count(*) FROM page)<=50 ORDER BY position,id))),
            'conversations', json((SELECT json_group_array(json_object(
                'id',c.id,'folder',coalesce(c.folder_id,0),'kind',c.kind,'user',c.peer,'username',coalesce(c.title,u.username),
                'nickname',coalesce(u.nickname,''),'remark',coalesce(ct.remark,''),'avatar_revision',coalesce(u.avatar_revision,c.avatar_revision),
                'has_avatar',json(CASE WHEN avatar.user_id IS NULL AND ga.conversation_id IS NULL THEN 'false' ELSE 'true' END),
                'activity',c.activity,'notification_level',c.notification_level,'mute_until',c.mute_until,
                'marked_unread',json(CASE WHEN c.unread_mark_message_id IS NULL THEN 'false' ELSE 'true' END),
                'pinned',json(CASE WHEN c.pinned THEN 'true' ELSE 'false' END),
                'member_count',(SELECT count(*) FROM conversation_members WHERE conversation_id=c.id),
                 'announcement',c.announcement,'description',c.description,
                'speech_role',CASE WHEN c.owner_id=?1 THEN 2 WHEN c.is_admin THEN 1 ELSE 0 END,
                'group_muted',json(CASE WHEN g.conversation_id IS NOT NULL THEN 'true' ELSE 'false' END),
                'speech_until',coalesce(p.until,0),
                'server_time',CAST(unixepoch('subsec')*1000 AS INTEGER),
                'join_approval',json(CASE WHEN c.join_approval THEN 'true' ELSE 'false' END),
                'pinned_message',json((SELECT json_object('id',pm.id,'from',pm.sender_id,'username',pu.username,'nickname',pu.nickname,
                    'text',substr(pm.body,1,160),'edited_at',pm.edited_at,'deleted',json('false'))
                    FROM readable_messages pm JOIN users pu ON pu.id=pm.sender_id
                    WHERE pm.id=c.pinned_message_id AND pm.conversation_id=c.id AND pm.deleted=0 AND pm.id>c.visible_after)),
                'last',json((SELECT json_object('id',m.id,'conversation',m.conversation_id,'from',m.sender_id,
                    'username',author.username,'nickname',author.nickname,'timestamp',m.created_at,'text',m.body,'forwarded_from',coalesce((SELECT coalesce(nullif(nickname,''),username) FROM users WHERE id=m.original_sender AND m.deleted=0),''),'edited_at',m.edited_at,
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
                        'id',r.id,'from',r.sender_id,'username',ra.username,'nickname',ra.nickname,'text',substr(r.body,1,160),'edited_at',r.edited_at,
                        'deleted',json(CASE WHEN r.deleted THEN 'true' ELSE 'false' END)) END,
                    'attachment',CASE WHEN ref.message_id IS NULL THEN NULL ELSE json_object(
                        'filename',a.filename,'media_type',a.media_type,'size',a.size,'object_key',a.object_key) END)
                    FROM readable_messages m JOIN users author ON author.id=m.sender_id
                    LEFT JOIN readable_messages r ON r.id=m.reply_to_id AND r.id>c.visible_after
                    LEFT JOIN users ra ON ra.id=r.sender_id
                    LEFT JOIN user_avatars sender_avatar ON sender_avatar.user_id=author.id
                    LEFT JOIN message_attachments ref ON ref.message_id=m.id AND m.deleted=0
    LEFT JOIN files a ON a.object_key=ref.object_key
                    WHERE m.conversation_id=c.id AND m.id>c.visible_after ORDER BY m.id DESC LIMIT 1)),
                'unread',(SELECT count(*) FROM messages m WHERE m.conversation_id=c.id AND m.deleted=0
                      AND m.id>max(c.last_read_message_id,c.visible_after)
                      AND m.sender_id<>?1)
            )) FROM visible c LEFT JOIN users u ON u.id=c.peer LEFT JOIN contacts ct ON ct.owner_id=?1 AND ct.contact_id=c.peer
                LEFT JOIN user_avatars avatar ON avatar.user_id=u.id
                LEFT JOIN group_avatars ga ON ga.conversation_id=c.id
                LEFT JOIN group_mutes g ON g.conversation_id=c.id
                LEFT JOIN group_member_mutes p ON p.conversation_id=c.id AND p.user_id=?1)),
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
