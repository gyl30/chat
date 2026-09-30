#ifndef CHAT_SRC_CHAT_SESSION_HPP
#define CHAT_SRC_CHAT_SESSION_HPP

#include <cstdint>
#include <optional>
#include <string>

#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>

#include "json_rpc.hpp"
#include "websocket.hpp"

class chat_session
{
   public:
    chat_session(boost::corosio::io_context& io_context, websocket_connection& connection, std::string const& database_connection_string);

    boost::capy::task<void> run();

   private:
    boost::capy::task<simdjson::error_code> handle_authenticate(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_echo(json_rpc_request& request, std::string& response);

    boost::corosio::io_context& io_context_;
    websocket_connection& connection_;
    std::string const& database_connection_string_;
    std::optional<std::int64_t> user_id_;
};

#endif
