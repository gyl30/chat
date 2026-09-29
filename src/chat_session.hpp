#ifndef CHAT_SRC_CHAT_SESSION_HPP
#define CHAT_SRC_CHAT_SESSION_HPP

#include <boost/capy/task.hpp>

#include "json_rpc.hpp"
#include "websocket.hpp"

class chat_session
{
   public:
    explicit chat_session(websocket_connection& connection);

    boost::capy::task<void> run();

   private:
    boost::capy::task<simdjson::error_code> handle_echo(json_rpc_request& request, std::string& response);

    websocket_connection& connection_;
};

#endif
