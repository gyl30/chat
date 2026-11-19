#ifndef CHAT_SRC_SERVER_HPP
#define CHAT_SRC_SERVER_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <system_error>

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tls_context.hpp>
#include <chat/detail/websocket_heartbeat.hpp>
#include <boost/http/server/router.hpp>

#include "invite_attempts.hpp"
#include "blocking_worker.hpp"
#include "online_users.hpp"
class database;
class s3_objects;

class chat_server
{
   public:
    chat_server(boost::corosio::io_context& io_context,
                std::size_t worker_count,
                boost::http::router<boost::http::route_params> router,
                database& storage,
                s3_objects& objects,
                std::optional<boost::corosio::tls_context> tls = std::nullopt,
                chat::detail::websocket_heartbeat_config heartbeat = {});

    std::error_code bind(boost::corosio::endpoint endpoint);

    void start();

    void stop();

    void join();

   private:
    online_users users_;
    invite_attempts invite_attempts_;
    blocking_worker passwords_{64, 2};
    boost::corosio::tcp_server server_;
};

#endif
