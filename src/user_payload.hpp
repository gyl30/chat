#ifndef CHAT_SRC_USER_PAYLOAD_HPP
#define CHAT_SRC_USER_PAYLOAD_HPP

#include <chat/user.hpp>

struct user_payload
{
    std::int64_t id;
    std::string username;
    std::int64_t avatar_revision;
    bool has_avatar;

    explicit user_payload(chat::user const& user)
        : id(user.id), username(user.username), avatar_revision(user.avatar.revision), has_avatar(user.avatar.present)
    {
    }
};

#endif
