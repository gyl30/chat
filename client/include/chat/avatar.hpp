#ifndef CHAT_CLIENT_INCLUDE_CHAT_AVATAR_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_AVATAR_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace chat
{

inline constexpr std::size_t max_avatar_size = 1024 * 1024;
inline constexpr std::size_t max_avatar_pixels = 16 * 1024 * 1024;

struct avatar_state
{
    std::int64_t revision = 0;
    bool present = false;
    bool operator==(avatar_state const&) const = default;
};

struct avatar
{
    std::int64_t user = 0;
    avatar_state state;
    std::string media_type;
    std::string data;
};

}    // namespace chat

#endif
