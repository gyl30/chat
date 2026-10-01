#ifndef CHAT_CLIENT_INCLUDE_CHAT_ATTACHMENT_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_ATTACHMENT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace chat
{

inline constexpr std::size_t max_attachment_size = 10 * 1024 * 1024;
inline constexpr std::size_t attachment_chunk_size = 32 * 1024;

struct attachment_info
{
    std::string filename;
    std::string media_type;
    std::int64_t size = 0;
};

}    // namespace chat

#endif
