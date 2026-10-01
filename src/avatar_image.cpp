#include "avatar_image.hpp"

#include <csetjmp>
#include <cstdint>
#include <memory>
#include <vector>
#include <jpeglib.h>
#include <png.h>
#include <chat/avatar.hpp>

std::string avatar_media_type(std::string_view content)
{
    std::string media_type;
    if (content.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8)) && content.ends_with(std::string_view("\0\0\0\0IEND\xae\x42\x60\x82", 12)))
    {
        std::size_t offset = 8;
        for (;;)
        {
            if (content.size() - offset < 12)
            {
                return {};
            }
            auto const* bytes = reinterpret_cast<unsigned char const*>(content.data() + offset);
            auto const length = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
            if (length > content.size() - offset - 12)
            {
                return {};
            }
            auto const ending = content.substr(offset + 4, 4) == "IEND";
            offset += length + 12;
            if (ending)
            {
                if (offset != content.size() || length != 0)
                {
                    return {};
                }
                break;
            }
        }
        png_image image{};
        image.version = PNG_IMAGE_VERSION;
        if (png_image_begin_read_from_memory(&image, content.data(), content.size()) && image.width > 0 && image.height > 0 &&
            static_cast<std::uint64_t>(image.width) * image.height <= chat::max_avatar_pixels)
        {
            image.format = PNG_FORMAT_RGBA;
            std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
            if (png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) && image.warning_or_error == 0)
            {
                media_type = "image/png";
            }
        }
        png_image_free(&image);
    }
    else if (content.starts_with(std::string_view("\xff\xd8\xff", 3)) && content.ends_with(std::string_view("\xff\xd9", 2)))
    {
        struct jpeg_failure
        {
            jpeg_error_mgr errors;
            std::jmp_buf recovery;
        };
        jpeg_failure failure{};
        auto decoder = std::make_unique<jpeg_decompress_struct>();
        decoder->err = jpeg_std_error(&failure.errors);
        failure.errors.error_exit = [](j_common_ptr info) { std::longjmp(reinterpret_cast<jpeg_failure*>(info->err)->recovery, 1); };
        failure.errors.output_message = [](j_common_ptr) {};
        if (setjmp(failure.recovery) == 0)
        {
            jpeg_create_decompress(decoder.get());
            jpeg_mem_src(decoder.get(), reinterpret_cast<unsigned char const*>(content.data()), content.size());
            if (jpeg_read_header(decoder.get(), TRUE) == JPEG_HEADER_OK &&
                static_cast<std::uint64_t>(decoder->image_width) * decoder->image_height <= chat::max_avatar_pixels &&
                jpeg_start_decompress(decoder.get()))
            {
                auto row = (*decoder->mem->alloc_sarray)(
                    reinterpret_cast<j_common_ptr>(decoder.get()), JPOOL_IMAGE, decoder->output_width * decoder->output_components, 1);
                while (decoder->output_scanline < decoder->output_height)
                {
                    jpeg_read_scanlines(decoder.get(), row, 1);
                }
                if (jpeg_finish_decompress(decoder.get()) && failure.errors.num_warnings == 0 && decoder->src->bytes_in_buffer == 0)
                {
                    media_type = "image/jpeg";
                }
            }
        }
        jpeg_destroy_decompress(decoder.get());
    }
    return media_type;
}
