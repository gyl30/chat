#include "message_store.hpp"
#include "bundle_store.hpp"
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
                   EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=m.conversation_id AND user_id=?1),coalesce(m.forward_source,0),m.content
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
            if (query->column_text(6) != message.content() || query->column_int64(1) != message.conversation ||
                query->column_int64(5) != message.forward_source.value_or(0) ||
                (!message.forward_source && query->column_int64(2) == 0 && ((query->column_int64(3) != 0) != attachment)))
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
        if (*access != send_access::allowed)
        {
            return message_write_outcome{*access == send_access::muted ? muted : unavailable};
        }
    }

    if (message.mention_all || !message.poll_options.empty())
    {
        auto role = read_group_role(db, message.conversation, message.sender);
        if (!role)
        {
            return std::unexpected(role.error());
        }
        if (!*role || (message.mention_all && **role == chat::member_role::member))
        {
            return message_write_outcome{unavailable};
        }
    }
    if (message.reply_to)
    {
        auto quote = db.prepare(R"SQL(
            SELECT 1 FROM messages m JOIN conversation_members own ON own.conversation_id=m.conversation_id
            WHERE m.conversation_id=?1 AND m.id=?2 AND m.deleted=0 AND own.user_id=?3 AND m.id>own.visible_after
        )SQL",
                                message.conversation,
                                *message.reply_to,
                                message.sender);
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

std::expected<message_write_outcome, sqlite_error> insert_message(sqlite_database& db, message_to_write const& message, std::string_view object_key)
{
    auto preflight = check_message_write(db, message, !object_key.empty());
    if (!preflight)
    {
        return std::unexpected(std::move(preflight.error()));
    }
    if (*preflight)
    {
        return **preflight;
    }

    std::string body;
    std::string source_key;
    bool markdown = message.markdown;
    std::optional<std::int64_t> original_sender;
    if (message.forward_source)
    {
        // Read the original and establish the new reference in the same write transaction.
        auto source = db.prepare(R"SQL(
            SELECT m.body,coalesce(m.original_sender,m.sender_id),ref.object_key,m.markdown FROM readable_messages m
            JOIN conversation_members own ON own.conversation_id=m.conversation_id AND own.user_id=?2
            LEFT JOIN message_attachments ref ON ref.message_id=m.id
            WHERE m.id=?1 AND m.id>own.visible_after AND NOT m.deleted AND m.content='message'
        )SQL",
                                 *message.forward_source,
                                 message.sender);
        if (!source)
        {
            return std::unexpected(source.error());
        }
        auto row = source->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            return message_write_outcome{message_write_rejection::unavailable};
        }
        body = source->column_text(0);
        original_sender = source->column_int64(1);
        source_key = source->column_text(2);
        markdown = source->column_int64(3) != 0;
        object_key = source_key;
    }
    else if (!object_key.empty())
    {
        auto q = db.prepare(R"SQL(SELECT filename FROM files WHERE object_key=?1 AND uploader_id=?2
                  AND NOT EXISTS(SELECT 1 FROM attachment_cleanup WHERE object_key=?1 AND reserved_at=0))SQL",
                            object_key,
                            message.sender);
        if (!q)
        {
            return std::unexpected(q.error());
        }
        auto row = q->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            return message_write_outcome{message_write_rejection::unavailable};
        }
        body = q->column_text(0);
    }
    auto const now = epoch_milliseconds();
    if (auto error = db.exec(R"SQL(
        INSERT INTO messages(sender_id,conversation_id,client_id,body,reply_to_id,created_at,forward_source,original_sender,markdown,mention_all,content)
        VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)
    )SQL",
                             message.sender,
                             message.conversation,
                             message.client_id,
                             message.forward_source || !object_key.empty() ? std::string_view(body) : message.text,
                             message.reply_to,
                             now,
                             message.forward_source,
                             original_sender,
                             int(markdown),
                             int(message.mention_all),
                             message.content());
        error)
    {
        return std::unexpected(std::move(error));
    }
    auto const id = db.last_insert_rowid();
    if (!message.poll_options.empty())
    {
        if (auto err = db.exec("INSERT INTO polls(message,anonymous,multiple,correct,explanation) VALUES(?,?,?,?,?)",
                               id,
                               int(message.settings.anonymous),
                               int(message.settings.multiple),
                               message.settings.correct,
                               message.settings.explanation);
            err)
        {
            return std::unexpected(err);
        }
        int pos = 0;
        for (auto const& label : message.poll_options)
        {
            if (auto err = db.exec("INSERT INTO poll_options(message,position,label) VALUES(?,?,?)", id, ++pos, label); err)
            {
                return std::unexpected(err);
            }
        }
    }
    if (!message.bundle.empty())
    {
        auto copied = store_bundle(db, id, message.sender, message.bundle);
        if (!copied)
        {
            return std::unexpected(copied.error());
        }
        if (!*copied)
        {
            return message_write_rejection::unavailable;
        }
    }
    if (message.content() == "message" && object_key.empty() && message.text.contains('@'))
    {
        if (auto error = replace_message_mentions(db, message.conversation, id, message.text); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (!object_key.empty())
    {
        if (auto e = db.exec("INSERT INTO message_attachments(message_id,object_key) VALUES(?,?)", id, object_key); e)
        {
            return std::unexpected(e);
        }
    }
    if (auto error = db.exec("UPDATE conversations SET activity=max(activity+1,?1) WHERE id=?2", now, message.conversation); error)
    {
        return std::unexpected(std::move(error));
    }
    return message_write{id, true};
}
