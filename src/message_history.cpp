#include "message_history.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
constexpr std::size_t kPageSize = 50;

std::optional<std::int64_t> optional_integer(sqlite_statement const& row, int column)
{
    return row.column_is_null(column) ? std::nullopt : std::optional(row.column_int64(column));
}

std::expected<void, sqlite_error> populate_interactions(
    sqlite_database& db, std::int64_t conversation, std::vector<chat::message>& messages)
{
    if (messages.empty())
    {
        return {};
    }
    // The history page contains every message of this conversation between these IDs,
    // including tombstones. Exclude the lookahead row and messages in other conversations.
    auto const first = messages.front().id;
    auto const last = messages.back().id;
    auto reactions = db.prepare(R"SQL(
        SELECT m.id,r.emoji,r.user_id
        FROM messages m JOIN message_reactions r ON r.message_id=m.id
        WHERE m.conversation_id=?1 AND m.id BETWEEN ?2 AND ?3
        ORDER BY m.id,r.emoji,r.user_id
    )SQL", conversation, first, last);
    if (!reactions)
    {
        return std::unexpected(std::move(reactions.error()));
    }
    for (;;)
    {
        auto row = reactions->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            break;
        }
        auto& message = *std::ranges::lower_bound(messages, reactions->column_int64(0), {}, &chat::message::id);
        auto const emoji = reactions->column_text(1);
        if (message.reactions.empty() || message.reactions.back().emoji != emoji)
        {
            message.reactions.push_back({std::string(emoji), {}});
        }
        message.reactions.back().users.push_back(reactions->column_int64(2));
    }

    auto mentions = db.prepare(R"SQL(
        SELECT m.id,u.id,u.username
        FROM messages m JOIN message_mentions mention ON mention.message_id=m.id
        JOIN users u ON u.id=mention.user_id
        WHERE m.conversation_id=?1 AND m.id BETWEEN ?2 AND ?3
        ORDER BY m.id,u.id
    )SQL", conversation, first, last);
    if (!mentions)
    {
        return std::unexpected(std::move(mentions.error()));
    }
    for (;;)
    {
        auto row = mentions->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return {};
        }
        auto& message = *std::ranges::lower_bound(messages, mentions->column_int64(0), {}, &chat::message::id);
        message.mentions.push_back({mentions->column_int64(1), std::string(mentions->column_text(2))});
    }
}

chat::message read_message_row(sqlite_statement const& row, std::int64_t user, std::int64_t conversation)
{
    chat::message message;
    message.id = row.column_int64(0);
    message.conversation = conversation;
    message.from = row.column_int64(1);
    message.username = row.column_text(2);
    message.avatar = {row.column_int64(3), row.column_int64(4) != 0};
    message.text = row.column_text(5);
    message.timestamp = row.column_int64(6);
    message.edited_at = optional_integer(row, 7);
    message.deleted = row.column_int64(8) != 0;
    message.reaction_revision = row.column_int64(9);
    if (user == message.from)
    {
        message.client_id = optional_integer(row, 10);
    }
    if (!row.column_is_null(11))
    {
        message.reply = chat::quoted_message{
            row.column_int64(11), row.column_int64(12), std::string(row.column_text(13)),
            std::string(row.column_text(14)), optional_integer(row, 15), row.column_int64(16) != 0};
    }
    if (!row.column_is_null(17))
    {
        message.attachment = chat::attachment_info{
            std::string(row.column_text(17)), std::string(row.column_text(18)), row.column_int64(19)};
    }
    return message;
}
} // namespace

std::expected<std::optional<chat::messages_result>, sqlite_error> read_message_history(
    sqlite_database& db, std::int64_t user, std::int64_t conversation,
    std::optional<std::int64_t> before, std::optional<std::int64_t> after)
{
    auto snapshot = sqlite_transaction::begin_read(db);
    if (!snapshot)
    {
        return std::unexpected(std::move(snapshot.error()));
    }
    auto readers = db.prepare(
        "SELECT user_id,last_read_message_id FROM conversation_members WHERE conversation_id=?1 ORDER BY user_id",
        conversation);
    if (!readers)
    {
        return std::unexpected(std::move(readers.error()));
    }

    chat::messages_result result;
    bool member = false;
    for (;;)
    {
        auto row = readers->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            break;
        }
        auto const reader = readers->column_int64(0);
        member = member || reader == user;
        result.read_positions.push_back({reader, readers->column_int64(1)});
    }
    if (!member)
    {
        return std::nullopt;
    }

    std::string query = R"SQL(
        SELECT m.id,m.sender_id,u.username,u.avatar_revision,avatar.user_id IS NOT NULL,
               m.body,m.created_at,m.edited_at,m.deleted,m.reaction_revision,m.client_id,
               r.id,r.sender_id,author.username,substr(r.body,1,160),r.edited_at,r.deleted,
               a.filename,a.media_type,a.size
        FROM messages m JOIN users u ON u.id=m.sender_id
        LEFT JOIN user_avatars avatar ON avatar.user_id=u.id
        LEFT JOIN messages r ON r.id=m.reply_to_id
        LEFT JOIN users author ON author.id=r.sender_id
        LEFT JOIN message_attachments a ON a.message_id=m.id AND m.deleted=0
        WHERE m.conversation_id=?1 AND
    )SQL";
    query += after ? "m.id>?2 ORDER BY m.id ASC" : "m.id<?2 ORDER BY m.id DESC";
    query += " LIMIT ?3";
    auto page = db.prepare(query, conversation, after.value_or(before.value_or(std::numeric_limits<std::int64_t>::max())),
                           static_cast<std::int64_t>(kPageSize + 1));
    if (!page)
    {
        return std::unexpected(std::move(page.error()));
    }

    for (;;)
    {
        auto row = page->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            break;
        }
        if (result.messages.size() == kPageSize)
        {
            result.has_more = true;
            break;
        }
        result.messages.push_back(read_message_row(*page, user, conversation));
    }
    if (!after)
    {
        std::ranges::reverse(result.messages);
    }
    if (auto populated = populate_interactions(db, conversation, result.messages); !populated)
    {
        return std::unexpected(std::move(populated.error()));
    }
    if (auto error = snapshot->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<std::optional<std::int64_t>, sqlite_error> update_read_position(
    sqlite_database& db, std::int64_t user, std::int64_t conversation, std::int64_t message)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    auto update = db.prepare(R"SQL(
        UPDATE conversation_members SET last_read_message_id=max(last_read_message_id,?3)
        WHERE conversation_id=?1 AND user_id=?2
          AND EXISTS(SELECT 1 FROM messages WHERE conversation_id=?1 AND id=?3)
        RETURNING last_read_message_id
    )SQL", conversation, user, message);
    if (!update)
    {
        return std::unexpected(std::move(update.error()));
    }
    auto row = update->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row)
    {
        return std::nullopt;
    }
    auto const position = update->column_int64(0);
    if (auto end = update->step(); !end)
    {
        return std::unexpected(std::move(end.error()));
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return position;
}
