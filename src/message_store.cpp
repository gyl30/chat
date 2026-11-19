#include "message_store.hpp"
#include "conversation_store.hpp"
#include "sqlite_mentions.hpp"
#include "timestamp.hpp"

#include <sqlite3.h>
#include <utility>

std::expected<std::optional<message_write_outcome>, sqlite_error> check_message_write(sqlite_database& db,
                                                                                      message_to_write const& message,
                                                                                      bool attachment)
{
    using enum message_write_rejection;
    if (message.client_id <= 0)
    {
        return message_write_outcome{invalid_client_id};
    }
    {
        auto query = db.prepare(R"SQL(
            SELECT m.id, m.conversation_id, m.deleted,
                   EXISTS(SELECT 1 FROM message_attachments WHERE message_id=m.id),
                   EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=m.conversation_id AND user_id=?1)
            FROM messages m WHERE m.sender_id=?1 AND m.client_id=?2
        )SQL",
                                message.sender,
                                message.client_id);
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
            if (query->column_int64(1) != message.conversation || (query->column_int64(2) == 0 && ((query->column_int64(3) != 0) != attachment)))
            {
                return message_write_outcome{invalid_client_id};
            }
            if (query->column_int64(4) == 0)
            {
                return message_write_outcome{unavailable};
            }
            return message_write_outcome{message_write{query->column_int64(0), false}};
        }
    }

    {
        auto access = can_send_to_conversation(db, message.conversation, message.sender);
        if (!access)
        {
            return std::unexpected(std::move(access.error()));
        }
        if (!*access)
        {
            return message_write_outcome{unavailable};
        }
    }

    if (message.reply_to)
    {
        auto quote = db.prepare("SELECT 1 FROM messages WHERE conversation_id=?1 AND id=?2 AND deleted=0", message.conversation, *message.reply_to);
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
            return message_write_outcome{invalid_reply};
        }
    }

    return std::nullopt;
}

std::expected<message_write_outcome, sqlite_error> insert_message(sqlite_database& db,
                                                                  message_to_write const& message,
                                                                  uploaded_attachment const* attachment)
{
    auto preflight = check_message_write(db, message, attachment != nullptr);
    if (!preflight)
    {
        return std::unexpected(std::move(preflight.error()));
    }
    if (*preflight)
    {
        return **preflight;
    }

    auto const now = epoch_milliseconds();
    if (auto error = db.exec(R"SQL(
        INSERT INTO messages(sender_id,conversation_id,client_id,body,reply_to_id,created_at)
        VALUES(?1,?2,?3,?4,?5,?6)
    )SQL",
                             message.sender,
                             message.conversation,
                             message.client_id,
                             message.text,
                             message.reply_to,
                             now);
        error)
    {
        return std::unexpected(std::move(error));
    }
    auto const id = db.last_insert_rowid();
    if (!attachment && message.text.contains('@'))
    {
        if (auto error = replace_message_mentions(db, message.conversation, id, message.text); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (attachment)
    {
        if (auto error = db.exec("DELETE FROM attachment_cleanup WHERE object_key=?1 AND reserved_at>0", attachment->object_key); error)
        {
            return std::unexpected(std::move(error));
        }
        if (db.changes() != 1)
        {
            return std::unexpected(sqlite_error{SQLITE_CONSTRAINT, "attachment object has no reservation"});
        }
        if (auto error = db.exec(R"SQL(
            INSERT INTO message_attachments(message_id,object_key,filename,media_type,size,checksum,created_at)
            VALUES(?1,?2,?3,?4,?5,?6,?7)
        )SQL",
                                 id,
                                 attachment->object_key,
                                 attachment->filename,
                                 attachment->media_type,
                                 attachment->size,
                                 attachment->checksum,
                                 now);
            error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (auto error = db.exec("UPDATE conversations SET activity=max(activity,?1) WHERE id=?2", now, message.conversation); error)
    {
        return std::unexpected(std::move(error));
    }
    return message_write{id, true};
}
