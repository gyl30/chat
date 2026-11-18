#ifndef CHAT_SRC_MESSAGE_CHANGES_HPP
#define CHAT_SRC_MESSAGE_CHANGES_HPP

#include <optional>
#include <variant>
#include <chat/message.hpp>
#include "sqlite_database.hpp"

enum class message_change_rejection { unavailable, cannot_send };

struct changed_message
{
    chat::message message;
    bool cleared_pin = false;
};

// Caller owns the write transaction so it can validate the realtime payload
// before committing the body, references, and cleanup records together.
std::expected<std::variant<changed_message, message_change_rejection>, sqlite_error> change_message_body(
    sqlite_database& db, std::int64_t user, std::int64_t conversation, std::int64_t message,
    std::optional<std::string_view> new_text);

struct changed_reaction
{
    chat::reaction_update update;
    bool changed = false;
};

std::expected<std::variant<changed_reaction, message_change_rejection>, sqlite_error> set_message_reaction(
    sqlite_database& db, std::int64_t user, std::int64_t conversation, std::int64_t message, std::string_view emoji);

#endif
