#include "password_hash.hpp"

#include <array>

#include <boost/http/bcrypt.hpp>
#include <openssl/rand.h>

#include <chat/detail/base64.hpp>

std::expected<std::string, std::error_code> hash_password(std::string_view password)
{
    std::array<unsigned char, 16> random;
    if (RAND_bytes(random.data(), random.size()) != 1)
    {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    // bcrypt uses the same 6-bit encoding as Base64, with a different
    // alphabet and without padding.
    constexpr std::string_view standard = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    constexpr std::string_view bcrypt = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    auto const encoded = chat::detail::encode_base64({reinterpret_cast<char const*>(random.data()), random.size()});
    std::string salt = "$2b$12$";
    for (auto const value : encoded)
    {
        if (value == '=')
        {
            break;
        }
        salt.push_back(bcrypt[standard.find(value)]);
    }
    std::error_code error;
    auto const hash = boost::http::bcrypt::hash(password, salt, error);
    if (error)
    {
        return std::unexpected(error);
    }
    return std::string(hash.data(), hash.size());
}

std::expected<bool, std::error_code> check_password(std::string_view password, std::string_view hash)
{
    std::error_code error;
    auto const matches = boost::http::bcrypt::compare(password, hash, error);
    if (error)
    {
        return std::unexpected(error);
    }
    return matches;
}
