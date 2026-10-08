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
inline constexpr std::size_t legacy_invite_token_length = 64;

// A stored token: an invite code, or a legacy 64-digit lowercase hex token that stays valid until revoked.
inline bool valid_invite_token(std::string_view token)
{
    if (token.size() == legacy_invite_token_length)
    {
        return std::ranges::all_of(token, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }
    return token.size() == invite_code_length &&
        std::ranges::all_of(token, [](char c) { return invite_code_alphabet.find(c) != std::string_view::npos; });
}

// Accepts what people paste or type: any case, spaces or dashes between groups, or an old chat://join/ link.
inline std::optional<std::string> normalize_invite_token(std::string_view input)
{
    constexpr std::string_view legacy_prefix = "chat://join/";
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) { input.remove_prefix(1); }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t' || input.back() == '\n' || input.back() == '\r'))
    {
        input.remove_suffix(1);
    }
    if (input.starts_with(legacy_prefix)) { input.remove_prefix(legacy_prefix.size()); }
    std::string token;
    for (auto c : input)
    {
        if (c == ' ' || c == '-' || c == '\t') { continue; }
        token += c;
    }
    auto const legacy = token.size() == legacy_invite_token_length;
    for (auto& c : token)
    {
        if (legacy && c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
        if (!legacy && c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
    }
    if (!valid_invite_token(token)) { return std::nullopt; }
    return token;
}

// Codes are shown in two groups of four; legacy tokens are shown unchanged.
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
