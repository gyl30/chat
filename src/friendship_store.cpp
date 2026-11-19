#include "friendship_store.hpp"

#include <utility>

#include "timestamp.hpp"

namespace
{
using chat::friendship_state;

chat::user read_user(sqlite_statement const& row)
{
    return {row.column_int64(0), std::string(row.column_text(1)), {row.column_int64(2), row.column_int64(3) != 0}};
}
}    // namespace

std::expected<std::variant<friendship_change, friendship_rejection>, sqlite_error> change_friendship(sqlite_database& db,
                                                                                                     std::int64_t caller,
                                                                                                     std::int64_t peer,
                                                                                                     friendship_action action)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(std::move(transaction.error()));
    }

    friendship_change change;
    {
        auto snapshot = db.prepare(R"SQL(
            SELECT u.id, u.username, u.avatar_revision, EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id),
                   EXISTS(SELECT 1 FROM contacts WHERE owner_id=?1 AND contact_id=?2),
                   (SELECT requester_id FROM friend_requests WHERE (requester_id=?1 AND recipient_id=?2)
                                                            OR (requester_id=?2 AND recipient_id=?1)),
                   (SELECT id FROM conversations WHERE kind='direct' AND direct_user_low=min(?1,?2)
                                                                    AND direct_user_high=max(?1,?2))
            FROM users u WHERE u.id=?2 AND EXISTS(SELECT 1 FROM users WHERE id=?1)
        )SQL",
                                   caller,
                                   peer);
        if (!snapshot)
        {
            return std::unexpected(std::move(snapshot.error()));
        }
        auto row = snapshot->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return friendship_rejection::user_not_found;
        }
        change.result.user = read_user(*snapshot);
        change.direct_conversation = snapshot->column_int64(6);
        auto const requester = snapshot->column_int64(5);
        change.previous = friendship_state::none;
        if (snapshot->column_int64(4) != 0)
        {
            change.previous = friendship_state::accepted;
        }
        else if (requester != 0)
        {
            change.previous = requester == caller ? friendship_state::outgoing_pending : friendship_state::incoming_pending;
        }
    }
    change.result.state = change.previous;

    switch (action)
    {
        case friendship_action::request:
            if (change.previous == friendship_state::accepted)
            {
                return friendship_rejection::already_friends;
            }
            if (change.previous == friendship_state::none)
            {
                if (auto error = db.exec(
                        "INSERT INTO friend_requests(requester_id,recipient_id,created_at) VALUES(?1,?2,?3)", caller, peer, epoch_milliseconds());
                    error)
                {
                    return std::unexpected(std::move(error));
                }
                change.result.state = friendship_state::outgoing_pending;
            }
            break;
        case friendship_action::accept:
        case friendship_action::reject:
            if (change.previous != friendship_state::incoming_pending)
            {
                return friendship_rejection::no_incoming_request;
            }
            if (auto error = db.exec("DELETE FROM friend_requests WHERE requester_id=?2 AND recipient_id=?1", caller, peer); error)
            {
                return std::unexpected(std::move(error));
            }
            change.result.state = friendship_state::none;
            if (action == friendship_action::accept)
            {
                if (auto error = db.exec(
                        "INSERT INTO contacts(owner_id,contact_id,created_at) VALUES(?1,?2,?3),(?2,?1,?3)", caller, peer, epoch_milliseconds());
                    error)
                {
                    return std::unexpected(std::move(error));
                }
                change.result.state = friendship_state::accepted;
            }
            break;
        case friendship_action::cancel:
            if (change.previous == friendship_state::outgoing_pending)
            {
                if (auto error = db.exec("DELETE FROM friend_requests WHERE requester_id=?1 AND recipient_id=?2", caller, peer); error)
                {
                    return std::unexpected(std::move(error));
                }
                change.result.state = friendship_state::none;
            }
            break;
        case friendship_action::remove:
            if (auto error = db.exec("DELETE FROM friend_requests WHERE (requester_id=?1 AND recipient_id=?2)"
                                     " OR (requester_id=?2 AND recipient_id=?1)",
                                     caller,
                                     peer);
                error)
            {
                return std::unexpected(std::move(error));
            }
            if (auto error = db.exec("DELETE FROM contacts WHERE (owner_id=?1 AND contact_id=?2)"
                                     " OR (owner_id=?2 AND contact_id=?1)",
                                     caller,
                                     peer);
                error)
            {
                return std::unexpected(std::move(error));
            }
            change.result.state = friendship_state::none;
            break;
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return change;
}

std::expected<chat::friend_requests_result, sqlite_error> read_friend_requests(sqlite_database& db, std::int64_t user)
{
    auto statement = db.prepare(R"SQL(
        SELECT u.id, u.username, u.avatar_revision, EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id),
               f.created_at, f.requester_id=?1
        FROM friend_requests f JOIN users u ON u.id=CASE WHEN f.requester_id=?1 THEN f.recipient_id ELSE f.requester_id END
        WHERE f.requester_id=?1 OR f.recipient_id=?1 ORDER BY f.created_at DESC, u.id DESC
    )SQL",
                                user);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    chat::friend_requests_result result;
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
        auto& requests = statement->column_int64(5) ? result.outgoing : result.incoming;
        requests.push_back({read_user(*statement), statement->column_int64(4)});
    }
}

std::expected<std::vector<chat::user>, sqlite_error> read_contacts(sqlite_database& db, std::int64_t user)
{
    auto statement = db.prepare(R"SQL(
        SELECT u.id, u.username, u.avatar_revision, EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id)
        FROM contacts c JOIN users u ON u.id=c.contact_id WHERE c.owner_id=?1 ORDER BY u.username, u.id
    )SQL",
                                user);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<chat::user> result;
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
        result.push_back(read_user(*statement));
    }
}
