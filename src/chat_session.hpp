#ifndef CHAT_SRC_CHAT_SESSION_HPP
#define CHAT_SRC_CHAT_SESSION_HPP

#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <string>

#include <boost/capy/task.hpp>
#include "json_rpc.hpp"
#include "online_users.hpp"
#include "websocket.hpp"

class pg_connection_pool;
class pg_connection;

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
    boost::capy::task<simdjson::error_code> handle_attachment(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_avatar(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_echo(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_conversations(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_conversation_preference(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_contacts(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_create_conversation(json_rpc_request& request,
                                                                       std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_members(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_group_management(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_group_invite(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_group_join_request(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_messages(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_presence(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_unread_count(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_mark_read(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_set_typing(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_register(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_update_message(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_message_reaction(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_send_message(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_search_users(json_rpc_request& request, std::string& response);

    boost::capy::task<void> publish_presence(bool online);

    boost::capy::task<std::expected<bool, std::error_code>> check_conversation_send(pg_connection& connection,
                                                                                std::int64_t conversation);
    boost::capy::task<std::expected<bool, std::error_code>> publish_conversation(std::int64_t conversation,
        std::string notification, bool require_sender_permission = false);
    boost::capy::task<void> publish_join_request(std::int64_t conversation, std::int64_t user, std::string state);

    bool enqueue_message(std::string message);

    websocket_connection& connection_;
    online_users& users_;
    pg_connection_pool& database_;
    std::optional<std::int64_t> user_id_;
    std::string username_;
    std::deque<std::string> outgoing_messages_;
    struct attachment_upload
    {
        std::int64_t id;
        std::int64_t conversation;
        std::string filename;
        std::int64_t size;
        std::string content;
    };
    std::optional<attachment_upload> upload_;
    std::int64_t next_upload_id_ = 1;
    struct avatar_upload
    {
        std::int64_t id;
        std::int64_t size;
        std::string content;
    };
    std::optional<avatar_upload> avatar_upload_;
};

#endif
