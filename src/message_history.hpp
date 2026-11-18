#ifndef CHAT_SRC_MESSAGE_HISTORY_HPP
#define CHAT_SRC_MESSAGE_HISTORY_HPP

#include <cstdint>
#include <expected>
#include <optional>

#include <chat/message.hpp>

#include "sqlite_database.hpp"

// Null means the conversation is not accessible to the caller.
std::expected<std::optional<chat::messages_result>, sqlite_error> read_message_history(
    sqlite_database& db, std::int64_t user, std::int64_t conversation,
    std::optional<std::int64_t> before, std::optional<std::int64_t> after, std::string_view search = "");

// Read within the caller's snapshot or write transaction.
std::expected<std::optional<chat::message>, sqlite_error> read_message(
    sqlite_database& db, std::int64_t user, std::int64_t conversation, std::int64_t message);

// Returns the committed position, or null if the message is not in the
// conversation or the caller is not a member.
std::expected<std::optional<std::int64_t>, sqlite_error> update_read_position(
    sqlite_database& db, std::int64_t user, std::int64_t conversation, std::int64_t message);

#endif
