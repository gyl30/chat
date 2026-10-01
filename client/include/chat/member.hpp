#ifndef CHAT_CLIENT_INCLUDE_CHAT_MEMBER_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_MEMBER_HPP

#include <cstdint>
#include <string>
#include "avatar.hpp"

namespace chat
{

enum class member_role { member, admin, owner };

struct conversation_member
{
    std::int64_t id = 0;
    std::string username;
    member_role role = member_role::member;
    avatar_state avatar;
};

}

#endif
