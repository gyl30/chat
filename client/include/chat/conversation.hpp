#ifndef CHAT_CLIENT_INCLUDE_CHAT_CONVERSATION_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_CONVERSATION_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <vector>

#include "message.hpp"
#include "user.hpp"

namespace chat
{

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
    conversation_kind kind = conversation_kind::direct;
    std::int64_t user = 0;
    std::string username;
    std::uint64_t member_count = 0;
    message last;
    std::uint64_t unread = 0;
    avatar_state avatar;
    bool muted = false;
    bool pinned = false;
    std::optional<quoted_message> pinned_message;
    std::string announcement;
    bool join_approval = false;
};

struct conversations_result
{
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

enum class group_join_request_state { pending, accepted, rejected };

struct group_join_request_event
{
    std::int64_t conversation = 0;
    std::int64_t user = 0;
    group_join_request_state state = group_join_request_state::pending;
};

}    // namespace chat

#endif
