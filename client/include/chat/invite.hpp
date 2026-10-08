#ifndef CHAT_CLIENT_INCLUDE_CHAT_INVITE_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_INVITE_HPP

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace chat
{

// Group invite codes are short like a verification code. The alphabet leaves out 0, 1, I, L and O,
// which are easily misread when a code is read aloud or retyped.
inline constexpr std::string_view invite_code_alphabet = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
inline constexpr std::size_t invite_code_length = 8;

inline bool valid_invite_token(std::string_view token)
{
    return token.size() == invite_code_length &&
        std::ranges::all_of(token, [](char c) { return invite_code_alphabet.find(c) != std::string_view::npos; });
}

// Accepts what people type: any case, with spaces or a dash between the two groups.
inline std::optional<std::string> normalize_invite_token(std::string_view input)
{
    std::string token;
    for (auto c : input)
    {
        if (c == ' ' || c == '-' || c == '\t' || c == '\n' || c == '\r') { continue; }
        token += c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }
    if (!valid_invite_token(token)) { return std::nullopt; }
    return token;
}

// Shown in two groups of four, such as K7QM-3XWP.
inline std::string format_invite_token(std::string_view token)
{
    if (token.size() != invite_code_length) { return std::string(token); }
    std::string value(token.substr(0, invite_code_length / 2));
    value += '-';
    value += token.substr(invite_code_length / 2);
    return value;
}

}    // namespace chat

#endif
