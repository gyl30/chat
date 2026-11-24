#include "s3_xml.hpp"

#include <array>
#include <charconv>
#include <yxml.h>

namespace s3
{
namespace
{
std::string_view local_name(std::string_view tag)
{
    auto const colon = tag.rfind(':');
    return colon == tag.npos ? tag : tag.substr(colon + 1);
}
std::string_view trim(std::string_view value)
{
    constexpr std::string_view whitespace = " \r\n\t";
    auto const first = value.find_first_not_of(whitespace);
    return first == value.npos ? std::string_view{} : value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}
template <class T>
void read_number(std::string_view text, T& value)
{
    text = trim(text);
    if (text.empty())
    {
        return;
    }
    T parsed{};
    auto const end = text.data() + text.size();
    auto const [ptr, ec] = std::from_chars(text.data(), end, parsed);
    if (ec == std::errc{} && ptr == end)
    {
        value = parsed;
    }
}
}    // namespace

std::optional<multipart_xml> read_multipart_xml(std::string_view body)
{
    // CompleteMultipartUpload may emit whitespace before the XML document.
    body = trim(body);
    std::array<unsigned char, 4096> stack;
    yxml_t xml;
    yxml_init(&xml, stack.data(), stack.size());
    multipart_xml result;
    std::string field, section, section_value, field_value;
    bool field_leaf = false, section_leaf = false;
    std::pair<std::string, std::string> upload;
    multipart_xml::part part;
    int depth = 0;
    for (unsigned char byte : body)
    {
        auto const token = yxml_parse(&xml, byte);
        if (token < 0)
        {
            return std::nullopt;
        }
        if (token == YXML_ELEMSTART)
        {
            ++depth;
            if (depth == 1)
            {
                result.root = local_name(xml.elem);
            }
            else if (depth == 2)
            {
                section = local_name(xml.elem);
                section_value.clear();
                section_leaf = true;
            }
            else if (depth == 3)
            {
                field = local_name(xml.elem);
                field_value.clear();
                field_leaf = true;
                section_leaf = false;
            }
            else if (depth == 4)
            {
                field_leaf = false;
            }
        }
        else if (token == YXML_CONTENT)
        {
            if (depth == 2 && section_leaf)
            {
                section_value += xml.data;
            }
            else if (depth == 3 && field_leaf)
            {
                field_value += xml.data;
            }
        }
        else if (token == YXML_ELEMEND)
        {
            if (depth == 3 && field_leaf && result.root == "ListMultipartUploadsResult" && section == "Upload")
            {
                if (field == "Key")
                {
                    upload.first = std::move(field_value);
                }
                else if (field == "UploadId")
                {
                    upload.second = std::move(field_value);
                }
            }
            else if (depth == 3 && field_leaf && result.root == "ListPartsResult" && section == "Part")
            {
                if (field == "PartNumber")
                {
                    read_number(field_value, part.number);
                }
                else if (field == "Size")
                {
                    read_number(field_value, part.size);
                }
                else if (field == "ETag")
                {
                    auto tag = trim(field_value);
                    if (tag.size() >= 2 && tag.front() == '"' && tag.back() == '"')
                    {
                        tag.remove_prefix(1);
                        tag.remove_suffix(1);
                    }
                    part.etag = tag;
                }
            }
            else if (depth == 2)
            {
                if (result.root == "ListMultipartUploadsResult")
                {
                    if (section == "Upload")
                    {
                        result.uploads.push_back(std::exchange(upload, {}));
                    }
                    else if (section_leaf && section == "IsTruncated")
                    {
                        result.truncated = trim(section_value) == "true";
                    }
                    else if (section_leaf && section == "NextKeyMarker")
                    {
                        result.next_key = std::move(section_value);
                    }
                    else if (section_leaf && section == "NextUploadIdMarker")
                    {
                        result.next_upload = std::move(section_value);
                    }
                }
                else if (result.root == "ListPartsResult" && section == "Part")
                {
                    result.parts.push_back(std::exchange(part, {}));
                }
                else if (result.root == "InitiateMultipartUploadResult" && section_leaf && section == "UploadId")
                {
                    result.upload_id = std::move(section_value);
                }
                else if (result.root == "Error" && section_leaf && section == "Code")
                {
                    result.error_code = trim(section_value);
                }
            }
            --depth;
        }
    }
    return yxml_eof(&xml) == YXML_OK ? std::optional(std::move(result)) : std::nullopt;
}

std::string xml_text(std::string_view text)
{
    std::string result;
    for (auto c : text)
    {
        switch (c)
        {
            case '&':
                result += "&amp;";
                break;
            case '<':
                result += "&lt;";
                break;
            case '>':
                result += "&gt;";
                break;
            default:
                result += c;
                break;
        }
    }
    return result;
}
}    // namespace s3
