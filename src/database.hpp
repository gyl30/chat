#ifndef CHAT_SRC_DATABASE_HPP
#define CHAT_SRC_DATABASE_HPP

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

    static std::expected<pointer, sqlite_error> open(std::string path);

    database(database const&) = delete;
    database& operator=(database const&) = delete;

    // A whole transaction must fit in one job so the connection never yields
    // while it owns the write lock.
    template <class F>
    auto run(F job) -> boost::capy::task<std::invoke_result_t<F&, sqlite_database&>>
    {
        return run_on(worker_, database_, sqlite_database::open_mode::incremental_vacuum, std::move(job));
    }

    template <class F>
    auto search(F job) -> boost::capy::task<std::invoke_result_t<F&, sqlite_database&>>
    {
        return run_on(search_worker_, reader_, sqlite_database::open_mode::read_only, std::move(job));
    }

   private:
    template <class F>
    auto run_on(blocking_worker& worker, sqlite_database& db, sqlite_database::open_mode mode, F job)
    {
        return worker.run(
            [this, &db, mode, job = std::move(job)] mutable -> std::invoke_result_t<F&, sqlite_database&>
            {
                if (!db.is_open())
                {
                    auto reopened = open_connection(path_, mode);
                    if (!reopened)
                    {
                        return std::unexpected(std::move(reopened.error()));
                    }
                    db = std::move(*reopened);
                }
                return job(db);
            },
            queue_error);
    }

    database(std::string path, sqlite_database connection);
    static std::expected<sqlite_database, sqlite_error> open_connection(std::string const& path, sqlite_database::open_mode mode);
    static sqlite_error queue_error(blocking_worker::unavailable reason);

    std::string path_;
    sqlite_database database_;
    sqlite_database reader_;
    blocking_worker worker_{1024};
    blocking_worker search_worker_{16};
};

#endif
