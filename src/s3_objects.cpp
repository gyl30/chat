#include "s3_objects.hpp"
#include "curl_handles.hpp"
#include "s3_signer.hpp"
#include "s3_xml.hpp"
#include <spdlog/spdlog.h>

namespace
{
using chat::detail::add_header;
using chat::detail::curl_headers;
using chat::detail::make_curl;
using chat::detail::use_openssl_ca_file;
s3_error queue_error(blocking_worker::unavailable) { return {0, "S3 worker unavailable"}; }

std::string_view s3_error_reason(std::string_view body)
{
    auto parsed = s3::read_multipart_xml(body);
    if (parsed && parsed->root == "Error")
    {
        // Log only known S3 codes. Error messages and other fields may contain credentials or object names.
        for (std::string_view code : {"NoSuchBucket", "BucketAlreadyOwnedByYou", "BucketAlreadyExists", "AccessDenied", "InvalidAccessKeyId", "SignatureDoesNotMatch",
                                      "AuthorizationHeaderMalformed", "RequestTimeTooSkewed", "InvalidRequest", "InvalidArgument",
                                      "InternalError", "SlowDown", "ServiceUnavailable"})
        {
            if (parsed->error_code == code)
            {
                return code;
            }
        }
    }
    return "S3 request failed";
}
struct response
{
    std::string body, checksum;
    curl_off_t length = -1;
};

struct bucket_client
{
    s3_settings const& settings;

    object_result<response> request(std::string_view method,
                                    std::string const& key,
                                    s3::parameters const& params = {},
                                    std::string_view body = {},
                                    std::initializer_list<std::string> fields = {}) const
    {
        curl_headers headers{nullptr, curl_slist_free_all};
        auto curl = make_curl();
        if (!curl)
        {
            return std::unexpected(s3_error{0, "S3 request setup failed"});
        }
        for (auto const& field : fields)
        {
            if (!add_header(headers, field))
            {
                return std::unexpected(s3_error{0, "S3 request setup failed"});
            }
        }
        response result;
        auto const query = s3::query(params);
        auto const url = s3::origin(settings) + s3::path(settings.bucket, key) + (query.empty() ? "" : "?" + query);
        auto const sig = "aws:amz:" + settings.region + ":s3";
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        // curl 8.23 rejects PATH_AS_IS together with AWS_SIGV4. Internal
        // bucket/object paths have no dot segments; presigned transfers differ.
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
        use_openssl_ca_file(curl.get());
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 4L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl.get(), CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(curl.get(), CURLOPT_AWS_SIGV4, sig.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_USERNAME, settings.access_key.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, settings.secret_key.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &result.body);
        curl_easy_setopt(
            curl.get(),
            CURLOPT_WRITEFUNCTION,
            +[](char* data, std::size_t size, std::size_t count, void* target) -> std::size_t
            {
                auto& buffer = *static_cast<std::string*>(target);
                auto const length = size * count;
                // No Chat operation requests an unbounded listing.
                if (length > 2 * 1024 * 1024 - buffer.size())
                {
                    return CURL_WRITEFUNC_ERROR;
                }
                buffer.append(data, length);
                return length;
            });
        if (method == "HEAD")
        {
            curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);
        }
        else if (method == "POST" || method == "PUT")
        {
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.empty() ? "" : body.data());
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
            if (method == "PUT")
            {
                curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "PUT");
            }
        }
        else if (method == "DELETE")
        {
            curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "DELETE");
        }
        if (auto const rc = curl_easy_perform(curl.get()); rc != CURLE_OK)
        {
            // libcurl's fixed error description never includes the URL or credentials.
            spdlog::warn("S3 {} failed: {}", method, curl_easy_strerror(rc));
            return std::unexpected(s3_error{0, curl_easy_strerror(rc)});
        }
        long status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        if (status < 200 || status >= 300)
        {
            return std::unexpected(s3_error{static_cast<int>(status), s3_error_reason(result.body)});
        }
        if (method == "HEAD")
        {
            curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &result.length);
            curl_header* header = nullptr;
            if (curl_easy_header(curl.get(), "x-amz-meta-sha256", 0, CURLH_HEADER, -1, &header) == CURLHE_OK)
            {
                result.checksum = header->value;
            }
        }
        return result;
    }

    // Recover an initiation whose response or SQLite update was interrupted.
    object_result<std::string> find_multipart(std::string const& key) const
    {
        s3::parameters params{{"uploads", ""}, {"prefix", key}};
        for (;;)
        {
            auto r = request("GET", "", params);
            if (!r)
            {
                return std::unexpected(r.error());
            }
            auto list = s3::read_multipart_xml(r->body);
            if (!list || list->root != "ListMultipartUploadsResult")
            {
                return std::unexpected(s3_error{0, "invalid S3 upload list"});
            }
            for (auto& [name, id] : list->uploads)
            {
                if (name == key && !id.empty())
                {
                    return std::move(id);
                }
            }
            if (!list->truncated)
            {
                return std::string{};
            }
            if (list->next_key.empty() || (params.size() == 4 && params[2].second == list->next_key && params[3].second == list->next_upload))
            {
                return std::unexpected(s3_error{0, "invalid S3 upload list markers"});
            }
            params.resize(2);
            params.emplace_back("key-marker", std::move(list->next_key));
            params.emplace_back("upload-id-marker", std::move(list->next_upload));
        }
    }
};
// Cleanup must not release a reservation while an already-started request can
// still create a multipart or a completed object at its exact key.
struct active_object_write
{
    std::unordered_map<std::string, std::size_t>& active;
    std::string const& key;

