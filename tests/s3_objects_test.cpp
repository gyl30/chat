#include "s3_objects.hpp"
#include "attachment_cleanup.hpp"
#include "attachment_io.hpp"
#include "database.hpp"
#include "message_store.hpp"
#include "schema.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sqlite3.h>
#include <vector>
#include <iostream>
#include <limits>
#include <string>
#include <unistd.h>

#include <miniocpp/client.h>
#include <boost/capy/test/run_blocking.hpp>

namespace
{
struct socket_handle
{
    int fd;
    socket_handle(socket_handle const&) = delete;
    socket_handle& operator=(socket_handle const&) = delete;
    explicit socket_handle(int descriptor) : fd(descriptor) {}
    ~socket_handle()
    {
        if (fd >= 0)
        {
            close(fd);
        }
    }
};

boost::capy::task<bool> check_connection_failures(s3_settings settings)
{
    using namespace std::chrono_literals;
    socket_handle listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (listener.fd == -1 || bind(listener.fd, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
        getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &size) != 0)
    {
        co_return false;
    }
    settings.endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    s3_objects network_probe(std::move(settings));
    auto refused = co_await network_probe.read("attachments/test", 7, 0, 7);
    if (refused || refused.error().status != 0 || listen(listener.fd, 2) != 0)
    {
        co_return false;
    }
    std::cout << "PASS refused S3 connection returns a transport error\n";

    std::atomic<bool> accepted_stalled_request = false;
    std::atomic<bool> release_stall = false;
    std::jthread fault_server([&] {
        pollfd waiting{listener.fd, POLLIN, 0};
        if (poll(&waiting, 1, 5000) <= 0)
        {
            return;
        }
        socket_handle incomplete(accept(listener.fd, nullptr, nullptr));
        if (incomplete.fd < 0)
        {
            return;
        }
        pollfd ready{incomplete.fd, POLLIN, 0};
        if (poll(&ready, 1, 5000) <= 0)
        {
            return;
        }
        char request[4096];
        if (recv(incomplete.fd, request, sizeof(request), 0) <= 0)
        {
            return;
        }
        constexpr std::string_view truncated =
            "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-6/7\r\nContent-Length: 7\r\n"
            "Connection: close\r\n\r\n123";
        if (send(incomplete.fd, truncated.data(), truncated.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(truncated.size()))
        {
            return;
        }
        shutdown(incomplete.fd, SHUT_RDWR);
        if (poll(&waiting, 1, 5000) <= 0)
        {
            return;
        }
        socket_handle stalled(accept(listener.fd, nullptr, nullptr));
        if (stalled.fd < 0)
        {
            return;
        }
        accepted_stalled_request = true;
        release_stall.wait(false);
    });

    auto truncated = co_await network_probe.read("attachments/test", 7, 0, 7);
    auto const start = std::chrono::steady_clock::now();
    auto timeout = co_await network_probe.read("attachments/test", 7, 0, 7);
    auto const elapsed = std::chrono::steady_clock::now() - start;
    release_stall = true;
    release_stall.notify_one();
    bool const deadline = accepted_stalled_request && !timeout && elapsed >= 12s && elapsed < 25s;
    std::cout << (!truncated ? "PASS" : "FAIL") << " incomplete response produces no partial bytes\n";
    std::cout << (deadline ? "PASS" : "FAIL") << " stalled S3 request returns within its deadline\n";
    co_return !truncated && deadline;
}
} // namespace

boost::capy::task<int> exercise_silo()
{
    auto const* endpoint = std::getenv("CHAT_TEST_S3_ENDPOINT");
    auto const* access_key = std::getenv("CHAT_TEST_S3_ACCESS_KEY");
    auto const* secret_key = std::getenv("CHAT_TEST_S3_SECRET_KEY");
    if (!endpoint || !access_key || !secret_key)
    {
        std::cout << "BLOCKED: isolated Silo endpoint and credentials are required\n";
        co_return 77;
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
        co_return 1;
    }

    s3_objects objects(settings);
    std::string const bytes("123\0abc", 7);
    std::string const key = "attachments/test";
    auto stored = co_await objects.put(key, bytes, "application/octet-stream");
    if (!stored)
    {
        std::cerr << "PUT status " << stored.error().status << ": " << stored.error().detail << '\n';
    }
    check(stored && stored->size() == 64, "Silo validates attachment SHA-256 and object size");
    auto duplicate = co_await objects.put(key, "replacement", "application/octet-stream");
    check(!duplicate && duplicate.error().status == 412, "a second PUT cannot overwrite the object");
    auto chunk = co_await objects.read(key, bytes.size(), 2, 4);
    if (!chunk)
    {
        std::cerr << "GET status " << chunk.error().status << ": " << chunk.error().detail << '\n';
    }
    check(chunk && *chunk == bytes.substr(2, 4), "Silo returns an exact binary byte range");
    auto wrong_size = co_await objects.read(key, bytes.size() + 1, 2, 4);
    check(!wrong_size, "download rejects mismatched metadata");
    check(!(co_await objects.read(key, bytes.size(), bytes.size() + 1, 0)), "invalid range rejected locally");
    check((co_await objects.remove(key)).has_value(), "delete the private object");
    auto missing = co_await objects.read(key, bytes.size(), 0, bytes.size());
    check(!missing && missing.error().status == 404, "deleted object is unavailable");
    check((co_await objects.remove(key)).has_value(), "cleanup retry is idempotent");

    auto empty = co_await objects.put(key, {}, "application/octet-stream");
    check(empty.has_value(), "Silo accepts empty attachments");
    check((co_await objects.remove(key)).has_value(), "delete the empty object");
    auto bad_credentials = settings;
    bad_credentials.secret_key += "invalid";
    s3_objects denied(std::move(bad_credentials));
    auto unauthenticated = co_await denied.read(key, bytes.size(), 0, bytes.size());
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
                    INSERT INTO users(username,password_hash,last_seen_at) VALUES('Alice',printf('%060d',1),0),('Bob',printf('%060d',2),0),('Outsider',printf('%060d',3),0);
                    INSERT INTO conversations(kind,activity,direct_user_low,direct_user_high) VALUES('direct',0,1,2);
                    INSERT INTO conversation_members(conversation_id,user_id) VALUES(1,1),(1,2);
                    INSERT INTO contacts(owner_id,contact_id,created_at) VALUES(1,2,0),(2,1,0);
                    CREATE TABLE failure(message INTEGER REFERENCES messages(id) DEFERRABLE INITIALLY DEFERRED);
                    CREATE TRIGGER fail_attachment_commit AFTER INSERT ON message_attachments
                    BEGIN INSERT INTO failure(message) VALUES(-1); END;
                )SQL").ok(), "prepare a deferred failure at SQLite commit");
            }
        }
        auto const attachment = [&](std::int64_t client_id) {
            return completed_attachment_upload{1, 1, client_id, {}, "file.bin", "application/octet-stream", bytes};
        };
        {
            auto db = database::open(file);
            check(db.has_value(), "open SQLite worker for interrupted upload");
            if (db)
            {
                auto rejected = co_await publish_attachment(**db, objects, attachment(10));
                auto const* error = rejected ? nullptr : std::get_if<sqlite_error>(&rejected.error());
                check(error && error->code == SQLITE_CONSTRAINT_FOREIGNKEY,
                      "Silo upload succeeds while publishing transaction fails at commit");
            }
        }
        auto db = database::open(file);
        check(db.has_value(), "reopen SQLite after the failed commit");
        if (db)
        {
            constexpr auto cutoff = std::numeric_limits<std::int64_t>::max();
            auto recovery_snapshot = [](sqlite_database& connection) {
                return attachment_cleanup_batch(connection, cutoff);
            };
            auto abandoned = co_await (*db)->run(recovery_snapshot);
            check(abandoned && abandoned->size() == 1 && abandoned->front().starts_with("attachments/") &&
                  abandoned->front().find("file.bin") == std::string::npos,
                  "failed upload reservation retains an opaque object key across restart");
            if (abandoned && abandoned->size() == 1)
            {
                auto before_cleanup = co_await objects.read(abandoned->front(), bytes.size(), 0, bytes.size());
                check(before_cleanup && *before_cleanup == bytes, "orphan object really exists on Silo");
                auto recovered = co_await recover_attachment_objects(**db, objects, cutoff);
                check(recovered && *recovered == 1, "recovery deletes before acknowledging");
                check(!(co_await objects.read(abandoned->front(), bytes.size(), 0, bytes.size())), "orphan object is gone");
            }
            auto drop_failure = [](sqlite_database& connection) -> std::expected<void, sqlite_error> {
                if (auto error = connection.exec("DROP TRIGGER fail_attachment_commit"); error)
                {
                    return std::unexpected(std::move(error));
                }
                return {};
            };
            check((co_await (*db)->run(drop_failure)).has_value(), "allow a subsequent commit");
            auto published = co_await publish_attachment(**db, objects, attachment(11));
            auto repeated = co_await publish_attachment(**db, objects, attachment(11));
            auto pending = co_await (*db)->run(recovery_snapshot);
            auto const* replayed = repeated ? std::get_if<message_write>(&*repeated) : nullptr;
            auto const* saved = published ? std::get_if<message_write>(&*published) : nullptr;
            check(saved && saved->id == 1 && pending && pending->empty(),
                  "publish message and claim its object together after the failed transaction");
            check(saved && replayed && replayed->id == saved->id && !replayed->inserted && pending && pending->empty(),
                  "replay settles without adding a reservation or writing another object");
            if (saved)
            {
                auto allowed = co_await download_attachment_chunk(**db, objects, 2, 1, saved->id, 0);
                auto denied = co_await download_attachment_chunk(**db, objects, 3, 1, saved->id, 0);
                auto wrong = co_await download_attachment_chunk(**db, objects, 2, 99, saved->id, 0);
                check(allowed && *allowed && (*allowed)->bytes == bytes && (*allowed)->size == bytes.size() &&
                      denied && !*denied && wrong && !*wrong,
                      "download returns bytes only for a current member in the correct conversation");
                auto remove_message = [](sqlite_database& connection) -> std::expected<void, sqlite_error> {
                    if (auto error = connection.exec("UPDATE messages SET deleted=1,body=''"); error)
                    {
                        return std::unexpected(std::move(error));
                    }
                    return {};
                };
                check((co_await (*db)->run(remove_message)).has_value(), "delete the message");
                auto deleted = co_await download_attachment_chunk(**db, objects, 2, 1, saved->id, 0);
                check(deleted && !*deleted, "deleted message denies attachment access before remote cleanup");
                auto finalized = co_await recover_attachment_objects(**db, objects, cutoff);
                check(finalized && *finalized == 1, "deletion atomically records the object for cleanup");
            }
            (*db).reset();
        }
        unlink(file);
    }

    minio::s3::RemoveBucketArgs cleanup;
    cleanup.bucket = settings.bucket;
    cleanup.region = settings.region;
    check(static_cast<bool>(client.RemoveBucket(cleanup)), "remove the test bucket");
    check(co_await check_connection_failures(settings), "connection fault regressions");
    co_return passed ? 0 : 1;
}


int main()
{
    int status = 1;
    boost::capy::test::run_blocking([&](int result) { status = result; })(exercise_silo());
    return status;
}
