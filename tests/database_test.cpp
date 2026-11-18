#include <atomic>
#include <chrono>
#include <sqlite3.h>
#include <unistd.h>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>

#include "database.hpp"
#include "schema.hpp"

namespace
{

int failures = 0;

void check(bool condition, std::string const& message)
{
    if (condition)
    {
        std::cout << "PASS " << message << '\n';
    }
    else
    {
        std::cerr << "FAIL " << message << '\n';
        ++failures;
    }
}

struct temporary_database
{
    std::string path;

    temporary_database()
    {
        auto const base = std::filesystem::temp_directory_path() /
            ("chat_sqlite_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        path = base.string();
        std::filesystem::remove(path);
        std::filesystem::remove(path + "-wal");
        std::filesystem::remove(path + "-shm");
    }

    ~temporary_database()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path + "-wal", error);
        std::filesystem::remove(path + "-shm", error);
    }

    static inline std::atomic<int> counter{0};
};

std::int64_t count(sqlite_database& database, std::string_view table)
{
    auto statement = database.prepare("SELECT count(*) FROM " + std::string(table));
    if (!statement)
    {
        return -1;
    }
    auto row = statement->step();
    if (!row || !*row)
    {
        return -1;
    }
    return statement->column_int64(0);
}

boost::capy::task<> low_level_tests(temporary_database& file)
{
    {
        auto opened = sqlite_database::open(file.path);
        check(opened.has_value(), "first open creates the database");
        if (!opened)
        {
            co_return;
        }
        check(initialize_schema(*opened).ok(), "first open creates the schema");

        auto statement = opened->prepare("PRAGMA user_version");
        auto row = statement ? statement->step() : std::expected<bool, sqlite_error>{};
        check(statement && row && *row && statement->column_int64(0) == chat_schema_version, "schema version is recorded");

        auto insert = opened->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES(?,?,?)");
        check(insert.has_value(), "prepare insert");
        if (insert)
        {
            check(insert->bind(1, "alice").ok() && insert->bind(2, std::string(60, 'x')).ok() && insert->bind(3, 1).ok(),
                  "bind insert parameters");
            check(insert->step().has_value(), "insert user");
            check(opened->last_insert_rowid() == 1, "first user id is one");
        }

        auto conversation = opened->prepare("INSERT INTO conversations(kind,title,activity) VALUES('group','group',0)");
        check(conversation.has_value(), "prepare conversation insert");
        if (conversation)
        {
            check(conversation->step().has_value(), "insert conversation");
        }

        auto missing_user = opened->prepare(
            "INSERT INTO messages(conversation_id,sender_id,body,created_at) VALUES(1,999,'body',0)");
        check(missing_user.has_value(), "prepare foreign key insert");
        if (missing_user)
        {
            auto result = missing_user->step();
            check(!result && result.error().code == SQLITE_CONSTRAINT_FOREIGNKEY, "foreign key violation is reported");
        }

        auto transaction = sqlite_transaction::begin_immediate(*opened);
        check(transaction.has_value(), "begin immediate transaction");
        if (transaction)
        {
            if (auto statement = opened->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES('bob','" +
                    std::string(60, 'y') + "',1)"); statement)
            {
                check(statement->step().has_value(), "insert inside transaction");
            }
            check(transaction->rollback().ok(), "explicit rollback");
        }
        check(count(*opened, "users") == 1, "rollback removes the inserted user");

        auto committed = sqlite_transaction::begin_immediate(*opened);
        if (committed)
        {
            if (auto statement = opened->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES('carol','" +
                    std::string(60, 'z') + "',1)"); statement)
            {
                check(statement->step().has_value(), "insert before commit");
            }
            check(committed->commit().ok(), "explicit commit");
        }
        check(count(*opened, "users") == 2, "commit persists the user");

        auto member = opened->prepare("INSERT INTO conversation_members(conversation_id,user_id) VALUES(1,1)");
        check(member && member->step().has_value(), "insert group member");
        auto cascade = opened->prepare("DELETE FROM users WHERE id=1");
        check(cascade && cascade->step().has_value(), "delete user");
        check(count(*opened, "conversation_members") == 0, "foreign key cascade deletes memberships");
    }

    {
        auto reopened = sqlite_database::open(file.path);
        check(reopened.has_value(), "reopen existing database");
        if (reopened)
        {
            check(initialize_schema(*reopened).ok(), "reopen accepts the stored schema version");
            check(count(*reopened, "users") == 1, "reopen sees committed data");
        }
    }

    {
        auto read_only = sqlite_database::open_read_only(file.path);
        check(read_only.has_value(), "open read-only connection");
        if (read_only)
        {
            auto statement = read_only->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES('dave','" +
                std::string(60, 'w') + "',1)");
            check(statement.has_value(), "prepare write on a read-only connection");
            if (statement)
            {
                auto result = statement->step();
                check(!result && (result.error().code & 0xff) == SQLITE_READONLY, "read-only write is rejected");
            }
        }
    }

    {
        auto first = sqlite_database::open(file.path, 0);
        auto second = sqlite_database::open(file.path, 0);
        check(first && second, "open two writers for busy testing");
        if (first && second)
        {
            check(first->exec("BEGIN IMMEDIATE").ok(), "first connection takes the write lock");
            auto insert = second->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES('eve','" +
                std::string(60, 'v') + "',1)");
            check(insert.has_value(), "prepare competing write");
            if (insert)
            {
                auto result = insert->step();
                check(!result && ((result.error().code & 0xff) == SQLITE_BUSY || (result.error().code & 0xff) == SQLITE_LOCKED),
                      "competing write reports busy");
            }
            check(first->exec("COMMIT").ok(), "first connection releases the write lock");
            if (insert)
            {
                check(insert->step().has_value(), "competing write succeeds after the lock is released");
            }
        }
    }

    {
        auto opened = sqlite_database::open(file.path);
        check(opened.has_value(), "open for disk-full test");
        if (opened)
        {
            auto pages = opened->prepare("PRAGMA page_count");
            auto row = pages ? pages->step() : std::expected<bool, sqlite_error>{};
            check(pages && row && *row, "read page count");
            std::int64_t const limit = pages ? pages->column_int64(0) : 0;
            check(opened->exec("PRAGMA max_page_count=" + std::to_string(limit)).ok(), "limit the database size");
            auto insert = opened->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES(?,?,1)");
            if (insert)
            {
                check(insert->bind(1, std::string(8192, 'u')).ok() && insert->bind(2, std::string(60, 'u')).ok(),
                      "bind oversized value");
            }
            auto result = insert ? insert->step() : std::expected<bool, sqlite_error>{};
            check(insert && !result && (result.error().code & 0xff) == SQLITE_FULL, "disk full is reported");
            check(count(*opened, "users") >= 1, "database remains usable after a failed write");
        }
    }

    co_return;
}

