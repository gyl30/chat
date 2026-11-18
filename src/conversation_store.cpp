#include "conversation_store.hpp"

#include <chrono>
#include <utility>

namespace
{
std::int64_t timestamp()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::expected<bool, sqlite_error> all_contacts(sqlite_database& db, std::int64_t actor,
                                               std::span<std::int64_t const> people)
{
    auto statement = db.prepare("SELECT 1 FROM contacts WHERE owner_id=?1 AND contact_id=?2");
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    if (auto error = statement->bind(1, actor); error)
    {
        return std::unexpected(std::move(error));
    }
    for (auto person : people)
    {
        if (auto error = statement->bind(2, person); error)
        {
            return std::unexpected(std::move(error));
        }
        auto row = statement->step();
        if (!row || !*row)
        {
            return row;
        }
        if (auto error = statement->reset(); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    return true;
}

template<class... Args>
sqlite_error write(sqlite_database& db, std::string_view sql, Args const&... args)
{
    auto statement = db.prepare(sql, args...);
    if (!statement)
    {
        return std::move(statement.error());
    }
    auto row = statement->step();
    return row ? sqlite_error{} : std::move(row.error());
}

constexpr std::string_view kInsertMember = R"SQL(
    INSERT INTO conversation_members(conversation_id,user_id,joined_message_id) VALUES(?1,?2,?3)
    ON CONFLICT(conversation_id,user_id) DO NOTHING
)SQL";
} // namespace

std::expected<std::optional<std::int64_t>, sqlite_error> open_direct(
    sqlite_database& db, std::int64_t user, std::int64_t peer)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    auto allowed = all_contacts(db, user, std::span(&peer, 1));
    if (!allowed)
    {
        return std::unexpected(std::move(allowed.error()));
    }
    if (!*allowed)
    {
        return std::nullopt;
    }
    if (auto error = write(db, R"SQL(
        INSERT INTO conversations(kind,direct_user_low,direct_user_high,activity)
        VALUES('direct',min(?1,?2),max(?1,?2),?3)
        ON CONFLICT(direct_user_low,direct_user_high) WHERE kind='direct' DO NOTHING
    )SQL", user, peer, timestamp()); error)
    {
        return std::unexpected(std::move(error));
    }
    auto found = db.prepare(R"SQL(
        SELECT id FROM conversations
        WHERE kind='direct' AND direct_user_low=min(?1,?2) AND direct_user_high=max(?1,?2)
    )SQL", user, peer);
    if (!found)
    {
        return std::unexpected(std::move(found.error()));
    }
    auto row = found->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row)
    {
        return std::nullopt;
    }
    auto const id = found->column_int64(0);
    for (auto member : {user, peer})
    {
        if (auto error = write(db, kInsertMember, id, member, 0); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return id;
}

std::expected<std::optional<std::int64_t>, sqlite_error> create_group(
    sqlite_database& db, std::int64_t owner, std::string_view title, std::span<std::int64_t const> members)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }
    auto allowed = all_contacts(db, owner, members);
    if (!allowed)
    {
        return std::unexpected(std::move(allowed.error()));
    }
    if (!*allowed)
    {
        return std::nullopt;
    }
    if (auto error = write(db,
        "INSERT INTO conversations(kind,title,owner_id,activity) VALUES('group',?1,?2,?3)",
        title, owner, timestamp()); error)
    {
        return std::unexpected(std::move(error));
    }
    auto const id = db.last_insert_rowid();
    if (auto error = write(db, kInsertMember, id, owner, 0); error)
    {
        return std::unexpected(std::move(error));
    }
    for (auto member : members)
    {
        if (auto error = write(db, kInsertMember, id, member, 0); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return id;
}

std::expected<std::variant<bool, group_invite_rejection>, sqlite_error> invite_group_members(
    sqlite_database& db, std::int64_t group, std::int64_t actor, std::span<std::int64_t const> invitees)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }

    std::int64_t watermark = 0;
    {
        auto group_state = db.prepare(R"SQL(
            SELECT c.owner_id=?2 OR own.is_admin,
                   coalesce((SELECT max(id) FROM messages WHERE conversation_id=?1),0)
            FROM conversations c
            JOIN conversation_members own ON own.conversation_id=c.id AND own.user_id=?2
            WHERE c.id=?1 AND c.kind='group'
        )SQL", group, actor);
        if (!group_state)
        {
            return std::unexpected(std::move(group_state.error()));
        }
        auto row = group_state->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return group_invite_rejection::unavailable;
        }
        if (group_state->column_int64(0) == 0)
        {
            return group_invite_rejection::permission_denied;
        }
        watermark = group_state->column_int64(1);
    }
    auto allowed = all_contacts(db, actor, invitees);
    if (!allowed)
    {
        return std::unexpected(std::move(allowed.error()));
    }
    if (!*allowed)
    {
        return group_invite_rejection::contact_required;
    }

    bool added = false;
    for (auto invitee : invitees)
    {
        if (auto error = write(db, kInsertMember, group, invitee, watermark); error)
        {
            return std::unexpected(std::move(error));
        }
        added = added || db.changes() != 0;
        if (auto error = write(db, "DELETE FROM group_join_requests WHERE conversation_id=?1 AND user_id=?2",
                                group, invitee); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return added;
}
