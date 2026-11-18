#ifndef CHAT_SRC_USER_STORE_HPP
#define CHAT_SRC_USER_STORE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <chat/user.hpp>
#include <chat/presence.hpp>

#include "sqlite_database.hpp"

struct account_credentials
{
    chat::user user;
    std::string password_hash;
};

std::expected<std::optional<std::int64_t>, sqlite_error> register_account(
    sqlite_database& db, std::string_view username, std::string_view password_hash);

std::expected<std::optional<account_credentials>, sqlite_error> find_account(
    sqlite_database& db, std::string_view username);

std::expected<std::vector<chat::user>, sqlite_error> search_accounts(
    sqlite_database& db, std::int64_t user, std::string_view prefix);

struct avatar_chunk
{
    chat::avatar_state state;
    std::int64_t offset = 0;
    std::int64_t size = 0;
    std::string media_type;
    std::string bytes;
    bool has_more = false;
};

std::expected<std::optional<avatar_chunk>, sqlite_error> read_avatar_chunk(
    sqlite_database& db, std::int64_t user, std::int64_t revision, std::int64_t offset);

struct avatar_change
{
    chat::avatar_state state;
    bool changed = false;
};

// Callers validate the image and authorization before storing it.
std::expected<avatar_change, sqlite_error> save_avatar(
    sqlite_database& db, std::int64_t user, std::string_view media_type, std::string_view bytes);
std::expected<avatar_change, sqlite_error> clear_avatar(sqlite_database& db, std::int64_t user);

std::expected<std::vector<std::int64_t>, sqlite_error> read_avatar_recipients(sqlite_database& db, std::int64_t user);

struct presence_change
{
    std::int64_t last_seen;
    std::vector<std::int64_t> recipients;
};

std::expected<presence_change, sqlite_error> record_presence(sqlite_database& db, std::int64_t user, bool online);
std::expected<std::vector<chat::presence>, sqlite_error> read_contact_presence(sqlite_database& db, std::int64_t user);

#endif
