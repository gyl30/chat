#ifndef CHAT_SRC_CHAT_SESSION_HPP
#define CHAT_SRC_CHAT_SESSION_HPP

#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/capy/task.hpp>
#include <sqlite3.h>
#include "database.hpp"
#include "json_rpc.hpp"
#include "login_session_store.hpp"
#include "timestamp.hpp"
#include "online_users.hpp"
#include "sqlite_database.hpp"
#include "websocket.hpp"

class invite_attempts;
class blocking_worker;
class database;
class s3_objects;

namespace chat
{
struct conversation_view_event;
}
struct quoted_notification;

class chat_session
{
   public:
    chat_session(websocket_connection& connection,
                 online_users& users,
                 invite_attempts& invite_attempts,
                 blocking_worker& passwords,
                 database& storage,
                 s3_objects& objects);

    boost::capy::task<void> run();

   private:
    boost::capy::task<void> exchange_messages();
    simdjson::error_code update_avatar_upload(simdjson::ondemand::document& document, json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> get_avatar_chunk(simdjson::ondemand::document& document,
                                                             json_rpc_request& request,
                                                             std::string& response);
    boost::capy::task<simdjson::error_code> dispatch_request(json_rpc_request& request, std::string& response);
    void invalidate_login();
    void publish_login_changes(std::span<std::int64_t const> revoked);
    boost::capy::task<simdjson::error_code> handle_login_management(json_rpc_request& request, std::string& response);

    template <class F>
    auto run_db(F job)
    {
        // Check and run in the same serialized database job, including continuation after S3 work.
        return database_.run(
            [id = login_id_, job = std::move(job)](sqlite_database& db) mutable -> std::invoke_result_t<F&, sqlite_database&>
            {
                auto active = login_active(db, id, epoch_milliseconds());
                if (!active)
                {
                    return std::unexpected(active.error());
                }
                if (!*active)
                {
                    return std::unexpected(sqlite_error{SQLITE_AUTH, "Login session no longer valid"});
                }
                return job(db);
            });
    }
    boost::capy::task<simdjson::error_code> handle_authenticate(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_contact_change(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_conversation_file(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_attachment(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_avatar(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_conversations(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_conversation_preference(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_contact_settings(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_get_contacts(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_create_conversation(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_members(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_group_management(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_group_invite(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_group_join_request(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_messages(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_get_presence(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_mark_read(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_set_typing(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_register(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_update_message(json_rpc_request& request, std::string& response);
    boost::capy::task<simdjson::error_code> handle_message_reaction(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_send_message(json_rpc_request& request, std::string& response);

    boost::capy::task<simdjson::error_code> handle_search_users(json_rpc_request& request, std::string& response);

    void publish_personal_view(chat::conversation_view_event const& view);
    boost::capy::task<void> publish_presence(bool online);

    struct sent_message
    {
        std::int64_t conversation;
        bool deleted;
        bool attachment;
        bool member;
        std::string json;
    };
    // The caller's message already stored under client_id, in its current state.
    boost::capy::task<std::expected<std::optional<sent_message>, sqlite_error>> find_sent_message(std::int64_t client_id);
    enum class notification_result
    {
        skipped,
        delivered,
        denied,
        failed
    };
    boost::capy::task<notification_result> publish_conversation(std::int64_t conversation,
                                                                std::string notification,
                                                                bool require_sender_permission = false,
                                                                std::int64_t message = 0,
                                                                quoted_notification const* quote = nullptr);
    boost::capy::task<void> publish_join_request(std::int64_t conversation, std::int64_t user, std::string state);

    bool enqueue_message(std::string message);

    websocket_connection& connection_;
    online_users& users_;
    invite_attempts& invite_attempts_;
    blocking_worker& passwords_;
    database& database_;
    s3_objects& objects_;
    std::optional<std::int64_t> user_id_;
    std::int64_t login_id_ = 0;
    std::int64_t login_expires_ = 0;
    std::int64_t next_login_touch_ = 0;
    bool revoked_ = false;
    bool exchanging_request_ = false;
    std::string username_;
    std::deque<std::string> outgoing_messages_;
    std::string device_;
    struct avatar_upload
    {
        std::int64_t id;
        std::int64_t size;
        std::string content;
    };
    std::optional<avatar_upload> avatar_upload_;
};

#endif
