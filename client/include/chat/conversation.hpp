#ifndef CHAT_CLIENT_INCLUDE_CHAT_CONVERSATION_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_CONVERSATION_HPP

#include <cstdint>
#include <string>

#include "message.hpp"

namespace chat
{

struct conversation
{
    std::int64_t user = 0;
    std::string username;
    message last;
    std::uint64_t unread = 0;
};

}    // namespace chat

#endif
