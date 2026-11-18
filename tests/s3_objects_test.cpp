#include "s3_objects.hpp"
#include "attachment_cleanup.hpp"
#include "message_store.hpp"
#include "schema.hpp"

#include <cstdlib>
#include <sqlite3.h>
#include <vector>
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

    char file[] = "/tmp/chat-silo-test.XXXXXX";
    auto const fd = mkstemp(file);
    check(fd >= 0, "create isolated SQLite file");
    if (fd >= 0)
    {
        close(fd);
        {
            auto db = sqlite_database::open(file);
            check(db && initialize_schema(*db).ok(), "initialize attachment schema");
            if (db)
            {
                check(db->exec(R"SQL(
                    INSERT INTO users(username,password_hash,last_seen_at) VALUES('Alice',printf('%060d',1),0);
                    INSERT INTO conversations(kind,activity,direct_user_low,direct_user_high) VALUES('direct',0,1,1);
                    INSERT INTO conversation_members(conversation_id,user_id) VALUES(1,1);
                    CREATE TABLE failure(message INTEGER REFERENCES messages(id) DEFERRABLE INITIALLY DEFERRED);
                    CREATE TRIGGER fail_attachment_commit AFTER INSERT ON message_attachments
                    BEGIN INSERT INTO failure(message) VALUES(-1); END;
                )SQL").ok(), "prepare a deferred failure at SQLite commit");
                check(reserve_attachment_object(*db, key, 10).ok(), "persist the reservation before PUT");
                auto transfer = objects.put(key, bytes, "application/octet-stream");
                check(transfer.has_value(), "upload before the failed database commit");
                if (transfer)
                {
                    uploaded_attachment attachment{key, "file.bin", "application/octet-stream",
                                                   static_cast<std::int64_t>(bytes.size()), *transfer};
                    auto rejected = write_message(*db, {1, 1, 10, "file.bin", {}}, &attachment);
                    check(!rejected && rejected.error().code == SQLITE_CONSTRAINT_FOREIGNKEY,
                          "Silo upload succeeds while publishing transaction fails at commit");
                }
            }
        }
        {
            auto db = sqlite_database::open(file);
            check(db.has_value(), "reopen SQLite after the failed commit");
            if (db)
            {
                auto live = db->prepare("SELECT count(*) FROM messages");
                auto result = live ? live->step() : std::expected<bool, sqlite_error>{false};
                check(result && *result && live->column_int64(0) == 0, "no message was published by the failed commit");
                live = sqlite_statement{};
                auto too_early = attachment_cleanup_batch(*db, 9);
                auto abandoned = attachment_cleanup_batch(*db, 10);
                check(too_early && too_early->empty() && abandoned && *abandoned == std::vector{key},
                      "failed upload reservation survives restart and honors the cleanup cutoff");
                check(objects.remove(key) && acknowledge_attachment_cleanup(*db, key).ok(),
                      "delete the orphan object before acknowledging cleanup");
                check(!objects.read(key, bytes.size(), 0, bytes.size()), "orphan object is gone");
                check(db->exec("DROP TRIGGER fail_attachment_commit").ok(), "allow a subsequent commit");
                check(reserve_attachment_object(*db, key, 20).ok(), "reserve the cleaned key");
                auto transfer = objects.put(key, bytes, "application/octet-stream");
                check(transfer.has_value(), "upload attachment for a normal commit");
                if (transfer)
                {
                    uploaded_attachment attachment{key, "file.bin", "application/octet-stream",
                                                   static_cast<std::int64_t>(bytes.size()), *transfer};
                    auto published = write_message(*db, {1, 1, 11, "file.bin", {}}, &attachment);
                    auto pending = attachment_cleanup_batch(*db, 20);
                    check(published && std::holds_alternative<message_write>(*published) && pending && pending->empty(),
                          "publish message and claim reserved object together");
                    check(db->exec("UPDATE messages SET deleted=1,body=''").ok(), "delete the message");
                    pending = attachment_cleanup_batch(*db, 20);
                    auto metadata = db->prepare("SELECT count(*) FROM message_attachments");
                    auto remaining = metadata ? metadata->step() : std::expected<bool, sqlite_error>{false};
                    check(remaining && *remaining && metadata->column_int64(0) == 0 &&
                          pending && *pending == std::vector{key},
                          "message deletion hides metadata and atomically queues object removal");
                    check(objects.remove(key) && acknowledge_attachment_cleanup(*db, key).ok(), "finish object cleanup");
                }
            }
        }
        unlink(file);
    }

    minio::s3::RemoveBucketArgs cleanup;
    cleanup.bucket = settings.bucket;
    cleanup.region = settings.region;
    check(static_cast<bool>(client.RemoveBucket(cleanup)), "remove the test bucket");
    return passed ? 0 : 1;
}
