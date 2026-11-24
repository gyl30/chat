#include "message_changes.hpp"

#include <utility>

#include "conversation_store.hpp"
#include "message_history.hpp"
#include "sqlite_mentions.hpp"
#include "timestamp.hpp"

std::expected<std::variant<changed_message, message_change_rejection>, sqlite_error> change_message_body(
    sqlite_database& db, std::int64_t user, std::int64_t conversation, std::int64_t message, std::optional<std::string_view> new_text)
{
    using enum message_change_rejection;
    auto const editing = new_text.has_value();
    if (editing)
    {
        auto allowed = can_send_to_conversation(db, conversation, user);
        if (!allowed)
        {
            return std::unexpected(std::move(allowed.error()));
        }
        if (*allowed != send_access::allowed)
        {
            return *allowed == send_access::muted ? muted : cannot_send;
        }
    }
    auto const now = epoch_milliseconds();
    auto update = db.prepare(R"SQL(
        UPDATE messages AS m SET
            body=?4,archive_key=NULL,deleted=?5,
            edited_at=CASE WHEN ?5 THEN edited_at ELSE max(?6,coalesce(edited_at,0)+1) END,
            reaction_revision=reaction_revision+CASE WHEN ?5 AND deleted=0 THEN 1 ELSE 0 END
        WHERE m.id=?1 AND m.conversation_id=?2 AND m.sender_id=?3
          AND EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=?2 AND user_id=?3 AND m.id>visible_after)
          AND m.deleted=0 AND (?5 OR (m.content='message' AND m.forward_source IS NULL AND NOT EXISTS(SELECT 1 FROM message_attachments WHERE message_id=?1)))
    )SQL",
                             message,
                             conversation,
                             user,
                             new_text.value_or(""),
                             static_cast<int>(!editing),
                             now);
    if (!update)
    {
        return std::unexpected(std::move(update.error()));
    }
    if (auto done = update->step(); !done)
    {
        return std::unexpected(std::move(done.error()));
    }
    if (db.changes() == 0)
    {
        // Retrying a successful deletion must acknowledge the existing tombstone,
        // without writing it again or publishing another update.
        if (!editing)
        {
            auto existing = read_message(db, user, conversation, message);
            if (!existing)
            {
                return std::unexpected(std::move(existing.error()));
            }
            if (*existing && (*existing)->from == user && (*existing)->deleted)
            {
                return changed_message{std::move(**existing), false, false};
            }
        }
        return unavailable;
    }
    bool cleared_pin = false;
    if (!editing)
    {
        if (auto error = db.exec("DELETE FROM message_reactions WHERE message_id=?1", message); error)
        {
            return std::unexpected(std::move(error));
        }
        if (auto error = db.exec("UPDATE conversations SET pinned_message_id=NULL WHERE id=?1 AND pinned_message_id=?2", conversation, message);
            error)
        {
            return std::unexpected(std::move(error));
        }
        cleared_pin = db.changes() != 0;
    }
    if (auto error = replace_message_mentions(db, conversation, message, new_text.value_or("")); error)
    {
        return std::unexpected(std::move(error));
    }
    auto latest = read_message(db, user, conversation, message);
    if (!latest)
    {
        return std::unexpected(std::move(latest.error()));
    }
    if (!*latest)
    {
        return unavailable;
    }
    return changed_message{std::move(**latest), cleared_pin};
}

std::expected<std::variant<changed_reaction, message_change_rejection>, sqlite_error> set_message_reaction(
    sqlite_database& db, std::int64_t user, std::int64_t conversation, std::int64_t message, std::string_view emoji)
{
    using enum message_change_rejection;
    auto original = db.prepare(R"SQL(
        SELECT coalesce((SELECT emoji FROM message_reactions WHERE message_id=?1 AND user_id=?3),'')
        FROM messages m JOIN conversation_members own ON own.conversation_id=m.conversation_id
        WHERE m.id=?1 AND m.conversation_id=?2 AND own.user_id=?3 AND m.id>own.visible_after AND m.deleted=0
    )SQL",
                               message,
                               conversation,
                               user);
    if (!original)
    {
        return std::unexpected(std::move(original.error()));
    }
    auto row = original->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row)
    {
        return unavailable;
    }
    auto const changed = original->column_text(0) != emoji;
    original = sqlite_statement{};
    auto allowed = can_send_to_conversation(db, conversation, user);
    if (!allowed)
    {
        return std::unexpected(std::move(allowed.error()));
    }
    if (*allowed == send_access::unavailable || (*allowed == send_access::muted && !emoji.empty()))
    {
        return *allowed == send_access::muted ? muted : cannot_send;
    }
    if (changed)
    {
        auto error = emoji.empty() ? db.exec("DELETE FROM message_reactions WHERE message_id=?1 AND user_id=?2", message, user)
                                   : db.exec(
                                         R"SQL(
            INSERT INTO message_reactions(message_id,user_id,emoji) VALUES(?1,?2,?3)
            ON CONFLICT(message_id,user_id) DO UPDATE SET emoji=excluded.emoji
        )SQL",
                                         message,
                                         user,
                                         emoji);
        if (error)
        {
            return std::unexpected(std::move(error));
        }
        error = db.exec("UPDATE messages SET reaction_revision=reaction_revision+1 WHERE id=?1", message);
        if (error)
        {
            return std::unexpected(std::move(error));
        }
    }
    auto latest = read_message(db, user, conversation, message);
    if (!latest)
    {
        return std::unexpected(std::move(latest.error()));
    }
    if (!*latest)
    {
        return unavailable;
    }
    return changed_reaction{{conversation, message, (*latest)->reaction_revision, std::move((*latest)->reactions)}, changed};
}
