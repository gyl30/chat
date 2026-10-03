#pragma once

#include <string_view>

#include <boost/locale/utf.hpp>

namespace chat
{

inline bool unicode_whitespace(char32_t value)
{
    return (value >= 0x0009 && value <= 0x000d) || value == 0x0020 || value == 0x0085 || value == 0x00a0 ||
        value == 0x1680 || (value >= 0x2000 && value <= 0x200a) || value == 0x2028 || value == 0x2029 ||
        value == 0x202f || value == 0x205f || value == 0x3000;
}

inline bool text_is_blank(std::string_view text)
{
    auto current = text.begin();
    while (current != text.end())
    {
        if (!unicode_whitespace(boost::locale::utf::utf_traits<char>::decode(current, text.end())))
        {
            return false;
        }
    }
    return true;
}

inline bool valid_username(std::string_view username)
{
    if (username.empty() || username.size() > 64)
    {
        return false;
    }
    bool visible = false;
    bool last_whitespace = false;
    auto current = username.begin();
    while (current != username.end())
    {
        auto const value = boost::locale::utf::utf_traits<char>::decode(current, username.end());
        if (value == boost::locale::utf::illegal || value == boost::locale::utf::incomplete || value <= 0x001f ||
            (value >= 0x007f && value <= 0x009f) || value == '@' || value == 0x061c ||
            (value >= 0x200e && value <= 0x200f) || (value >= 0x2028 && value <= 0x202e) ||
            (value >= 0x2066 && value <= 0x2069))
        {
            return false;
        }
        last_whitespace = unicode_whitespace(value);
        if (!visible && last_whitespace)
        {
            return false;
        }
        visible = visible || !last_whitespace;
    }
    return visible && !last_whitespace;
}

inline bool valid_group_title(std::string_view title)
{
    return title.size() <= 256 && title.find('\0') == std::string_view::npos && !text_is_blank(title);
}

} // namespace chat
