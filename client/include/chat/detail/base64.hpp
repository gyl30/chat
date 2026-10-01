#ifndef CHAT_CLIENT_INCLUDE_CHAT_DETAIL_BASE64_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_DETAIL_BASE64_HPP

#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <openssl/evp.h>

namespace chat::detail
{

inline std::string encode_base64(std::string_view bytes)
{
    std::string encoded(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    auto const size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<unsigned char const*>(bytes.data()), static_cast<int>(bytes.size()));
    encoded.resize(size);
    return encoded;
}

inline std::optional<std::string> decode_base64(std::string_view encoded)
{
    if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || encoded.size() % 4 != 0)
    {
        return {};
    }
    if (encoded.empty())
    {
        return std::string{};
    }
    std::string decoded(encoded.size() / 4 * 3, '\0');
    auto size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
        reinterpret_cast<unsigned char const*>(encoded.data()), static_cast<int>(encoded.size()));
    if (size < 0)
    {
        return {};
    }
    if (encoded.back() == '=')
    {
        --size;
    }
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=')
    {
        --size;
    }
    decoded.resize(size);
    if (encode_base64(decoded) != encoded)
    {
        return {};
    }
    return decoded;
}

}    // namespace chat::detail

#endif
