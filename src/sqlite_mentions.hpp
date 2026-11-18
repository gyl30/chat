#ifndef CHAT_SRC_SQLITE_MENTIONS_HPP
#define CHAT_SRC_SQLITE_MENTIONS_HPP

#include <cstdint>
#include <expected>
#include <string_view>

#include "sqlite_database.hpp"

// The caller owns the transaction that changes the message body.
sqlite_error replace_message_mentions(sqlite_database& db, std::int64_t conversation, std::int64_t message,
                                      std::string_view text);

#endif
