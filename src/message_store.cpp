#include "message_store.hpp"

#include <chrono>
#include <utility>

std::expected<std::variant<message_write, message_write_rejection>, sqlite_error> write_text_message(
    sqlite_database& db, text_message_write const& message)
{
    using enum message_write_rejection;
    if (message.client_id <= 0)
    {
        return invalid_client_id;
    }
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }

    {
        auto query = db.prepare(R"SQL(
            SELECT m.id, m.conversation_id, m.deleted,
                   EXISTS(SELECT 1 FROM message_attachments WHERE message_id=m.id),
                   EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=m.conversation_id AND user_id=?1)
            FROM messages m WHERE m.sender_id=?1 AND m.client_id=?2
        )SQL", message.sender, message.client_id);
        if (!query)
        {
            return std::unexpected(std::move(query.error()));
        }
        auto row = query->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (*row)
        {
            if (query->column_int64(1) != message.conversation ||
                (query->column_int64(2) == 0 && query->column_int64(3) != 0))
            {
                return invalid_client_id;
            }
            if (query->column_int64(4) == 0)
            {
                return unavailable;
            }
            return message_write{query->column_int64(0), false};
        }
    }

    {
        auto access = db.prepare(R"SQL(
            SELECT c.kind FROM conversations c
            JOIN conversation_members own ON c.id=own.conversation_id AND own.user_id=?1
            WHERE c.id=?2 AND (c.kind='group' OR c.direct_user_low=c.direct_user_high OR EXISTS(
                SELECT 1 FROM contacts WHERE owner_id=?1
                    AND contact_id=CASE WHEN c.direct_user_low=?1 THEN c.direct_user_high ELSE c.direct_user_low END))
        )SQL", message.sender, message.conversation);
        if (!access)
        {
            return std::unexpected(std::move(access.error()));
        }
        auto row = access->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return unavailable;
        }
    }

    if (message.reply_to)
    {
        auto quote = db.prepare("SELECT 1 FROM messages WHERE conversation_id=?1 AND id=?2 AND deleted=0",
                                message.conversation, *message.reply_to);
        if (!quote)
        {
            return std::unexpected(std::move(quote.error()));
        }
        auto row = quote->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return invalid_reply;
        }
    }

    auto const now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto insert = db.prepare(R"SQL(
        INSERT INTO messages(sender_id,conversation_id,client_id,body,reply_to_id,created_at)
        VALUES(?1,?2,?3,?4,?5,?6)
    )SQL", message.sender, message.conversation, message.client_id, message.text, message.reply_to, now);
    if (!insert)
    {
        return std::unexpected(std::move(insert.error()));
    }
    if (auto result = insert->step(); !result)
    {
        return std::unexpected(std::move(result.error()));
    }
    auto const id = db.last_insert_rowid();
    auto activity = db.prepare("UPDATE conversations SET activity=max(activity,?1) WHERE id=?2", now, message.conversation);
    if (!activity)
    {
        return std::unexpected(std::move(activity.error()));
    }
    if (auto result = activity->step(); !result)
    {
        return std::unexpected(std::move(result.error()));
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return message_write{id, true};
}
