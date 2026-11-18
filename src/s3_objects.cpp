#include "s3_objects.hpp"

#include <array>
#include <utility>

#include <miniocpp/request.h>
#include <openssl/sha.h>

#include <chat/attachment.hpp>
#include <chat/detail/base64.hpp>

namespace
{
constexpr long kConnectTimeoutSeconds = 3;
constexpr long kRequestTimeoutSeconds = 15;

std::expected<minio::http::Response, s3_error> execute_object_request(
    s3_settings const& settings, minio::http::Method method, std::string const& key,
    minio::utils::Multimap headers = {}, std::string_view body = {}, minio::http::DataFunction receive = nullptr)
{
    minio::s3::BaseUrl url(settings.endpoint, settings.https, settings.region);
    if (!url || settings.bucket.empty() || settings.region.empty() ||
        settings.access_key.empty() || settings.secret_key.empty() || key.empty())
    {
        return std::unexpected(s3_error{0, "invalid S3 configuration or object key"});
    }
    minio::creds::StaticProvider credentials(settings.access_key, settings.secret_key);
    minio::s3::Request request(method, settings.region, url, std::move(headers), {});
    request.bucket_name = settings.bucket;
    request.object_name = key;
    request.body = body;
    request.datafunc = std::move(receive);
    auto http = request.ToHttpRequest(&credentials);
    http.connect_timeout_secs = kConnectTimeoutSeconds;
    http.timeout_secs = kRequestTimeoutSeconds;
    auto result = http.Execute();
    if (!result)
    {
        return std::unexpected(s3_error{result.status_code, "object request failed"});
    }
    return result;
}
s3_error queue_error(blocking_worker::unavailable reason)
{
    return {0, reason == blocking_worker::unavailable::full ? "S3 queue is full" : "S3 worker has stopped"};
}
} // namespace

s3_objects::s3_objects(s3_settings settings) : settings_(std::move(settings)) {}

std::expected<std::string, s3_error> s3_objects::put_object(std::string const& key, std::string_view bytes,
                                                    std::string_view media_type) const
{
    if (bytes.size() > chat::max_attachment_size)
    {
        return std::unexpected(s3_error{0, "attachment exceeds the size limit"});
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    auto const* input = reinterpret_cast<unsigned char const*>(bytes.empty() ? "" : bytes.data());
    if (!SHA256(input, bytes.size(), digest.data()))
    {
        return std::unexpected(s3_error{0, "could not compute attachment checksum"});
    }
    auto const checksum = chat::detail::encode_base64(
        {reinterpret_cast<char const*>(digest.data()), digest.size()});
    minio::utils::Multimap headers;
    headers.Add("Content-Type", std::string(media_type));
    headers.Add("x-amz-checksum-sha256", checksum);
    headers.Add("If-None-Match", "*");
    auto uploaded = execute_object_request(settings_, minio::http::Method::kPut, key, std::move(headers), bytes);
    if (!uploaded)
    {
        return std::unexpected(std::move(uploaded.error()));
    }
    auto const checksums = uploaded->headers.Get("x-amz-checksum-sha256");
    if (checksums.size() != 1 || checksums.front() != checksum)
    {
        return std::unexpected(s3_error{uploaded->status_code, "S3 checksum confirmation is missing or incorrect"});
    }
    auto metadata = execute_object_request(settings_, minio::http::Method::kHead, key);
    if (!metadata)
    {
        return std::unexpected(std::move(metadata.error()));
    }
    auto const lengths = metadata->headers.Get("content-length");
    if (lengths.size() != 1 || lengths.front() != std::to_string(bytes.size()))
    {
        return std::unexpected(s3_error{metadata->status_code, "S3 object size differs from the upload"});
    }

    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (auto const byte : digest)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0xf]);
    }
    return result;
}

std::expected<std::string, s3_error> s3_objects::read_object(std::string const& key, std::size_t size,
                                                     std::size_t offset, std::size_t length) const
{
    if (size > chat::max_attachment_size || offset > size || length > size - offset)
    {
        return std::unexpected(s3_error{0, "invalid S3 object range"});
    }
    if (length == 0)
    {
        return std::string{};
    }
    auto const range = std::to_string(offset) + "-" + std::to_string(offset + length - 1);
    minio::utils::Multimap headers;
    headers.Add("Range", "bytes=" + range);
    std::string bytes;
    bytes.reserve(length);
    auto result = execute_object_request(settings_, minio::http::Method::kGet, key, std::move(headers), {},
        [&](minio::http::DataFunctionArgs chunk) {
            if (chunk.datachunk.size() > length - bytes.size())
            {
                return false;
            }
            bytes.append(chunk.datachunk);
            return true;
        });
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    auto const ranges = result->headers.Get("content-range");
    if (result->status_code != 206 || ranges.size() != 1 ||
        ranges.front() != "bytes " + range + "/" + std::to_string(size) || bytes.size() != length)
    {
        return std::unexpected(s3_error{result->status_code, "S3 object range differs from the requested range"});
    }
    return bytes;
}

std::expected<void, s3_error> s3_objects::remove_object(std::string const& key) const
{
    auto result = execute_object_request(settings_, minio::http::Method::kDelete, key);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    return {};
}

boost::capy::task<std::expected<std::string, s3_error>> s3_objects::put(
    std::string key, std::string bytes, std::string media_type)
{
    return worker_.run(
        [this, key = std::move(key), bytes = std::move(bytes), media_type = std::move(media_type)] {
            return put_object(key, bytes, media_type);
        }, queue_error);
}

boost::capy::task<std::expected<std::string, s3_error>> s3_objects::read(
    std::string key, std::size_t size, std::size_t offset, std::size_t length)
{
    return worker_.run([this, key = std::move(key), size, offset, length] {
        return read_object(key, size, offset, length);
    }, queue_error);
}

boost::capy::task<std::expected<void, s3_error>> s3_objects::remove(std::string key)
{
    return worker_.run([this, key = std::move(key)] { return remove_object(key); }, queue_error);
}
