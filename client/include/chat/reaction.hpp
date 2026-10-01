#ifndef CHAT_CLIENT_INCLUDE_CHAT_REACTION_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_REACTION_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chat
{

inline constexpr std::array<std::string_view, 6> reaction_choices{"👍", "❤️", "😂", "😮", "😢", "🎉"};

struct reaction
{
    std::string emoji;
    std::vector<std::int64_t> users;
};

struct reaction_update
{
    std::int64_t conversation = 0;
    std::int64_t message = 0;
    std::int64_t revision = 0;
    std::vector<reaction> reactions;
};

} // namespace chat

#endif
