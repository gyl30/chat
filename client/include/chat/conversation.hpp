#ifndef CHAT_CLIENT_INCLUDE_CHAT_CONVERSATION_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_CONVERSATION_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <vector>

#include "message.hpp"
#include "member.hpp"
#include "user.hpp"

namespace chat
{

enum class notification_level
{
    all,
    mentions,
    muted
};

struct conversation_view_event
{
    std::int64_t conversation;
    std::string action;
    std::optional<std::int64_t> through{};
    std::optional<std::int64_t> level{};
    std::optional<std::int64_t> until{};
};

enum class conversation_kind
{
    direct,
    group
};

struct conversation_cursor
{
    std::int64_t activity = 0;
    std::int64_t id = 0;
    bool pinned = false;
};

struct conversation
{
    std::int64_t id = 0;
    std::int64_t folder = 0;
    conversation_kind kind = conversation_kind::direct;
    std::int64_t user = 0;
    std::string username;
    std::string remark;
    std::string nickname;
    std::uint64_t member_count = 0;
    message last;
    std::uint64_t unread = 0;
    avatar_state avatar;
    notification_level notifications = notification_level::all;
    std::int64_t mute_until = 0;
    bool hidden = false;
    std::int64_t join_requests = 0;
    bool marked_unread = false;
    bool unread_mention = false;
    bool pinned = false;
    std::optional<quoted_message> pinned_message;
    std::string announcement;
    std::string description;
    bool join_approval = false;
    member_role speech_role = member_role::member;
    bool group_muted = false;
    std::int64_t speech_until = 0;
    std::int64_t server_time = 0;
};

struct conversation_folder
{
    std::int64_t id;
    std::string name;
};

struct conversations_result
{
    std::vector<conversation_folder> folders;
    std::vector<conversation> conversations;
    std::optional<conversation_cursor> next;
};

enum class group_join_state
{
    joined,
    member,
    pending
};

struct group_join_result
{
    std::int64_t conversation = 0;
    std::string title;
    std::uint64_t member_count = 0;
    group_join_state state = group_join_state::member;
};

struct group_join_request
{
    user applicant;
    std::int64_t created_at = 0;
};

struct group_join_requests_result
{
    std::vector<group_join_request> requests;
    std::optional<std::int64_t> next;
};

enum class group_join_request_state
{
    pending,
    accepted,
    rejected
};

struct group_join_request_event
{
    std::int64_t conversation = 0;
    std::int64_t user = 0;
    group_join_request_state state = group_join_request_state::pending;
};

}    // namespace chat

#endif
