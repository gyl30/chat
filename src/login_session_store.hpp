#ifndef CHAT_SRC_LOGIN_SESSION_STORE_HPP
#define CHAT_SRC_LOGIN_SESSION_STORE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <chat/login_session.hpp>
#include <chat/user.hpp>

#include "sqlite_database.hpp"

inline constexpr std::int64_t kLoginLifetimeMs = INT64_C(30) * 24 * 60 * 60 * 1000;
inline constexpr std::int64_t kLoginActivityIntervalMs = 60 * 1000;

struct active_login
{
    std::int64_t id;
    std::int64_t created_at;
    chat::user user;
};

std::string login_digest(std::string_view secret);
std::optional<std::string> new_login_token();
bool valid_login_token(std::string_view token);

std::expected<std::optional<active_login>, sqlite_error> create_login(sqlite_database& db,
                                                                      chat::user const& user,
                                                                      std::string_view password_hash,
                                                                      std::string_view digest,
                                                                      std::string_view installation,
                                                                      std::string_view device_name,
                                                                      std::string_view client_type,
                                                                      std::int64_t now);
std::expected<std::optional<active_login>, sqlite_error> find_login(sqlite_database& db,
                                                                    std::string_view digest,
                                                                    std::string_view installation,
                                                                    std::int64_t now);
std::expected<bool, sqlite_error> login_active(sqlite_database& db, std::int64_t id, std::int64_t now);
std::expected<void, sqlite_error> touch_login(sqlite_database& db, std::int64_t id, std::int64_t now);
std::expected<std::vector<chat::login_session>, sqlite_error> list_logins(sqlite_database& db,
                                                                          std::int64_t user,
                                                                          std::int64_t current,
                                                                          std::int64_t now);
std::expected<std::vector<std::int64_t>, sqlite_error> revoke_logins(
    sqlite_database& db, std::int64_t user, std::int64_t current, std::optional<std::int64_t> target, std::int64_t now);
std::expected<std::optional<std::vector<std::int64_t>>, sqlite_error> change_login_password(
    sqlite_database& db, std::int64_t user, std::int64_t current, std::string_view previous, std::string_view replacement, std::int64_t now);

#endif
