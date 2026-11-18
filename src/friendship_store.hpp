#ifndef CHAT_SRC_FRIENDSHIP_STORE_HPP
#define CHAT_SRC_FRIENDSHIP_STORE_HPP

#include <cstdint>
#include <expected>
#include <variant>
#include <vector>

#include <chat/friendship.hpp>

#include "sqlite_database.hpp"

enum class friendship_action { request, accept, reject, cancel, remove };
enum class friendship_rejection { user_not_found, already_friends, no_incoming_request };

struct friendship_change
{
    chat::friendship_result result;
    chat::friendship_state previous;
    std::int64_t direct_conversation;
};

// These operations run synchronously on the database thread.
std::expected<std::variant<friendship_change, friendship_rejection>, sqlite_error> change_friendship(
    sqlite_database& db, std::int64_t caller, std::int64_t peer, friendship_action action);

std::expected<chat::friend_requests_result, sqlite_error> read_friend_requests(sqlite_database& db, std::int64_t user);
std::expected<std::vector<chat::user>, sqlite_error> read_contacts(sqlite_database& db, std::int64_t user);

#endif
