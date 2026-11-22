#include "s3_objects.hpp"
#include <miniocpp/baseclient.h>
#include <pugixml.hpp>

namespace
{
using minio::http::Method;
using minio::utils::Multimap;
s3_error queue_error(blocking_worker::unavailable) { return {0, "S3 worker unavailable"}; }
struct bucket_client
{
    minio::s3::BaseUrl url;
    minio::creds::StaticProvider credentials;
    minio::s3::BaseClient client;
    s3_settings const& settings;
    explicit bucket_client(s3_settings const& s)
        : url(s.endpoint, s.https, s.region), credentials(s.access_key, s.secret_key), client(url, &credentials), settings(s)
    {
    }
    template <class A>
    A args(std::string const& key) const
    {
        A a;
        a.bucket = settings.bucket;
        a.object = key;
        a.region = settings.region;
        return a;
    }
    object_result<minio::http::Response> request(Method method, std::string const& key, Multimap query = {})
    {
        minio::s3::Request req(method, settings.region, url, {}, std::move(query));
        req.bucket_name = settings.bucket;
        req.object_name = key;
        auto http = req.ToHttpRequest(&credentials);
        http.connect_timeout_secs = 4;
        http.timeout_secs = 30;
        auto r = http.Execute();
        if (!r)
        {
            return std::unexpected(s3_error{r.status_code, "S3 request failed"});
        }
        return r;
    }
    // Recover an initiation whose response or SQLite update was interrupted.
    object_result<std::string> find_multipart(std::string const& key)
    {
        Multimap query;
        query.Add("uploads", "");
        query.Add("prefix", key);
        auto r = request(Method::kGet, "", std::move(query));
        if (!r)
        {
            return std::unexpected(r.error());
        }
        pugi::xml_document doc;
        if (!doc.load_string(r->body.c_str()) || !doc.child("ListMultipartUploadsResult"))
        {
            return std::unexpected(s3_error{0, "invalid S3 upload list"});
        }
        for (auto item : doc.document_element().children("Upload"))
        {
            if (key == item.child_value("Key"))
            {
                return item.child_value("UploadId");
            }
        }
        return std::string{};
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
boost::capy::task<object_result<std::string>> s3_objects::initiate(multipart_upload const& task)
{
    active_object_write const pending{active_writes_, task.key};
    co_return co_await worker_.run(
        [this, &task]() -> object_result<std::string>
        {
            bucket_client b(settings_);
            auto existing = b.find_multipart(task.key);
            if (!existing || !existing->empty())
            {
                return existing;
            }
            auto a = b.args<minio::s3::CreateMultipartUploadArgs>(task.key);
            a.headers.Add("Content-Type", task.media_type);
            a.headers.Add("x-amz-meta-sha256", task.checksum);
            auto r = b.client.CreateMultipartUpload(std::move(a));
            if (!r || r->upload_id.empty())
            {
                return std::unexpected(s3_error{0, "multipart creation failed"});
            }
            return r->upload_id;
        },
        queue_error);
}
boost::capy::task<object_result<std::string>> s3_objects::signed_url(std::string key, std::string multipart, int part)
{
    return worker_.run(
        [this, key = std::move(key), multipart = std::move(multipart), part]() -> object_result<std::string>
        {
            bucket_client b(settings_);
            auto a = b.args<minio::s3::GetPresignedObjectUrlArgs>(key);
            a.method = part ? Method::kPut : Method::kGet;
            a.expiry_seconds = part ? 900 : 120;
            if (part)
            {
                a.extra_query_params.Add("uploadId", multipart);
                a.extra_query_params.Add("partNumber", std::to_string(part));
            }
            auto r = b.client.GetPresignedObjectUrl(std::move(a));
            if (!r)
            {
                return std::unexpected(s3_error{0, "could not sign URL"});
            }
            return r->url;
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
            Multimap q;
            q.Add("uploadId", task.multipart);
            q.Add("part-number-marker", std::to_string(number - 1));
            q.Add("max-parts", "1");
            auto r = b.request(Method::kGet, task.key, std::move(q));
            if (!r)
            {
                return std::unexpected(r.error());
            }
            pugi::xml_document doc;
            if (!doc.load_string(r->body.c_str()) || !doc.child("ListPartsResult"))
            {
                return std::unexpected(s3_error{0, "invalid part list"});
            }
            auto p = doc.document_element().child("Part");
            if (!p || p.child("PartNumber").text().as_int() != number)
            {
                return std::nullopt;
            }
            std::string tag = p.child_value("ETag");
            if (tag.size() > 1 && tag.front() == '"' && tag.back() == '"')
            {
                tag = tag.substr(1, tag.size() - 2);
            }
            return std::pair{static_cast<std::int64_t>(p.child("Size").text().as_llong(-1)), std::move(tag)};
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
            auto head = b.request(Method::kHead, task.key);
            if (!head)
            {
                if (head.error().status != 404)
                {
                    return std::unexpected(head.error());
                }
                auto a = b.args<minio::s3::CompleteMultipartUploadArgs>(task.key);
                a.upload_id = task.multipart;
                for (auto const& p : task.parts)
                {
                    a.parts.emplace_back(p.number, p.etag);
                }
                auto done = b.client.CompleteMultipartUpload(std::move(a));
                if (!done)
                {
                    return std::unexpected(s3_error{0, "multipart completion failed"});
                }
                head = b.request(Method::kHead, task.key);
            }
            if (!head || head->headers.GetFront("content-length") != std::to_string(task.size) ||
                head->headers.GetFront("x-amz-meta-sha256") != task.checksum)
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
                Multimap q;
                q.Add("uploadId", multipart);
                auto r = b.request(Method::kDelete, key, std::move(q));
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
            auto r = b.request(Method::kDelete, key);
            if (!r)
            {
                return std::unexpected(r.error());
            }
            return {};
        },
        queue_error);
}
