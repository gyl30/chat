#include "user_store.hpp"

#include <utility>

#include <sqlite3.h>
#include <chat/avatar.hpp>

#include "timestamp.hpp"
#include "conversation_store.hpp"

std::expected<std::optional<std::int64_t>, sqlite_error> register_account(sqlite_database& db,
                                                                          std::string_view username,
                                                                          std::string_view password_hash)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    if (auto error = db.exec(R"SQL(
        INSERT INTO users(username,password_hash,last_seen_at) VALUES(?1,?2,?3)
        ON CONFLICT(username) DO NOTHING
    )SQL",
                             username,
                             password_hash,
                             epoch_milliseconds());
        error)
    {
        return std::unexpected(std::move(error));
    }
    if (db.changes() == 0)
    {
        return std::nullopt;
    }
    auto const id = db.last_insert_rowid();
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return id;
}

std::expected<std::optional<account_credentials>, sqlite_error> find_account(sqlite_database& db, std::string_view username)
{
    auto statement = db.prepare(R"SQL(
        SELECT u.id,u.username,u.password_hash,u.avatar_revision,a.user_id IS NOT NULL,u.nickname
        FROM users u LEFT JOIN user_avatars a ON a.user_id=u.id WHERE u.username=?1
    )SQL",
                                username);
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
    return account_credentials{{statement->column_int64(0),
                                std::string(statement->column_text(1)),
                                {statement->column_int64(3), statement->column_int64(4) != 0},
                                std::string(statement->column_text(5))},
                               std::string(statement->column_text(2))};
}

std::expected<std::vector<chat::user>, sqlite_error> search_accounts(sqlite_database& db, std::int64_t user, std::string_view prefix)
{
    auto statement = db.prepare(R"SQL(
        SELECT u.id,u.username,u.avatar_revision,a.user_id IS NOT NULL,u.nickname
        FROM users u LEFT JOIN user_avatars a ON a.user_id=u.id WHERE u.id<>?1
          AND (instr(chat_lower(u.username),chat_lower(?2))=1 OR instr(chat_lower(u.nickname),chat_lower(?2))>0)
        ORDER BY (u.username=?2) DESC,u.username,u.id LIMIT 20
    )SQL",
                                user,
                                prefix);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<chat::user> matches;
    for (;;)
    {
        auto row = statement->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return matches;
        }
        matches.push_back({statement->column_int64(0),
                           std::string(statement->column_text(1)),
                           {statement->column_int64(2), statement->column_int64(3) != 0},
                           std::string(statement->column_text(4))});
    }
}

std::expected<std::optional<avatar_chunk>, sqlite_error> read_avatar_chunk(
    sqlite_database& db, std::int64_t target_user, std::int64_t group, std::int64_t viewer, std::int64_t revision, std::int64_t offset)
{
    auto constexpr chunk_size = static_cast<std::int64_t>(chat::avatar_chunk_size);
    auto source = group ? R"SQL(FROM conversations u JOIN conversation_members m ON m.conversation_id=u.id AND m.user_id=?5
            LEFT JOIN group_avatars a ON a.conversation_id=u.id WHERE u.kind='group' AND u.id=?1)SQL"
                        : "FROM users u LEFT JOIN user_avatars a ON a.user_id=u.id WHERE u.id=?1";
    auto sql = std::string(R"SQL(SELECT u.avatar_revision,a.size IS NOT NULL,a.size,a.media_type,
            CASE WHEN u.avatar_revision=?2 AND ?3<a.size THEN substr(a.data,?3+1,?4) END )SQL") +
               source + " AND (u.avatar_revision<>?2 OR ?3<=coalesce(a.size,0))";
    auto statement =
        group ? db.prepare(sql, group, revision, offset, chunk_size, viewer) : db.prepare(sql, target_user, revision, offset, chunk_size);
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
    avatar_chunk result;
    result.state = {statement->column_int64(0), statement->column_int64(1) != 0};
    if (!result.state.present || result.state.revision != revision)
    {
        return result;
    }

    result.offset = offset;
    result.size = statement->column_int64(2);
    result.media_type = statement->column_text(3);
    auto const data = statement->column_blob(4);
    if (!data.empty())
    {
        result.bytes.assign(reinterpret_cast<char const*>(data.data()), data.size());
    }
    result.has_more = result.size - result.offset > chunk_size;
    return result;
}

