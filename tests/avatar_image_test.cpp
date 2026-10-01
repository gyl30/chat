#include <iostream>
#include <string>
#include <zlib.h>

#include <chat/detail/base64.hpp>
#include "avatar_image.hpp"
#include "avatar_fixture.hpp"

int main()
{
    auto png = *chat::detail::decode_base64(avatar_png_base64);
    auto jpeg = *chat::detail::decode_base64(avatar_jpeg_base64);
    if (avatar_media_type(png) != "image/png" || avatar_media_type(jpeg) != "image/jpeg" || !avatar_media_type("not an image").empty() ||
        !avatar_media_type(png.substr(0, png.size() - 1)).empty() || !avatar_media_type(jpeg.substr(0, jpeg.size() - 1)).empty())
    {
        std::cerr << "FAIL avatar image format and completeness\n";
        return 1;
    }
    auto oversized = png;
    for (int offset : {16, 20})
    {
        oversized[offset] = 0;
        oversized[offset + 1] = 0;
        oversized[offset + 2] = static_cast<char>(0x80);
        oversized[offset + 3] = 0;
    }
    auto const crc = crc32(0, reinterpret_cast<unsigned char const*>(oversized.data() + 12), 17);
    for (int i = 0; i < 4; ++i)
    {
        oversized[29 + i] = static_cast<char>(crc >> (24 - i * 8));
    }
    auto trailing_png = png + "garbage" + png.substr(png.size() - 12);
    if (!avatar_media_type(oversized).empty() || !avatar_media_type(trailing_png).empty())
    {
        std::cerr << "FAIL PNG size/payload integrity\n";
        return 1;
    }
    auto trailing = jpeg + "garbage" + std::string("\xff\xd9", 2);
    auto corrupted = jpeg;
    corrupted[4] = 0;
    corrupted[5] = 1;
    if (!avatar_media_type(trailing).empty() || !avatar_media_type(corrupted).empty())
    {
        std::cerr << "FAIL JPEG payload integrity\n";
        return 1;
    }
    png[45] ^= 1;
    jpeg[2] = '\0';
    if (!avatar_media_type(png).empty() || !avatar_media_type(jpeg).empty())
    {
        std::cerr << "FAIL corrupted avatar image accepted\n";
        return 1;
    }
    std::cout << "PASS PNG/JPEG avatar validation\n";
}
