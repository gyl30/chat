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
        return worker_.run(
            [this, job = std::move(job)] mutable -> std::invoke_result_t<F&, sqlite_database&>
            {
                if (!database_.is_open())
                {
                    auto reopened = sqlite_database::open(path_);
                    if (!reopened)
                    {
                        return std::unexpected(std::move(reopened.error()));
                    }
                    database_ = std::move(*reopened);
                }
                return job(database_);
            },
            queue_error);
    }

   private:
    database(std::string path, sqlite_database connection);
    static sqlite_error queue_error(blocking_worker::unavailable reason);

    std::string path_;
    sqlite_database database_;
    blocking_worker worker_{1024};
};

#endif
