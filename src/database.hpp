#ifndef CHAT_SRC_DATABASE_HPP
#define CHAT_SRC_DATABASE_HPP

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "blocking_worker.hpp"
#include "sqlite_database.hpp"

class database
{
   public:
    using pointer = std::unique_ptr<database>;

    static std::expected<pointer, sqlite_error> open(std::string path, std::size_t queue_capacity = 1024);

    database(database const&) = delete;
    database& operator=(database const&) = delete;

    // A whole transaction must fit in one job so the connection never yields
    // while it owns the write lock.
    template <class F>
    auto run(F job) -> boost::capy::task<std::invoke_result_t<F&, sqlite_database&>>
    {
        return worker_.run([this, job = std::move(job)]() mutable { return job(database_); },
                           queue_error);
    }

    void shutdown() noexcept { worker_.shutdown(); }
    std::size_t pending_jobs() const { return worker_.pending_jobs(); }

   private:
    database(sqlite_database connection, std::size_t queue_capacity);
    static sqlite_error queue_error(blocking_worker::unavailable reason);

    sqlite_database database_;
    blocking_worker worker_;
};

#endif