boost::capy::task<> gated_job(database& db, std::promise<void>& started, std::future<void>& gate, std::atomic<int>& done)
{
    auto result = co_await db.run([&](sqlite_database&) -> std::expected<void, sqlite_error>
    {
        started.set_value();
        gate.wait();
        ++done;
        return {};
    });
    check(result.has_value(), "gated database job completes");
}

boost::capy::task<> counted_job(database& db, std::atomic<int>& done, int total, std::promise<void>& all_done)
{
    auto result = co_await db.run([&](sqlite_database&) -> std::expected<void, sqlite_error>
    {
        if (++done == total)
        {
            all_done.set_value();
        }
        return {};
    });
    check(result.has_value(), "drained database job completes");
}

boost::capy::task<> queue_test(temporary_database& file, boost::corosio::io_context& io)
{
    auto opened = database::open(file.path, 1);
    check(opened.has_value(), "open bounded database");
    if (!opened)
    {
        co_return;
    }
    auto& db = **opened;

    std::atomic<int> done{0};
    std::promise<void> first_started;
    std::promise<void> first_gate;
    auto first_started_future = first_started.get_future();
    auto first_gate_future = first_gate.get_future();
    boost::capy::run_async(io.get_executor())(gated_job(db, first_started, first_gate_future, done));
    first_started_future.wait();

    std::promise<void> second_started;
    std::promise<void> second_gate;
    auto second_started_future = second_started.get_future();
    auto second_gate_future = second_gate.get_future();
    boost::capy::run_async(io.get_executor())(gated_job(db, second_started, second_gate_future, done));

    for (int attempt = 0; attempt < 1000 && db.pending_jobs() != 1; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    check(db.pending_jobs() == 1, "job is queued behind the gated job");

    auto full = co_await db.run([](sqlite_database&) -> std::expected<void, sqlite_error> { return {}; });
    check(!full && full.error().message.find("full") != std::string::npos,
          "bounded queue rejects excess work");

    first_gate.set_value();
    second_started_future.wait();
    second_gate.set_value();
    for (int attempt = 0; attempt < 1000 && done != 2; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    check(done == 2, "queued job runs after the first finishes");
    co_return;
}

boost::capy::task<> async_tests(temporary_database& file, boost::corosio::io_context& io)
{
    auto opened = database::open(file.path, 64);
    check(opened.has_value(), "open asynchronous database");
    if (!opened)
    {
        co_return;
    }
    auto& db = **opened;

    std::weak_ptr<int> job_payload;
    auto deferred = [&] {
        auto payload = std::make_shared<int>(42);
        job_payload = payload;
        return db.run([payload](sqlite_database&) -> std::expected<int, sqlite_error> { return *payload; });
    }();
    bool const owns_job = !job_payload.expired();
    check(owns_job, "a deferred database task owns its job before suspension");
    if (owns_job)
    {
        auto result = co_await std::move(deferred);
        check(result && *result == 42, "a deferred database task returns its owned payload");
    }

    std::atomic<int> completed{0};
    std::promise<void> first_started;
    std::promise<void> first_gate;
    auto first_started_future = first_started.get_future();
    auto first_gate_future = first_gate.get_future();

    boost::capy::run_async(io.get_executor())(
        gated_job(db, first_started, first_gate_future, completed));
    first_started_future.wait();
    check(db.pending_jobs() == 0, "running job is not queued");

    first_gate.set_value();
    for (int attempt = 0; attempt < 1000 && completed != 1; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    check(completed == 1, "gated job completes");

    std::atomic<int> parallel_completed{0};
    std::promise<void> parallel_all_done;
    auto parallel_all_done_future = parallel_all_done.get_future();
    for (int i = 0; i < 32; ++i)
    {
        boost::capy::run_async(io.get_executor())(
            counted_job(db, parallel_completed, 32, parallel_all_done));
    }
    parallel_all_done_future.wait();
    check(parallel_completed == 32, "many concurrent jobs complete in order");

    std::atomic<int> blocked{0};
    std::atomic<int> drained{0};
    std::promise<void> drain_started;
    std::promise<void> drain_gate;
    std::promise<void> drain_done;
    auto drain_started_future = drain_started.get_future();
    auto drain_gate_future = drain_gate.get_future();
    auto drain_done_future = drain_done.get_future();
    boost::capy::run_async(io.get_executor())(
        gated_job(db, drain_started, drain_gate_future, blocked));
    drain_started_future.wait();
    for (int i = 0; i < 4; ++i)
    {
        boost::capy::run_async(io.get_executor())(
            counted_job(db, drained, 4, drain_done));
    }
    for (int attempt = 0; attempt < 1000 && db.pending_jobs() != 4; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    check(db.pending_jobs() == 4, "four jobs wait in the queue");
    drain_gate.set_value();
    db.shutdown();
    drain_done_future.wait();
    check(drained == 4, "shutdown drains queued jobs");
    co_return;
}

}    // namespace

int main()
{
    temporary_database file;
    boost::corosio::io_context io_context;
    int exit_code = 1;
    boost::capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(
        [](temporary_database& file, boost::corosio::io_context& io_context) -> boost::capy::task<int>
        {
            co_await low_level_tests(file);
            co_await queue_test(file, io_context);
            co_await async_tests(file, io_context);
            co_return failures == 0 ? 0 : 1;
        }(file, io_context));
    io_context.run();
    return exit_code;
}