    active_object_write(std::unordered_map<std::string, std::size_t>& writes, std::string const& object_key) : active(writes), key(object_key)
    {
        ++active[key];
    }

    ~active_object_write()
    {
        auto entry = active.find(key);
        if (--entry->second == 0)
        {
            active.erase(entry);
        }
    }
};
}    // namespace
s3_objects::s3_objects(s3_settings settings) : settings_(std::move(settings)) {}
object_result<bool> s3_objects::ensure_bucket()
{
    bucket_client const client{settings_};
    auto check = [&client]() -> object_result<void>
    {
        // Multipart uploads already require this permission. A HEAD response has no
        // S3 error document, so a generic HTTP 404 cannot prove the bucket is absent.
        auto response = client.request("GET", "", {{"uploads", ""}, {"max-uploads", "1"}});
        if (!response)
        {
            return std::unexpected(response.error());
        }
        auto list = s3::read_multipart_xml(response->body);
        if (!list || list->root != "ListMultipartUploadsResult")
        {
            return std::unexpected(s3_error{0, "invalid S3 upload list"});
        }
        return {};
    };

    auto const existing = check();
    if (existing)
    {
        return false;
    }
    if (existing.error().status != 404 || existing.error().reason != "NoSuchBucket")
    {
        return std::unexpected(existing.error());
    }

    std::string configuration;
    if (settings_.region != "us-east-1")
    {
        configuration = "<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><LocationConstraint>" +
                        s3::xml_text(settings_.region) + "</LocationConstraint></CreateBucketConfiguration>";
    }
    auto const created = client.request("PUT", "", {}, configuration, {"Content-Type: application/xml"});
    if (!created && (created.error().status != 409 || created.error().reason != "BucketAlreadyOwnedByYou"))
    {
        return std::unexpected(created.error());
    }
    auto const ready = check();
    if (!ready)
    {
        return std::unexpected(ready.error());
    }
    return created.has_value();
}

