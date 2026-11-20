#include "message_history.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

namespace
{
constexpr std::size_t kPageSize = 50;
constexpr std::string_view kMessageColumns = R"SQL(
    SELECT m.id,m.sender_id,u.username,u.avatar_revision,avatar.user_id IS NOT NULL,
           m.body,m.created_at,m.edited_at,m.deleted,m.reaction_revision,m.client_id,
           r.id,r.sender_id,author.username,substr(r.body,1,160),r.edited_at,r.deleted,
           a.filename,a.media_type,a.size,a.object_key
    FROM messages m JOIN users u ON u.id=m.sender_id
    JOIN conversation_members own ON own.conversation_id=m.conversation_id AND own.user_id=?1 AND m.id>own.joined_message_id
    LEFT JOIN user_avatars avatar ON avatar.user_id=u.id
    LEFT JOIN messages r ON r.id=m.reply_to_id AND r.id>own.joined_message_id
    LEFT JOIN users author ON author.id=r.sender_id
    LEFT JOIN message_attachments ref ON ref.message_id=m.id AND m.deleted=0
    LEFT JOIN files a ON a.object_key=ref.object_key
)SQL";

std::optional<std::int64_t> optional_integer(sqlite_statement const& row, int column)
{
    return row.column_is_null(column) ? std::nullopt : std::optional(row.column_int64(column));
}

std::expected<void, sqlite_error> populate_interactions(sqlite_database& db, std::span<chat::message> messages)
{
    if (messages.empty())
    {
        return {};
    }
    // Search pages can omit IDs between their bounds. Bind exactly the IDs
    // selected by the page, excluding any lookahead row.
    std::string ids = "[";
    for (auto const& message : messages)
    {
        if (ids.size() != 1)
        {
            ids.push_back(',');
        }
        ids.append(std::to_string(message.id));
    }
    ids.push_back(']');
    auto reactions = db.prepare(R"SQL(
        SELECT r.message_id,r.emoji,r.user_id
        FROM json_each(?1) page JOIN message_reactions r ON r.message_id=page.value
        ORDER BY r.message_id,r.emoji,r.user_id
    )SQL",
                                ids);
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
        SELECT mention.message_id,u.id,u.username
        FROM json_each(?1) page JOIN message_mentions mention ON mention.message_id=page.value
        JOIN users u ON u.id=mention.user_id
        ORDER BY mention.message_id,u.id
    )SQL",
                               ids);
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
        message.reply = chat::quoted_message{row.column_int64(11),
                                             row.column_int64(12),
                                             std::string(row.column_text(13)),
                                             std::string(row.column_text(14)),
                                             optional_integer(row, 15),
                                             row.column_int64(16) != 0};
    }
    if (!row.column_is_null(17))
    {
        message.attachment = chat::attachment_info{
            std::string(row.column_text(17)), std::string(row.column_text(18)), row.column_int64(19), std::string(row.column_text(20))};
    }
    return message;
}
}    // namespace

std::expected<std::optional<chat::messages_result>, sqlite_error> read_message_history(sqlite_database& db,
                                                                                       std::int64_t user,
                                                                                       std::int64_t conversation,
                                                                                       std::optional<std::int64_t> before,
                                                                                       std::optional<std::int64_t> after,
                                                                                       std::string_view search,
                                                                                       std::optional<std::int64_t> around)
{
    auto snapshot = sqlite_transaction::begin_read(db);
    if (!snapshot)
    {
        return std::unexpected(std::move(snapshot.error()));
    }
    auto readers = db.prepare(R"SQL(
            SELECT m.user_id,CASE WHEN m.last_read_message_id>own.joined_message_id THEN m.last_read_message_id ELSE 0 END
            FROM conversation_members own JOIN conversation_members m ON m.conversation_id=own.conversation_id
            WHERE own.conversation_id=?1 AND own.user_id=?2
              AND (?3 IS NULL OR (?3>own.joined_message_id AND EXISTS(
                  SELECT 1 FROM messages WHERE id=?3 AND conversation_id=?1 AND deleted=0)))
            ORDER BY m.user_id
        )SQL",
                              conversation,
                              user,
                              around);
    if (!readers)
    {
        return std::unexpected(std::move(readers.error()));
    }

    chat::messages_result result;
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
        result.read_positions.push_back({reader, readers->column_int64(1)});
    }
    if (result.read_positions.empty())
    {
        return std::nullopt;
    }

    // Both halves of an anchored page share the membership snapshot and the same row decoder.
    auto read_page = [&](std::string_view range, std::int64_t cursor, std::size_t limit) -> std::expected<bool, sqlite_error>
    {
        std::string query(kMessageColumns);
        query += "WHERE m.conversation_id=?2 AND (?5='' OR (m.deleted=0 AND instr(chat_lower(m.body),chat_lower(?5))>0)) AND ";
        query += range;
        query += " LIMIT ?4";
        auto page = db.prepare(query, user, conversation, cursor, static_cast<std::int64_t>(limit + 1), search);
        if (!page)
        {
            return std::unexpected(std::move(page.error()));
        }
        for (std::size_t count = 0;; ++count)
        {
            auto row = page->step();
            if (!row)
            {
                return std::unexpected(std::move(row.error()));
            }
            if (!*row)
            {
                return false;
            }
            if (count == limit)
            {
                return true;
            }
            result.messages.push_back(read_message_row(*page, user, conversation));
        }
    };
    auto more = read_page(after ? "m.id>?3 ORDER BY m.id ASC" : "m.id<?3 ORDER BY m.id DESC",
                          around.value_or(after.value_or(before.value_or(std::numeric_limits<std::int64_t>::max()))),
                          around ? 20 : kPageSize);
    if (!more)
    {
        return std::unexpected(std::move(more.error()));
    }
    result.has_more = *more;
    if (!after)
    {
        std::ranges::reverse(result.messages);
    }
    if (around)
    {
        auto newer = read_page("m.id>=?3 ORDER BY m.id ASC", *around, 21);
        if (!newer)
        {
            return std::unexpected(std::move(newer.error()));
        }
    }
    if (auto populated = populate_interactions(db, result.messages); !populated)
    {
        return std::unexpected(std::move(populated.error()));
    }
    if (auto error = snapshot->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<std::optional<chat::message>, sqlite_error> read_message(sqlite_database& db,
                                                                       std::int64_t user,
                                                                       std::int64_t conversation,
                                                                       std::int64_t message)
{
    std::string query(kMessageColumns);
    query += "WHERE m.conversation_id=?2 AND m.id=?3";
    auto statement = db.prepare(query, user, conversation, message);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    auto row = statement->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row)
    {
        return std::nullopt;
    }
    auto result = read_message_row(*statement, user, conversation);
    if (auto populated = populate_interactions(db, std::span(&result, 1)); !populated)
    {
        return std::unexpected(std::move(populated.error()));
    }
    return result;
}

