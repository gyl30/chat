#include "avatar_image.hpp"

#include <cstdlib>
#include <memory>

#include <chat/avatar.hpp>
#include <lodepng.h>

namespace
{
struct png_state : LodePNGState
{
    png_state() { lodepng_state_init(this); }
    ~png_state() { lodepng_state_cleanup(this); }
};

bool valid_chunks(unsigned char const* data, std::size_t size)
{
    // inspect() has already checked the signature and the first IHDR.
    for (std::size_t offset = 33; size - offset >= 12;)
    {
        auto const* chunk = data + offset;
        auto const length = lodepng_chunk_length(chunk);
        if (length > size - offset - 12 || lodepng_chunk_check_crc(chunk))
        {
            return false;
        }
        if (lodepng_chunk_type_equals(chunk, "acTL") || lodepng_chunk_type_equals(chunk, "fcTL") || lodepng_chunk_type_equals(chunk, "fdAT"))
        {
            return false;
        }
        offset += length + 12;
        if (lodepng_chunk_type_equals(chunk, "IEND"))
        {
            return length == 0 && offset == size;
        }
    }
    return false;
}
}    // namespace

bool valid_avatar_png(std::string_view content)
{
    if (content.size() > chat::max_avatar_size)
    {
        return false;
    }

    auto const* bytes = reinterpret_cast<unsigned char const*>(content.data());
    png_state state;
    unsigned width = 0, height = 0;
    constexpr auto extent = chat::avatar_extent;
    if (lodepng_inspect(&width, &height, &state, bytes, content.size()) || width != extent || height != extent ||
        !valid_chunks(bytes, content.size()))
    {
        return false;
    }

    // 16-bit RGBA is the widest allowed PNG format. Adam7 at 256×256
    // has fewer than twice the image height in filter bytes.
    state.decoder.zlibsettings.max_output_size = extent * extent * 8 + extent * 2;
    state.decoder.color_convert = 0;
    unsigned char* pixels = nullptr;
    auto const status = lodepng_decode(&pixels, &width, &height, &state, bytes, content.size());
    std::unique_ptr<unsigned char, decltype(&std::free)> const decoded(pixels, &std::free);
    return status == 0;
}