boost::capy::task<object_result<std::string>> s3_objects::initiate(multipart_upload const& task)
{
    active_object_write const pending{active_writes_, task.key};
    co_return co_await worker_.run(
        [this, &task]() -> object_result<std::string>
        {
            bucket_client b(settings_);
            auto existing = b.find_multipart(task.key);
            if (!existing)
            {
                spdlog::warn("S3 ListMultipartUploads failed (HTTP {}): {}", existing.error().status, existing.error().reason);
                return existing;
            }
            if (!existing->empty())
            {
                return existing;
            }
            auto r = b.request("POST", task.key, {{"uploads", ""}}, {}, {"Content-Type: " + task.media_type, "x-amz-meta-sha256: " + task.checksum});
            if (!r)
            {
                spdlog::warn("S3 CreateMultipartUpload failed (HTTP {}): {}", r.error().status, r.error().reason);
                return std::unexpected(r.error());
            }
            auto parsed = s3::read_multipart_xml(r->body);
            if (!parsed || parsed->root != "InitiateMultipartUploadResult" || parsed->upload_id.empty())
            {
                spdlog::warn("S3 CreateMultipartUpload returned an invalid result");
                return std::unexpected(s3_error{0, "invalid S3 initiation result"});
            }
            return std::move(parsed->upload_id);
        },
        queue_error);
}
boost::capy::task<object_result<std::string>> s3_objects::signed_url(std::string key, std::string multipart, int part)
{
    return worker_.run(
        [this, key = std::move(key), multipart = std::move(multipart), part]() -> object_result<std::string>
        {
            s3::parameters params;
            if (part)
            {
                params = {{"uploadId", multipart}, {"partNumber", std::to_string(part)}};
            }
            auto url = s3::presign(
                settings_, s3::path(settings_.bucket, key), part ? "PUT" : "GET", std::move(params), part ? 900 : 120, std::time(nullptr));
            if (url.empty())
            {
                return std::unexpected(s3_error{0, "could not sign URL"});
            }
            return url;
        },
        queue_error);
}
boost::capy::task<object_result<std::optional<std::pair<std::int64_t, std::string>>>> s3_objects::uploaded_part(multipart_upload const& task,
                                                                                                                int number)
{
    return worker_.run(
        [this, &task, number]() -> object_result<std::optional<std::pair<std::int64_t, std::string>>>
        {
            bucket_client b(settings_);
            auto r =
                b.request("GET", task.key, {{"uploadId", task.multipart}, {"part-number-marker", std::to_string(number - 1)}, {"max-parts", "1"}});
            if (!r)
            {
                return std::unexpected(r.error());
            }
            auto list = s3::read_multipart_xml(r->body);
            if (!list || list->root != "ListPartsResult")
            {
                return std::unexpected(s3_error{0, "invalid S3 part list"});
            }
            if (list->parts.empty() || list->parts.front().number != number)
            {
                return std::nullopt;
            }
            auto& p = list->parts.front();
            if (p.size < 0 || p.etag.empty())
            {
                return std::unexpected(s3_error{0, "invalid S3 part metadata"});
            }
            return std::pair{p.size, std::move(p.etag)};
        },
        queue_error);
}
boost::capy::task<object_result<void>> s3_objects::complete(multipart_upload const& task)
{
    active_object_write const pending{active_writes_, task.key};
    co_return co_await worker_.run(
        [this, &task]() -> object_result<void>
        {
            bucket_client b(settings_);
            // Complete may have succeeded before a disconnect. Only this reserved key can be published.
            auto head = b.request("HEAD", task.key);
            if (!head)
            {
                if (head.error().status != 404)
                {
                    return std::unexpected(head.error());
                }
                std::string body = "<CompleteMultipartUpload>";
                for (auto const& p : task.parts)
                {
                    body += "<Part><PartNumber>" + std::to_string(p.number) + "</PartNumber><ETag>\"" + s3::xml_text(p.etag) + "\"</ETag></Part>";
                }
                body += "</CompleteMultipartUpload>";
                auto done = b.request("POST", task.key, {{"uploadId", task.multipart}}, body, {"Content-Type: application/xml"});
                if (!done)
                {
                    return std::unexpected(done.error());
                }
                auto parsed = s3::read_multipart_xml(done->body);
                if (!parsed || parsed->root != "CompleteMultipartUploadResult")
                {
                    return std::unexpected(s3_error{0, "multipart completion failed"});
                }
                head = b.request("HEAD", task.key);
            }
            if (!head || head->length != task.size || head->checksum != task.checksum)
            {
                return std::unexpected(s3_error{0, "completed object metadata mismatch"});
            }
            return {};
        },
        queue_error);
}
boost::capy::task<object_result<void>> s3_objects::abort(std::string key, std::string multipart)
{
    return worker_.run(
        [this, key = std::move(key), multipart = std::move(multipart)] mutable -> object_result<void>
        {
            bucket_client b(settings_);
            if (multipart.empty())
            {
                auto id = b.find_multipart(key);
                if (!id)
                {
                    return std::unexpected(id.error());
                }
                multipart = std::move(*id);
            }
            if (!multipart.empty())
            {
                auto r = b.request("DELETE", key, {{"uploadId", multipart}});
                if (!r && r.error().status != 404)
                {
                    return std::unexpected(r.error());
                }
            }
            return {};
        },
        queue_error);
}
boost::capy::task<object_result<void>> s3_objects::remove(std::string key)
{
    return worker_.run(
        [this, key = std::move(key)]() -> object_result<void>
        {
            bucket_client b(settings_);
            auto r = b.request("DELETE", key);
            if (!r)
            {
                return std::unexpected(r.error());
            }
            return {};
        },
        queue_error);
}
