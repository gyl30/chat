#ifndef CHAT_CLIENT_INCLUDE_CHAT_USER_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_USER_HPP

#include <cstdint>
#include <string>
#include "avatar.hpp"

namespace chat
{

struct user
{
    std::int64_t id = 0;
    std::string username;
    avatar_state avatar;
};

}    // namespace chat

#endif
