#ifndef CHAT_SRC_MESSAGE_STORE_HPP
#define CHAT_SRC_MESSAGE_STORE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <variant>

#include "sqlite_database.hpp"

struct text_message_write
{
    std::int64_t sender;
    std::int64_t conversation;
    std::int64_t client_id;
    std::string_view text;
    std::optional<std::int64_t> reply_to;
};

enum class message_write_rejection { unavailable, invalid_client_id, invalid_reply };

struct message_write
{
    std::int64_t id;
    bool inserted;
};

// The duplicate check, access check, write and activity update share one transaction.
std::expected<std::variant<message_write, message_write_rejection>, sqlite_error> write_text_message(
    sqlite_database& db, text_message_write const& message);

#endif
