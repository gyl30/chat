#ifndef CHAT_CLIENT_INCLUDE_CHAT_TYPING_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_TYPING_HPP

#include <cstdint>
#include <string>

namespace chat
{

struct typing_event
{
    std::int64_t conversation = 0;
    std::int64_t user = 0;
    std::string username;
    bool typing = false;
};

}    // namespace chat

#endif
