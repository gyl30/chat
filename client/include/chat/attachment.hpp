#ifndef CHAT_CLIENT_INCLUDE_CHAT_ATTACHMENT_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_ATTACHMENT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace chat
{

inline constexpr std::int64_t max_attachment_size = INT64_C(4) * 1024 * 1024 * 1024;
inline constexpr std::int64_t attachment_part_size = INT64_C(16) * 1024 * 1024;
inline constexpr std::int64_t upload_retention_seconds = INT64_C(15) * 24 * 3600;
inline constexpr std::int64_t max_image_preview_size = INT64_C(10) * 1024 * 1024;

struct attachment_info
{
    std::string filename;
    std::string media_type;
    std::int64_t size = 0;
};

}    // namespace chat

#endif
