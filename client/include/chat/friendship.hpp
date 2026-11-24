#ifndef CHAT_CLIENT_INCLUDE_CHAT_FRIENDSHIP_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_FRIENDSHIP_HPP

#include <cstdint>
#include <vector>
#include "user.hpp"

namespace chat
{
struct contact_entry
{
    std::int64_t id;
    std::string username;
    std::int64_t avatar_revision;
    bool has_avatar;
    std::string remark;
    std::string group;
    std::string nickname;
};

struct contact_book
{
    std::vector<contact_entry> users;
    std::vector<std::string> groups;
};

enum class friendship_state
{
    none,
    outgoing_pending,
    incoming_pending,
    accepted
};

struct friendship_result
{
    chat::user user;
    friendship_state state = friendship_state::none;
};

struct friend_request
{
    chat::user user;
    std::int64_t created_at = 0;
};

struct friend_requests_result
{
    std::vector<friend_request> incoming;
    std::vector<friend_request> outgoing;
};
}    // namespace chat
#endif
