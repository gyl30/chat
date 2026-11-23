#ifndef CHAT_CLIENT_INCLUDE_CHAT_LOGIN_SESSION_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_LOGIN_SESSION_HPP

#include <cstdint>
#include <string>

namespace chat
{
inline constexpr int login_session_invalid = -32016;
inline constexpr int current_password_incorrect = -32017;

struct login_session
{
    std::int64_t id = 0;
    std::string device_name;
    std::string client_type;
    std::int64_t created_at = 0;
    std::int64_t last_seen = 0;
    bool current = false;
};
}    // namespace chat

#endif
