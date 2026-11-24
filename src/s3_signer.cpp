#include "s3_signer.hpp"
#include "s3_objects.hpp"

#include <algorithm>
#include <array>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <span>

namespace s3
{
namespace
{
std::string encode(std::string_view text, bool keep_slash = false)
{
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : text)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
            (c == '/' && keep_slash))
        {
            result += static_cast<char>(c);
        }
        else
        {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

using sha256 = std::array<unsigned char, 32>;

std::string lower_hex(sha256 const& bytes)
{
    constexpr std::string_view hex = "0123456789abcdef";
    std::string out;
    for (auto byte : bytes)
    {
        out += hex[byte >> 4];
        out += hex[byte & 15];
    }
    return out;
}

bool hmac(std::span<unsigned char const> secret, std::string_view value, sha256& result)
{
    return HMAC(EVP_sha256(),
                secret.data(),
                static_cast<int>(secret.size()),
                reinterpret_cast<unsigned char const*>(value.data()),
                value.size(),
                result.data(),
                nullptr) != nullptr;
}
}    // namespace

std::string path(std::string_view bucket, std::string_view key)
{
    auto result = "/" + encode(bucket);
    if (!key.empty())
    {
        result += "/" + encode(key, true);
    }
    return result;
}

std::string query(parameters const& params)
{
    parameters encoded;
    for (auto const& [name, value] : params)
    {
        encoded.emplace_back(encode(name), encode(value));
    }
    std::ranges::sort(encoded);
    std::string result;
    for (auto const& [name, value] : encoded)
    {
        if (!result.empty())
        {
            result += '&';
        }
        result += name + '=' + value;
    }
    return result;
}

std::string authority(s3_settings const& settings)
{
    auto result = settings.endpoint;
    // libcurl's HTTP Host omits the default port.
    std::string_view const default_port = settings.https ? ":443" : ":80";
    if (result.ends_with(default_port))
    {
        result.resize(result.size() - default_port.size());
    }
    return result;
}

std::string origin(s3_settings const& settings) { return (settings.https ? "https://" : "http://") + authority(settings); }

std::string presign(s3_settings const& settings, std::string const& uri, std::string_view method, parameters params, int seconds, std::time_t now)
{
    std::tm utc{};
    std::array<char, 17> timestamp{};
    if (!gmtime_r(&now, &utc) || !std::strftime(timestamp.data(), timestamp.size(), "%Y%m%dT%H%M%SZ", &utc))
    {
        return {};
    }
    auto const date = std::string(timestamp.data(), 8);
    auto const scope = date + '/' + settings.region + "/s3/aws4_request";
    params.emplace_back("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
    params.emplace_back("X-Amz-Credential", settings.access_key + '/' + scope);
    params.emplace_back("X-Amz-Date", timestamp.data());
    params.emplace_back("X-Amz-Expires", std::to_string(seconds));
    params.emplace_back("X-Amz-SignedHeaders", "host");

    auto const fields = query(params);
    auto const request = std::string(method) + '\n' + uri + '\n' + fields + "\nhost:" + authority(settings) + "\n\nhost\nUNSIGNED-PAYLOAD";
    sha256 request_hash, date_key, region_key, service_key, signing_key, signature;
    if (EVP_Digest(request.data(), request.size(), request_hash.data(), nullptr, EVP_sha256(), nullptr) != 1)
    {
        return {};
    }
    auto const to_sign = std::string("AWS4-HMAC-SHA256\n") + timestamp.data() + '\n' + scope + '\n' + lower_hex(request_hash);
    auto const root = "AWS4" + settings.secret_key;
    if (!hmac({reinterpret_cast<unsigned char const*>(root.data()), root.size()}, date, date_key) || !hmac(date_key, settings.region, region_key) ||
        !hmac(region_key, "s3", service_key) || !hmac(service_key, "aws4_request", signing_key) || !hmac(signing_key, to_sign, signature))
    {
        return {};
    }
    return origin(settings) + uri + '?' + fields + "&X-Amz-Signature=" + lower_hex(signature);
}
}    // namespace s3
