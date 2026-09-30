#ifndef CHAT_SRC_PG_CONNECTION_POOL_HPP
#define CHAT_SRC_PG_CONNECTION_POOL_HPP

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>

#include "pg_connection.hpp"

class pg_connection_pool
{
   public:
    class lease
    {
       public:
        lease() = default;
        lease(lease const&) = delete;
        lease& operator=(lease const&) = delete;
        lease(lease&& other) noexcept;
        lease& operator=(lease&& other) noexcept;
        ~lease();

        std::error_code error() const noexcept;
        pg_connection& connection() noexcept;

       private:
        friend class pg_connection_pool;

        lease(pg_connection_pool& pool, std::size_t index, std::error_code error) noexcept;
        explicit lease(std::error_code error) noexcept;

        pg_connection_pool* pool_ = nullptr;
        std::size_t index_ = 0;
        std::error_code error_;
    };

    pg_connection_pool(boost::corosio::io_context& io_context,
                       std::string connection_string,
                       std::size_t connection_count);

    boost::capy::task<lease> acquire();

   private:
    void release(std::size_t index) noexcept;

    std::string connection_string_;
    std::vector<std::unique_ptr<pg_connection>> connections_;
    std::deque<std::size_t> available_;
    boost::capy::async_event available_event_;
};

#endif
