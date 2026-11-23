#include "login_session_store.hpp"

#include <array>
#include <span>
#include <utility>

#include <openssl/rand.h>
#include <openssl/sha.h>

namespace
{
std::string hex_bytes(std::span<unsigned char const> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto const byte : bytes)
    {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
}    // namespace

std::string login_digest(std::string_view secret)
{
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<unsigned char const*>(secret.data()), secret.size(), digest.data());
    return hex_bytes(digest);
}

std::optional<std::string> new_login_token()
{
    std::array<unsigned char, 32> token{};
    if (RAND_bytes(token.data(), token.size()) != 1)
    {
        return std::nullopt;
    }
    return hex_bytes(token);
}

bool valid_login_token(std::string_view token) { return token.size() == 64 && token.find_first_not_of("0123456789abcdef") == std::string_view::npos; }

std::expected<std::optional<active_login>, sqlite_error> create_login(sqlite_database& db,
                                                                      chat::user const& user,
                                                                      std::string_view password_hash,
                                                                      std::string_view digest,
                                                                      std::string_view installation,
                                                                      std::string_view device_name,
                                                                      std::string_view client_type,
                                                                      std::int64_t now)
{
    // The password worker verified this exact hash. Do not issue a session if a password change won the race.
    if (auto error = db.exec(R"SQL(
        INSERT INTO login_sessions(user_id,token_hash,installation,device_name,client_type,created_at,last_seen)
        SELECT id,?3,?4,?5,?6,?7,?7 FROM users WHERE id=?1 AND password_hash=?2
    )SQL",
                             user.id,
                             password_hash,
                             digest,
                             installation,
                             device_name,
                             client_type,
                             now);
        error)
    {
        return std::unexpected(std::move(error));
    }
    if (!db.changes())
    {
        return std::nullopt;
    }
    return active_login{db.last_insert_rowid(), now, user};
}

std::expected<std::optional<active_login>, sqlite_error> find_login(sqlite_database& db,
                                                                    std::string_view digest,
                                                                    std::string_view installation,
                                                                    std::int64_t now)
{
    auto query = db.prepare(R"SQL(
        SELECT s.id,s.created_at,u.id,u.username,u.avatar_revision,a.user_id IS NOT NULL
        FROM login_sessions s JOIN users u ON u.id=s.user_id
        LEFT JOIN user_avatars a ON a.user_id=u.id
        WHERE s.token_hash=?1 AND s.installation=?2 AND s.revoked_at IS NULL AND s.created_at>?3
    )SQL",
                            digest,
                            installation,
                            now - kLoginLifetimeMs);
    if (!query)
    {
        return std::unexpected(query.error());
    }
    auto row = query->step();
    if (!row)
    {
        return std::unexpected(row.error());
    }
    if (!*row)
    {
        return std::nullopt;
    }
    return active_login{query->column_int64(0),
                        query->column_int64(1),
                        {query->column_int64(2), std::string(query->column_text(3)), {query->column_int64(4), query->column_int64(5) != 0}}};
}

std::expected<bool, sqlite_error> login_active(sqlite_database& db, std::int64_t id, std::int64_t now)
{
    auto query = db.prepare("SELECT 1 FROM login_sessions WHERE id=?1 AND revoked_at IS NULL AND created_at>?2", id, now - kLoginLifetimeMs);
    if (!query)
    {
        return std::unexpected(query.error());
    }
    return query->step();
}

std::expected<void, sqlite_error> touch_login(sqlite_database& db, std::int64_t id, std::int64_t now)
{
    if (auto error = db.exec("UPDATE login_sessions SET last_seen=?2 WHERE id=?1 AND last_seen<=?3 AND revoked_at IS NULL AND created_at>?4",
                             id,
                             now,
                             now - kLoginActivityIntervalMs,
                             now - kLoginLifetimeMs);
        error)
    {
        return std::unexpected(std::move(error));
    }
    return {};
}

std::expected<std::vector<chat::login_session>, sqlite_error> list_logins(sqlite_database& db,
                                                                          std::int64_t user,
                                                                          std::int64_t current,
                                                                          std::int64_t now)
{
    auto query = db.prepare(R"SQL(
        SELECT id,device_name,client_type,created_at,last_seen,id=?2
        FROM login_sessions WHERE user_id=?1 AND revoked_at IS NULL AND created_at>?3
        ORDER BY id=?2 DESC,last_seen DESC,id DESC
    )SQL",
                            user,
                            current,
                            now - kLoginLifetimeMs);
    if (!query)
    {
        return std::unexpected(query.error());
    }
    std::vector<chat::login_session> result;
    for (;;)
    {
        auto row = query->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            return result;
        }
        result.push_back({query->column_int64(0),
                          std::string(query->column_text(1)),
                          std::string(query->column_text(2)),
                          query->column_int64(3),
                          query->column_int64(4),
                          query->column_int64(5) != 0});
    }
}

std::expected<std::vector<std::int64_t>, sqlite_error> revoke_logins(
    sqlite_database& db, std::int64_t user, std::int64_t current, std::optional<std::int64_t> target, std::int64_t now)
{
    // A specific target includes explicit logout. With no target, never revoke the caller.
    auto query = db.prepare(R"SQL(
        UPDATE login_sessions SET revoked_at=?4 WHERE user_id=?1 AND revoked_at IS NULL AND created_at>?5
          AND CASE WHEN ?3 IS NULL THEN id<>?2 ELSE id=?3 END RETURNING id
    )SQL",
                            user,
                            current,
                            target,
                            now,
                            now - kLoginLifetimeMs);
    if (!query)
    {
        return std::unexpected(query.error());
    }
    std::vector<std::int64_t> result;
    for (;;)
    {
        auto row = query->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            return result;
        }
        result.push_back(query->column_int64(0));
    }
}

std::expected<std::optional<std::vector<std::int64_t>>, sqlite_error> change_login_password(
    sqlite_database& db, std::int64_t user, std::int64_t current, std::string_view previous, std::string_view replacement, std::int64_t now)
{
    auto transaction = sqlite_transaction::begin_immediate(db);
    if (!transaction)
    {
        return std::unexpected(transaction.error());
    }
    if (auto error = db.exec("UPDATE users SET password_hash=?3 WHERE id=?1 AND password_hash=?2", user, previous, replacement); error)
    {
        return std::unexpected(std::move(error));
    }
    if (!db.changes())
    {
        return std::nullopt;
    }
    auto revoked = revoke_logins(db, user, current, {}, now);
    if (!revoked)
    {
        return std::unexpected(revoked.error());
    }
    if (auto error = transaction->commit(); error)
    {
        return std::unexpected(std::move(error));
    }
    return std::move(*revoked);
}
