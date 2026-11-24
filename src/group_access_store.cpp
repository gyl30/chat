#include "group_access_store.hpp"

#include <array>
#include <limits>
#include <utility>

#include <openssl/rand.h>
#include <sqlite3.h>

#include <chat/invite.hpp>

#include "timestamp.hpp"

namespace
{
std::expected<std::optional<group_member_rejection>, sqlite_error> check_group_manager(sqlite_database& db, std::int64_t group, std::int64_t actor)
{
    auto role = read_group_role(db, group, actor);
    if (!role)
    {
        return std::unexpected(std::move(role.error()));
    }
    if (!*role)
    {
        return group_member_rejection::unavailable;
    }
    if (**role == chat::member_role::member)
    {
        return group_member_rejection::permission_denied;
    }
    return std::nullopt;
}

template <class... Args>
std::expected<bool, sqlite_error> write(sqlite_database& db, std::string_view sql, Args const&... args)
{
    if (auto error = db.exec(sql, args...); error)
    {
        return std::unexpected(std::move(error));
    }
    return db.changes() != 0;
}

constexpr std::string_view kJoinGroup = R"SQL(
    INSERT INTO conversation_members(conversation_id,user_id,joined_message_id)
    VALUES(?1,?2,(SELECT coalesce(max(id),0) FROM messages WHERE conversation_id=?1))
    ON CONFLICT(conversation_id,user_id) DO NOTHING
)SQL";
constexpr std::string_view kConsumeRequest = "DELETE FROM group_join_requests WHERE conversation_id=?1 AND user_id=?2";

std::expected<std::string, sqlite_error> make_invite()
{
    std::string token;
    constexpr auto alphabet = chat::invite_code_alphabet;
    constexpr auto limit = 256 - 256 % alphabet.size();
    while (token.size() < chat::invite_code_length)
    {
        std::array<unsigned char, 16> bytes;
        if (RAND_bytes(bytes.data(), bytes.size()) != 1)
        {
            return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot generate invite code"});
        }
        for (auto const byte : bytes)
        {
            if (byte < limit && token.size() < chat::invite_code_length)
            {
                token.push_back(alphabet[byte % alphabet.size()]);
            }
        }
    }
    return token;
}
}    // namespace