std::expected<std::optional<read_position_change>, sqlite_error> update_read_position(sqlite_database& db,
                                                                                      std::int64_t user,
                                                                                      std::int64_t conversation,
                                                                                      std::int64_t message)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    auto update = db.prepare(R"SQL(
        UPDATE conversation_members SET last_read_message_id=?3
        WHERE conversation_id=?1 AND user_id=?2 AND last_read_message_id<?3
          AND ?3>joined_message_id AND EXISTS(SELECT 1 FROM messages WHERE conversation_id=?1 AND id=?3)
        RETURNING last_read_message_id
    )SQL",
                             conversation,
                             user,
                             message);
    if (!update)
    {
        return std::unexpected(std::move(update.error()));
    }
    auto row = update->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    read_position_change change{};
    if (*row)
    {
        change = {update->column_int64(0), true};
        if (auto end = update->step(); !end)
        {
            return std::unexpected(std::move(end.error()));
        }
    }
    else
    {
        // A repeated or older acknowledgement remains successful, but does not notify peers.
        update = sqlite_statement{};
        auto current = db.prepare(R"SQL(
            SELECT last_read_message_id FROM conversation_members
            WHERE conversation_id=?1 AND user_id=?2
              AND ?3>joined_message_id AND EXISTS(SELECT 1 FROM messages WHERE conversation_id=?1 AND id=?3)
        )SQL",
                                  conversation,
                                  user,
                                  message);
        if (!current)
        {
            return std::unexpected(std::move(current.error()));
        }
        auto existing = current->step();
        if (!existing)
        {
            return std::unexpected(std::move(existing.error()));
        }
        if (!*existing)
        {
            return std::nullopt;
        }
        change = {current->column_int64(0), false};
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return change;
}

std::expected<chat::message_search_page, sqlite_error> search_all_messages(sqlite_database& db,
                                                                           std::int64_t user,
                                                                           std::string_view query,
                                                                           std::optional<std::int64_t> before)
{
    // Limit authorized matches before looking up display names and building snippets.
    auto page = db.prepare(R"SQL(
        WITH hits AS MATERIALIZED (
            SELECT m.id FROM conversation_members own CROSS JOIN messages m ON m.conversation_id=own.conversation_id
            WHERE own.user_id=?1 AND m.id>own.joined_message_id AND m.id<=coalesce(?3-1,9223372036854775807)
              AND m.deleted=0 AND instr(chat_lower(m.body),chat_lower(?2))>0
            ORDER BY m.id DESC LIMIT ?4
        )
        SELECT c.id,m.id,m.sender_id,m.created_at,coalesce(c.title,peer.username),u.username,
               substr(m.body,max(1,instr(chat_lower(m.body),chat_lower(?2))-32),160)
        FROM hits JOIN messages m ON m.id=hits.id JOIN conversations c ON c.id=m.conversation_id
        JOIN users u ON u.id=m.sender_id LEFT JOIN users peer ON peer.id=CASE WHEN c.direct_user_low=?1 THEN c.direct_user_high ELSE c.direct_user_low END
        ORDER BY m.id DESC
    )SQL",
                           user,
                           query,
                           before,
                           static_cast<std::int64_t>(kPageSize + 1));
    if (!page)
    {
        return std::unexpected(std::move(page.error()));
    }
    chat::message_search_page result;
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
            result.next_before = result.messages.back().id;
            break;
        }
        result.messages.push_back({
            .conversation = page->column_int64(0),
            .id = page->column_int64(1),
            .from = page->column_int64(2),
            .timestamp = page->column_int64(3),
            .title = std::string(page->column_text(4)),
            .username = std::string(page->column_text(5)),
            .snippet = std::string(page->column_text(6)),
        });
    }
    return result;
}
