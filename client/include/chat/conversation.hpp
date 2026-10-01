#ifndef CHAT_CLIENT_INCLUDE_CHAT_CONVERSATION_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_CONVERSATION_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <vector>

#include "message.hpp"

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
};

struct conversations_result
{
    std::vector<conversation> conversations;
    std::optional<conversation_cursor> next;
};

}    // namespace chat

#endif