std::expected<std::variant<group_invite_change, group_member_rejection>, sqlite_error> change_group_invite(sqlite_database& db,
                                                                                                           std::int64_t group,
                                                                                                           std::int64_t actor,
                                                                                                           invite_action action)
{
    auto tx = action == invite_action::inspect ? sqlite_transaction::begin_read(db) : sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(std::move(tx.error()));
    }
    auto denied = check_group_manager(db, group, actor);
    if (!denied)
    {
        return std::unexpected(std::move(denied.error()));
    }
    if (*denied)
    {
        return **denied;
    }

    group_invite_change result;
    {
        auto current = db.prepare("SELECT invite_token FROM conversations WHERE id=?1", group);
        if (!current)
        {
            return std::unexpected(std::move(current.error()));
        }
        auto row = current->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (*row && !current->column_is_null(0))
        {
            result.token.emplace(current->column_text(0));
        }
    }
    if (action == invite_action::create && !result.token)
    {
        for (int attempt = 0; attempt < 5 && !result.token; ++attempt)
        {
            auto candidate = make_invite();
            if (!candidate)
            {
                return std::unexpected(std::move(candidate.error()));
            }
            auto stored = write(db, "UPDATE OR IGNORE conversations SET invite_token=?2 WHERE id=?1", group, *candidate);
            if (!stored)
            {
                return std::unexpected(std::move(stored.error()));
            }
            if (*stored)
            {
                result = {std::move(*candidate), true};
            }
        }
        if (!result.token)
        {
            return std::unexpected(sqlite_error{SQLITE_CONSTRAINT_UNIQUE, "could not allocate a new invite code"});
        }
    }
    else if (action == invite_action::revoke && result.token)
    {
        auto removed = write(db, "UPDATE conversations SET invite_token=NULL WHERE id=?1", group);
        if (!removed)
        {
            return std::unexpected(std::move(removed.error()));
        }
        result = {std::nullopt, *removed};
    }
    if (auto error = tx->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<std::optional<group_join_change>, sqlite_error> join_with_invite(sqlite_database& db, std::int64_t actor, std::string_view token)
{
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(std::move(tx.error()));
    }
    group_join_change result;
    bool approval;
    {
        auto group = db.prepare(R"SQL(
            SELECT id,title,join_approval,
                (SELECT count(*) FROM conversation_members WHERE conversation_id=c.id),
                EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=c.id AND user_id=?2)
            FROM conversations c WHERE kind='group' AND invite_token=?1
        )SQL",
                                token,
                                actor);
        if (!group)
        {
            return std::unexpected(std::move(group.error()));
        }
        auto row = group->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return std::nullopt;
        }
        result.result.conversation = group->column_int64(0);
        result.result.title = group->column_text(1);
        approval = group->column_int64(2) != 0;
        result.result.member_count = group->column_int64(3);
        if (group->column_int64(4))
        {
            return result;
        }
    }
    auto const id = result.result.conversation;
    if (approval)
    {
        auto const now = epoch_milliseconds();
        auto requested = write(db,
                               R"SQL(
            INSERT INTO group_join_requests(conversation_id,user_id,created_at) VALUES(?1,?2,?3)
            ON CONFLICT(conversation_id,user_id) DO NOTHING
        )SQL",
                               id,
                               actor,
                               now);
        if (!requested)
        {
            return std::unexpected(std::move(requested.error()));
        }
        result.result.state = chat::group_join_state::pending;
        result.request_changed = *requested;
    }
    else
    {
        auto joined = write(db, kJoinGroup, id, actor);
        if (!joined)
        {
            return std::unexpected(std::move(joined.error()));
        }
        auto cleared = write(db, kConsumeRequest, id, actor);
        if (!cleared)
        {
            return std::unexpected(std::move(cleared.error()));
        }
        result.result.state = chat::group_join_state::joined;
        result.result.member_count += 1;
        result.request_changed = *cleared;
    }
    if (auto error = tx->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> set_group_join_approval(sqlite_database& db,
                                                                                                std::int64_t group,
                                                                                                std::int64_t actor,
                                                                                                bool required)
{
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(std::move(tx.error()));
    }
    auto denied = check_group_manager(db, group, actor);
    if (!denied)
    {
        return std::unexpected(std::move(denied.error()));
    }
    if (*denied)
    {
        return **denied;
    }
    auto updated = write(db, "UPDATE conversations SET join_approval=?2 WHERE id=?1 AND join_approval<>?2", group, static_cast<int>(required));
    if (!updated)
    {
        return std::unexpected(std::move(updated.error()));
    }
    if (auto error = tx->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return *updated;
}

std::expected<std::variant<chat::group_join_requests_result, group_member_rejection>, sqlite_error> read_group_join_requests(
    sqlite_database& db, std::int64_t group, std::int64_t actor, std::optional<std::int64_t> before)
{
    auto tx = sqlite_transaction::begin_read(db);
    if (!tx)
    {
        return std::unexpected(std::move(tx.error()));
    }
    auto denied = check_group_manager(db, group, actor);
    if (!denied)
    {
        return std::unexpected(std::move(denied.error()));
    }
    if (*denied)
    {
        return **denied;
    }
    auto page = db.prepare(R"SQL(
        SELECT u.id,u.username,u.avatar_revision,a.user_id IS NOT NULL,r.created_at,u.nickname
        FROM group_join_requests r JOIN users u ON u.id=r.user_id
        LEFT JOIN user_avatars a ON a.user_id=u.id
        WHERE r.conversation_id=?1 AND r.user_id<?2 ORDER BY r.user_id DESC LIMIT 51
    )SQL",
                           group,
                           before.value_or(std::numeric_limits<std::int64_t>::max()));
    if (!page)
    {
        return std::unexpected(std::move(page.error()));
    }
    chat::group_join_requests_result result;
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
        if (result.requests.size() == 50)
        {
            result.next = result.requests.back().applicant.id;
            break;
        }
        result.requests.push_back({{page->column_int64(0),
                                    std::string(page->column_text(1)),
                                    {page->column_int64(2), page->column_int64(3) != 0},
                                    std::string(page->column_text(5))},
                                   page->column_int64(4)});
    }
    if (auto error = tx->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> respond_to_group_join_request(
    sqlite_database& db, std::int64_t group, std::int64_t actor, std::int64_t applicant, bool accept)
{
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(std::move(tx.error()));
    }
    auto denied = check_group_manager(db, group, actor);
    if (!denied)
    {
        return std::unexpected(std::move(denied.error()));
    }
    if (*denied)
    {
        return **denied;
    }
    auto removed = write(db, kConsumeRequest, group, applicant);
    if (!removed)
    {
        return std::unexpected(std::move(removed.error()));
    }
    if (*removed && accept)
    {
        auto joined = write(db, kJoinGroup, group, applicant);
        if (!joined)
        {
            return std::unexpected(std::move(joined.error()));
        }
    }
    if (auto error = tx->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return *removed;
}
