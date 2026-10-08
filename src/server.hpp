#ifndef CHAT_SRC_SERVER_HPP
#define CHAT_SRC_SERVER_HPP

#include <cstddef>
#include <string>
#include <system_error>

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/http/server/router.hpp>

#include "invite_attempts.hpp"
#include "online_users.hpp"
#include "pg_connection_pool.hpp"

class chat_server
{
   public:
    chat_server(boost::corosio::io_context& io_context,
                std::size_t worker_count,
                boost::http::router<boost::http::route_params> router,
                std::string database_connection_string,
                std::size_t database_connection_count);

    std::error_code bind(boost::corosio::endpoint endpoint);

    boost::corosio::endpoint local_endpoint(std::size_t index = 0) const noexcept;

    void start();

    void stop();

    void join();

   private:
    online_users users_;
    invite_attempts invite_attempts_;
    pg_connection_pool database_;
    boost::corosio::tcp_server server_;
};

#endif
