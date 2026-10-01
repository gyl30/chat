#ifndef CHAT_CLIENT_INCLUDE_CHAT_PRESENCE_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_PRESENCE_HPP

#include <cstdint>

namespace chat
{

struct presence
{
    std::int64_t user = 0;
    bool online = false;
    std::int64_t last_seen = 0;
};

}    // namespace chat

#endif
