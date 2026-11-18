#ifndef CHAT_SRC_CONVERSATION_STORE_HPP
#define CHAT_SRC_CONVERSATION_STORE_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "sqlite_database.hpp"

std::expected<std::optional<std::int64_t>, sqlite_error> open_direct(
    sqlite_database& db, std::int64_t user, std::int64_t peer);

std::expected<std::optional<std::int64_t>, sqlite_error> create_group(
    sqlite_database& db, std::int64_t owner, std::string_view title, std::span<std::int64_t const> members);

enum class group_invite_rejection { unavailable, permission_denied, contact_required };

std::expected<std::variant<bool, group_invite_rejection>, sqlite_error> invite_group_members(
    sqlite_database& db, std::int64_t group, std::int64_t actor, std::span<std::int64_t const> invitees);

enum class group_member_action { promote, demote, transfer, remove, leave };
enum class group_member_rejection { unavailable, permission_denied, invalid_target, member_not_found, admin_limit };

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> change_group_member(
    sqlite_database& db, std::int64_t group, std::int64_t actor, group_member_action action, std::int64_t target);

#endif