std::expected<std::optional<avatar_change>, sqlite_error> write_avatar(
    sqlite_database& db, std::int64_t user, std::int64_t group, std::string_view media, std::string_view bytes)
{
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(tx.error());
    }
    if (group)
    {
        auto role = read_group_role(db, group, user);
        if (!role)
        {
            return std::unexpected(role.error());
        }
        if (!*role || **role == chat::member_role::member)
        {
            return std::nullopt;
        }
    }
    auto const id = group ? group : user;
    std::string const table = group ? "group_avatars" : "user_avatars";
    std::string const key = group ? "conversation_id" : "user_id";
    auto err =
        bytes.empty()
            ? db.exec("DELETE FROM " + table + " WHERE " + key + "=?1", id)
            : db.exec("INSERT INTO " + table + "(" + key + ",media_type,size,data,updated_at) VALUES(?1,?2,?3,?4,?5) ON CONFLICT(" + key +
                          ") DO UPDATE SET media_type=excluded.media_type,size=excluded.size,data=excluded.data,updated_at=excluded.updated_at",
                      id,
                      media,
                      static_cast<std::int64_t>(bytes.size()),
                      std::as_bytes(std::span(bytes.data(), bytes.size())),
                      epoch_milliseconds());
    if (err)
    {
        return std::unexpected(err);
    }
    bool const changed = db.changes() != 0;
    std::string const parent = group ? "conversations" : "users";
    auto stmt = db.prepare(changed ? "UPDATE " + parent + " SET avatar_revision=avatar_revision+1 WHERE id=?1 RETURNING avatar_revision"
                                   : "SELECT avatar_revision FROM " + parent + " WHERE id=?1",
                           id);
    if (!stmt)
    {
        return std::unexpected(stmt.error());
    }
    auto row = stmt->step();
    if (!row)
    {
        return std::unexpected(row.error());
    }
    if (!*row)
    {
        return std::nullopt;
    }
    avatar_change result{{stmt->column_int64(0), !bytes.empty()}, changed};
    if (auto done = stmt->step(); !done)
    {
        return std::unexpected(done.error());
    }
    if (auto error = tx->commit(); error)
    {
        return std::unexpected(error);
    }
    return result;
}

std::expected<std::vector<std::int64_t>, sqlite_error> read_avatar_recipients(sqlite_database& db, std::int64_t user)
{
    auto statement = db.prepare(R"SQL(
        SELECT peer FROM (
            SELECT contact_id AS peer FROM contacts WHERE owner_id=?1
            UNION SELECT m.user_id
                FROM conversation_members own
                JOIN conversations c ON c.id=own.conversation_id AND c.kind='group'
                JOIN conversation_members m ON m.conversation_id=c.id
                WHERE own.user_id=?1
        ) WHERE peer<>?1 ORDER BY peer
    )SQL",
                                user);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<std::int64_t> recipients;
    for (;;)
    {
        auto row = statement->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return recipients;
        }
        recipients.push_back(statement->column_int64(0));
    }
}

std::expected<presence_change, sqlite_error> record_presence(sqlite_database& db, std::int64_t user, bool online)
{
    auto transaction = online ? sqlite_transaction::begin_read(db) : sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    presence_change result{online ? 0 : epoch_milliseconds(), {}};
    if (!online)
    {
        if (auto error = db.exec("UPDATE users SET last_seen_at=?2 WHERE id=?1", user, result.last_seen); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    auto statement = db.prepare("SELECT owner_id FROM contacts WHERE contact_id=?1 ORDER BY owner_id", user);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    for (;;)
    {
        auto row = statement->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            break;
        }
        result.recipients.push_back(statement->column_int64(0));
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<std::vector<chat::presence>, sqlite_error> read_contact_presence(sqlite_database& db, std::int64_t user)
{
    auto statement = db.prepare(R"SQL(
        SELECT u.id,u.last_seen_at FROM contacts c JOIN users u ON u.id=c.contact_id
        WHERE c.owner_id=?1 ORDER BY u.id
    )SQL",
                                user);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<chat::presence> result;
    for (;;)
    {
        auto row = statement->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return result;
        }
        result.push_back({statement->column_int64(0), false, statement->column_int64(1)});
    }
}
