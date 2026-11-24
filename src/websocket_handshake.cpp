#include "websocket_handshake.hpp"

#include <array>
#include <memory>
#include <openssl/evp.h>

namespace chat::detail
{
namespace
{
char ascii_lower(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool ascii_iequals(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        if (ascii_lower(lhs[i]) != ascii_lower(rhs[i]))
        {
            return false;
        }
    }
    return true;
}

bool contains_token(std::string_view value, std::string_view token)
{
    while (!value.empty())
    {
        auto const comma = value.find(',');
        auto const current = trim_http_whitespace(value.substr(0, comma));
        if (ascii_iequals(current, token))
        {
            return true;
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return false;
}
}    // namespace

std::string_view trim_http_whitespace(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    {
        value.remove_suffix(1);
    }
    return value;
}

bool has_websocket_upgrade(std::string_view connection, std::string_view upgrade)
{
    return contains_token(connection, "upgrade") && contains_token(upgrade, "websocket");
}

bool make_websocket_accept(std::string_view key, std::string& accept)
{
    static constexpr std::string_view guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest_context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest_context)
    {
        return false;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestInit_ex(digest_context.get(), EVP_sha1(), nullptr) != 1 || EVP_DigestUpdate(digest_context.get(), key.data(), key.size()) != 1 ||
        EVP_DigestUpdate(digest_context.get(), guid.data(), guid.size()) != 1 ||
        EVP_DigestFinal_ex(digest_context.get(), digest.data(), &digest_size) != 1)
    {
        return false;
    }

    std::array<unsigned char, 64> encoded{};
    auto const encoded_size = EVP_EncodeBlock(encoded.data(), digest.data(), static_cast<int>(digest_size));
    if (encoded_size <= 0)
    {
        return false;
    }

    accept.assign(reinterpret_cast<char const*>(encoded.data()), static_cast<std::size_t>(encoded_size));
    return true;
}
}    // namespace chat::detail
