#ifndef CHAT_SRC_MESSAGE_STORE_HPP
#define CHAT_SRC_MESSAGE_STORE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "sqlite_database.hpp"
#include <chat/message.hpp>

struct message_to_write
{
    std::int64_t sender;
    std::int64_t conversation;
    std::int64_t client_id;
    std::string_view text;
    std::optional<std::int64_t> reply_to;
    std::optional<std::int64_t> forward_source{};
    bool markdown = false;
    bool mention_all = false;
    std::span<std::int64_t const> bundle{};
    std::span<std::string const> poll_options{};
    chat::poll_settings settings{};
    std::string_view content() const { return !bundle.empty() ? "bundle" : !poll_options.empty() ? "poll" : "message"; }
};

enum class message_write_rejection
{
    unavailable,
    muted,
    invalid_client_id,
    invalid_reply
};

struct message_write
{
    std::int64_t id;
    bool inserted;
};

using message_write_outcome = std::variant<message_write, message_write_rejection>;

// A null result allows a new send. An existing acknowledgement or a domain
// rejection stops it before upload; final publication repeats the check.
std::expected<std::optional<message_write_outcome>, sqlite_error> check_message_write(sqlite_database& db,
                                                                                      message_to_write const& message,
                                                                                      bool attachment);

// The caller owns the write transaction and can reject an oversized RPC
// response before publishing any data.
std::expected<message_write_outcome, sqlite_error> insert_message(sqlite_database& db,
                                                                  message_to_write const& message,
                                                                  std::string_view object_key = {});

#endif
