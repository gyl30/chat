#ifndef CHAT_CLIENT_INCLUDE_CHAT_MESSAGE_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_MESSAGE_HPP

#include <cstdint>
#include <string>

namespace chat
{

struct message
{
    std::int64_t id = 0;
    std::int64_t from = 0;
    std::string text;
};

}    // namespace chat

#endif
