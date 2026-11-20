#ifndef CHAT_SRC_CONVERSATION_STORE_HPP
#define CHAT_SRC_CONVERSATION_STORE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <chat/conversation.hpp>
#include <chat/member.hpp>
#include "sqlite_database.hpp"

std::expected<std::optional<std::int64_t>, sqlite_error> open_direct(sqlite_database& db, std::int64_t user, std::int64_t peer);

std::expected<std::optional<std::int64_t>, sqlite_error> create_group(sqlite_database& db,
                                                                      std::int64_t owner,
                                                                      std::string_view title,
                                                                      std::span<std::int64_t const> members);

std::expected<std::string, sqlite_error> read_conversation_list(sqlite_database& db,
                                                                std::int64_t user,
                                                                std::optional<chat::conversation_cursor> before);

// No rows means the caller no longer has access to the conversation.
std::expected<std::vector<chat::conversation_member>, sqlite_error> read_conversation_members(sqlite_database& db,
                                                                                              std::int64_t conversation,
                                                                                              std::int64_t actor);

std::expected<bool, sqlite_error> can_send_to_conversation(sqlite_database& db, std::int64_t conversation, std::int64_t actor);

enum class group_member_action
{
    promote,
    demote,
    transfer,
    remove,
    leave
};
enum class group_member_rejection
{
    unavailable,
    permission_denied,
    invalid_target,
    member_not_found,
    admin_limit,
    contact_required
};

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> invite_group_members(sqlite_database& db,
                                                                                             std::int64_t group,
                                                                                             std::int64_t actor,
                                                                                             std::span<std::int64_t const> invitees);

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> change_group_member(
    sqlite_database& db, std::int64_t group, std::int64_t actor, group_member_action action, std::int64_t target);

std::expected<std::optional<chat::member_role>, sqlite_error> read_group_role(sqlite_database& db, std::int64_t group, std::int64_t actor);

enum class group_text_field
{
    title,
    announcement
};
std::expected<std::variant<bool, group_member_rejection>, sqlite_error> set_group_text(
    sqlite_database& db, std::int64_t group, std::int64_t actor, group_text_field field, std::string_view text);

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> set_group_pin(sqlite_database& db,
                                                                                      std::int64_t group,
                                                                                      std::int64_t actor,
                                                                                      std::optional<std::int64_t> message);

#endif
