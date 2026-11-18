#ifndef CHAT_SRC_MESSAGE_STORE_HPP
#define CHAT_SRC_MESSAGE_STORE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <variant>

#include "sqlite_database.hpp"

struct message_to_write
{
    std::int64_t sender;
    std::int64_t conversation;
    std::int64_t client_id;
    std::string_view text;
    std::optional<std::int64_t> reply_to;
};

struct uploaded_attachment
{
    std::string_view object_key;
    std::string_view filename;
    std::string_view media_type;
    std::int64_t size;
    std::string_view checksum;
};

enum class message_write_rejection { unavailable, invalid_client_id, invalid_reply };

struct message_write
{
    std::int64_t id;
    bool inserted;
};

using message_write_outcome = std::variant<message_write, message_write_rejection>;

// A null result allows a new send. An existing acknowledgement or a domain
// rejection stops it before upload; final publication repeats the check.
std::expected<std::optional<message_write_outcome>, sqlite_error> check_message_write(
    sqlite_database& db, message_to_write const& message, bool attachment);

// The caller owns the write transaction and can reject an oversized RPC
// response before publishing any data.
std::expected<message_write_outcome, sqlite_error> insert_message(
    sqlite_database& db, message_to_write const& message, uploaded_attachment const* attachment = nullptr);

#endif
