#ifndef CHAT_SRC_MESSAGE_HISTORY_HPP
#define CHAT_SRC_MESSAGE_HISTORY_HPP

#include <cstdint>
#include <expected>
#include <optional>

#include <chat/message.hpp>

#include "sqlite_database.hpp"

// Null means the conversation is not accessible to the caller.
std::expected<std::optional<chat::messages_result>, sqlite_error> read_message_history(sqlite_database& db,
                                                                                       std::int64_t user,
                                                                                       std::int64_t conversation,
                                                                                       std::optional<std::int64_t> before,
                                                                                       std::optional<std::int64_t> after,
                                                                                       std::string_view search = "",
                                                                                       std::optional<std::int64_t> around = {},
                                                                                       std::optional<chat::message_category> category = {});

std::expected<chat::message_search_page, sqlite_error> search_all_messages(sqlite_database& db,
                                                                           std::int64_t user,
                                                                           chat::message_search_filter const& filter,
                                                                           std::optional<std::int64_t> before);

std::expected<std::vector<chat::message_search_scope>, sqlite_error> read_message_search_scopes(sqlite_database& db, std::int64_t user);

// Read within the caller's snapshot or write transaction.
std::expected<std::optional<chat::message>, sqlite_error> read_message(sqlite_database& db,
                                                                       std::int64_t user,
                                                                       std::int64_t conversation,
                                                                       std::int64_t message);

struct read_position_change
{
    std::int64_t message;
    bool advanced;
};

// Returns the committed position and whether it advanced, or null if the
// message is not in the conversation or the caller is not a member.
std::expected<std::optional<read_position_change>, sqlite_error> update_read_position(sqlite_database& db,
                                                                                      std::int64_t user,
                                                                                      std::int64_t conversation,
                                                                                      std::int64_t message);

#endif
