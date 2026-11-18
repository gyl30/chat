#include "s3_objects.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#include <miniocpp/client.h>

int main()
{
    auto const* endpoint = std::getenv("CHAT_TEST_S3_ENDPOINT");
    auto const* access_key = std::getenv("CHAT_TEST_S3_ACCESS_KEY");
    auto const* secret_key = std::getenv("CHAT_TEST_S3_SECRET_KEY");
    if (!endpoint || !access_key || !secret_key)
    {
        std::cout << "BLOCKED: isolated Silo endpoint and credentials are required\n";
        return 77;
    }

    bool passed = true;
    auto check = [&](bool condition, std::string_view description) {
        std::cout << (condition ? "PASS " : "FAIL ") << description << '\n';
        passed = passed && condition;
    };
    s3_settings settings{endpoint, "chat-s3-test-" + std::to_string(getpid()), access_key, secret_key, "us-east-1", false};
    minio::creds::StaticProvider credentials(settings.access_key, settings.secret_key);
    minio::s3::BaseUrl url(settings.endpoint, false, settings.region);
    minio::s3::Client client(url, &credentials);
    minio::s3::MakeBucketArgs bucket;
    bucket.bucket = settings.bucket;
    bucket.region = settings.region;
    auto created = client.MakeBucket(bucket);
    if (!created)
    {
        std::cerr << "FAIL create private test bucket: " << created.error().String() << '\n';
        return 1;
    }

    s3_objects objects(settings);
    std::string const bytes("123\0abc", 7);
    std::string const key = "attachments/test";
    auto stored = objects.put(key, bytes, "application/octet-stream");
    if (!stored)
    {
        std::cerr << "PUT status " << stored.error().status << ": " << stored.error().detail << '\n';
    }
    check(stored && stored->size() == 64, "Silo validates attachment SHA-256 and object size");
    auto duplicate = objects.put(key, "replacement", "application/octet-stream");
    check(!duplicate && duplicate.error().status == 412, "a second PUT cannot overwrite the object");
    auto chunk = objects.read(key, bytes.size(), 2, 4);
    if (!chunk)
    {
        std::cerr << "GET status " << chunk.error().status << ": " << chunk.error().detail << '\n';
    }
    check(chunk && *chunk == bytes.substr(2, 4), "Silo returns an exact binary byte range");
    auto wrong_size = objects.read(key, bytes.size() + 1, 2, 4);
    check(!wrong_size, "download rejects mismatched metadata");
    check(!objects.read(key, bytes.size(), bytes.size() + 1, 0), "invalid range rejected locally");
    check(objects.remove(key).has_value(), "delete the private object");
    auto missing = objects.read(key, bytes.size(), 0, bytes.size());
    check(!missing && missing.error().status == 404, "deleted object is unavailable");
    check(objects.remove(key).has_value(), "cleanup retry is idempotent");

    auto empty = objects.put(key, {}, "application/octet-stream");
    check(empty.has_value(), "Silo accepts empty attachments");
    check(objects.remove(key).has_value(), "delete the empty object");
    auto bad_credentials = settings;
    bad_credentials.secret_key += "invalid";
    s3_objects denied(std::move(bad_credentials));
    auto unauthenticated = denied.read(key, bytes.size(), 0, bytes.size());
    check(!unauthenticated && unauthenticated.error().status == 403, "Silo rejects invalid credentials");

    minio::s3::RemoveBucketArgs cleanup;
    cleanup.bucket = settings.bucket;
    cleanup.region = settings.region;
    check(static_cast<bool>(client.RemoveBucket(cleanup)), "remove the test bucket");
    return passed ? 0 : 1;
}
