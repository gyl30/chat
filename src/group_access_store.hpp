#ifndef CHAT_SRC_GROUP_ACCESS_STORE_HPP
#define CHAT_SRC_GROUP_ACCESS_STORE_HPP

#include "conversation_store.hpp"

enum class invite_action
{
    inspect,
    create,
    revoke
};

struct group_invite_change
{
    std::optional<std::string> token;
    bool changed = false;
};

std::expected<std::variant<group_invite_change, group_member_rejection>, sqlite_error> change_group_invite(sqlite_database& db,
                                                                                                           std::int64_t group,
                                                                                                           std::int64_t actor,
                                                                                                           invite_action action);

struct group_join_change
{
    chat::group_join_result result;
    bool request_changed = false;
};

// No result means the code is revoked or unknown.
std::expected<std::optional<group_join_change>, sqlite_error> join_with_invite(sqlite_database& db, std::int64_t actor, std::string_view token);

std::expected<std::variant<bool, group_member_rejection>, sqlite_error> set_group_join_approval(sqlite_database& db,
                                                                                                std::int64_t group,
                                                                                                std::int64_t actor,
                                                                                                bool required);
std::expected<std::variant<chat::group_join_requests_result, group_member_rejection>, sqlite_error> read_group_join_requests(
    sqlite_database& db, std::int64_t group, std::int64_t actor, std::optional<std::int64_t> before);
std::expected<std::variant<bool, group_member_rejection>, sqlite_error> respond_to_group_join_request(
    sqlite_database& db, std::int64_t group, std::int64_t actor, std::int64_t applicant, bool accept);

#endif
