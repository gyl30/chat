#ifndef CHAT_SRC_CHAT_SESSION_HPP
#define CHAT_SRC_CHAT_SESSION_HPP

#include <boost/capy/task.hpp>

#include "websocket.hpp"

class chat_session
{
   public:
    explicit chat_session(websocket_connection& connection);

    boost::capy::task<void> run();

   private:
    websocket_connection& connection_;
};

#endif
