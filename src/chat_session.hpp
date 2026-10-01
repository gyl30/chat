#ifndef CHAT_SRC_CHAT_SESSION_HPP
#define CHAT_SRC_CHAT_SESSION_HPP

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include <boost/capy/task.hpp>
#include "json_rpc.hpp"
#include "online_users.hpp"
#include "websocket.hpp"

class pg_connection_pool;

class chat_session
{
   public:
    chat_session(websocket_connection& connection,
                 online_users& users,
                 pg_connection_pool& database);

    boost::capy::task<void> run();

   private:
    boost::capy::task<simdjson::error_code> handle_authenticate(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_contact_change(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_echo(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_conversations(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_contacts(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_create_conversation(json_rpc_request& request,
                                                                       std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_members(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_messages(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_presence(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_unread_count(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_mark_read(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_register(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_update_message(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_send_message(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_search_users(json_rpc_request& request, std::string& response);

    boost::capy::task<void> publish_presence(bool online);

    boost::capy::task<bool> publish_conversation(std::int64_t conversation, std::string notification);

    bool enqueue_message(std::string message);

    websocket_connection& connection_;
    online_users& users_;
    pg_connection_pool& database_;
    std::optional<std::int64_t> user_id_;
    std::string username_;
    std::deque<std::string> outgoing_messages_;
};

#endif
