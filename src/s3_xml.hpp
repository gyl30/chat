#ifndef CHAT_SRC_S3_XML_HPP
#define CHAT_SRC_S3_XML_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace s3
{
struct multipart_xml
{
    std::string root, upload_id, next_key, next_upload, error_code;
    bool truncated = false;
    std::vector<std::pair<std::string, std::string>> uploads;
    struct part
    {
        int number = 0;
        std::int64_t size = -1;
        std::string etag;
    };
    std::vector<part> parts;
};

std::optional<multipart_xml> read_multipart_xml(std::string_view body);
std::string xml_text(std::string_view text);
}    // namespace s3
#endif
