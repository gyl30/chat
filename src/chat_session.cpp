#include <cstdint>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <utility>
#include <string_view>
#include <system_error>

#include "chat_session.hpp"
#include "invite_attempts.hpp"
#include "database.hpp"
#include "conversation_store.hpp"
#include "user_store.hpp"

namespace
{

constexpr std::string_view kAuthenticateMethod = "authenticate";
constexpr std::string_view kGetContactsMethod = "get_contacts";
constexpr std::string_view kGetConversationsMethod = "get_conversations";
constexpr std::string_view kGetMessagesMethod = "get_messages";
constexpr std::string_view kGetPresenceMethod = "get_presence";
constexpr std::string_view kGetUnreadCountMethod = "get_unread_count";
constexpr std::string_view kMarkReadMethod = "mark_read";
constexpr std::string_view kRegisterMethod = "register";
constexpr std::string_view kSendMessageMethod = "send_message";
constexpr std::string_view kSearchUsersMethod = "search_users";
constexpr std::size_t kMaxQueuedMessages = 64;

}    // namespace

chat_session::chat_session(websocket_connection& connection,
                           online_users& users,
                           invite_attempts& invite_attempts,
                           blocking_worker& passwords,
                           database& storage,
                           s3_objects& objects)
    : connection_(connection), users_(users), invite_attempts_(invite_attempts), passwords_(passwords), database_(storage), objects_(objects)
{
}

bool chat_session::enqueue_message(std::string message)
{
    if (outgoing_messages_.size() >= kMaxQueuedMessages)
    {
        connection_.close();
        return false;
    }

    outgoing_messages_.push_back(std::move(message));
    connection_.interrupt_receive();
    return true;
}

boost::capy::task<void> chat_session::publish_presence(bool online)
{
    auto const user = *user_id_;
    auto presence = co_await database_.run([=](sqlite_database& db) { return record_presence(db, user, online); });
    if (!presence)
    {
        spdlog::warn("presence update failed (SQLite {})", presence.error().code);
        co_return;
    }
    // Authentication can replace the session while the offline job is pending.
    if (!online && users_.find(user))
    {
        co_return;
    }
    auto notification = R"({"jsonrpc":"2.0","method":"presence","params":{"user":)" + std::to_string(user) + R"(,"online":)" +
                        (online ? "true" : "false") + R"(,"last_seen":)" + std::to_string(presence->last_seen) + "}}";
    for (auto const watcher : presence->recipients)
    {
        if (auto* session = users_.find(watcher))
        {
            session->enqueue_message(notification);
        }
    }
}

boost::capy::task<chat_session::notification_result> chat_session::publish_conversation(std::int64_t conversation,
                                                                                        std::string notification,
                                                                                        bool require_sender_permission)
{
    auto recipients = co_await database_.run(
        [=, user = *user_id_](sqlite_database& db) -> std::expected<std::optional<std::vector<std::int64_t>>, sqlite_error>
        {
            auto snapshot = sqlite_transaction::begin_read(db);
            if (!snapshot)
            {
                return std::unexpected(std::move(snapshot.error()));
            }
            if (require_sender_permission)
            {
                auto allowed = can_send_to_conversation(db, conversation, user);
                if (!allowed)
                {
                    return std::unexpected(std::move(allowed.error()));
                }
                if (!*allowed)
                {
                    return std::nullopt;
                }
            }
            auto rows = db.prepare(R"SQL(
            SELECT m.user_id FROM conversation_members m JOIN conversations c ON c.id=m.conversation_id
            WHERE c.id=?1 AND (m.user_id<>?2 OR c.direct_user_low=c.direct_user_high)
            ORDER BY m.user_id
        )SQL",
                                   conversation,
                                   user);
            if (!rows)
            {
                return std::unexpected(std::move(rows.error()));
            }
            std::vector<std::int64_t> result;
            for (;;)
            {
                auto row = rows->step();
                if (!row)
                {
                    return std::unexpected(std::move(row.error()));
                }
                if (!*row)
                {
                    break;
                }
                result.push_back(rows->column_int64(0));
            }
            if (auto error = snapshot->commit(); error)
            {
                return std::unexpected(std::move(error));
            }
            return result;
        });
    if (!recipients)
    {
        spdlog::warn("conversation notification lookup failed (SQLite {})", recipients.error().code);
        co_return notification_result::failed;
    }
    if (!*recipients)
    {
        co_return notification_result::denied;
    }
    bool realtime = false;
    for (auto const recipient : **recipients)
    {
        if (auto* session = users_.find(recipient))
        {
            realtime = session->enqueue_message(notification) || realtime;
        }
    }
    co_return realtime ? notification_result::delivered : notification_result::skipped;
}

boost::capy::task<void> chat_session::run()
{
    std::string response;
    bool running = true;

    while (running)
    {
        while (!outgoing_messages_.empty())
        {
            auto message = std::move(outgoing_messages_.front());
            outgoing_messages_.pop_front();

            auto [send_ec] = co_await connection_.send_text(message);
            if (send_ec)
            {
                running = false;
                break;
            }
        }
        if (!running)
        {
            break;
        }

        auto receive_result = co_await connection_.receive();
        auto& [ec, message] = receive_result;
        if (ec == std::errc::interrupted)
        {
            continue;
        }
        if (ec || message.message_type == websocket_message::type::close)
        {
            break;
        }

        json_rpc_request request;
        auto rpc_error = parse_json_rpc_request(message.payload, request, response);
        if (rpc_error)
        {
            break;
        }

        if (response.empty())
        {
            if (request.method == "send_friend_request" || request.method == "respond_friend_request" || request.method == "cancel_friend_request" ||
                request.method == "get_friend_requests" || request.method == "remove_contact")
            {
                rpc_error = co_await handle_contact_change(request, response);
            }
            else if (request.method == kAuthenticateMethod)
            {
                rpc_error = co_await handle_authenticate(request, response);
            }
            else if (request.method == "begin_avatar_upload" || request.method == "upload_avatar_chunk" || request.method == "finish_avatar_upload" ||
                     request.method == "cancel_avatar_upload" || request.method == "get_avatar" || request.method == "clear_avatar")
            {
                rpc_error = co_await handle_avatar(request, response);
            }
            else if (request.method == "edit_message" || request.method == "delete_message")
            {
                rpc_error = co_await handle_update_message(request, response);
            }
            else if (request.method == "set_message_reaction")
            {
                rpc_error = co_await handle_message_reaction(request, response);
            }
            else if (request.method == "set_conversation_muted" || request.method == "set_conversation_pinned")
            {
                rpc_error = co_await handle_conversation_preference(request, response);
            }
            else if (request.method == kGetContactsMethod)
            {
                rpc_error = co_await handle_get_contacts(request, response);
            }
            else if (request.method == "open_direct_conversation" || request.method == "create_group")
            {
                rpc_error = co_await handle_create_conversation(request, response);
            }
            else if (request.method == "get_members")
            {
                rpc_error = co_await handle_get_members(request, response);
            }
            else if (request.method == "get_group_invite" || request.method == "create_group_invite" || request.method == "revoke_group_invite" ||
                     request.method == "join_group")
            {
                rpc_error = co_await handle_group_invite(request, response);
            }
            else if (request.method == "set_group_join_approval" || request.method == "get_group_join_requests" ||
                     request.method == "respond_group_join_request")
            {
                rpc_error = co_await handle_group_join_request(request, response);
            }
            else if (request.method == "set_group_admin" || request.method == "rename_group" || request.method == "invite_group_members" ||
                     request.method == "leave_group" || request.method == "transfer_group_owner" || request.method == "remove_group_member" ||
                     request.method == "pin_group_message" || request.method == "unpin_group_message" || request.method == "set_group_announcement")
            {
                rpc_error = co_await handle_group_management(request, response);
            }
            else if (request.method == kGetConversationsMethod)
            {
                rpc_error = co_await handle_get_conversations(request, response);
            }
            else if (request.method == kGetMessagesMethod || request.method == "search_messages")
            {
                rpc_error = co_await handle_get_messages(request, response);
            }
            else if (request.method == kGetPresenceMethod)
            {
                rpc_error = co_await handle_get_presence(request, response);
            }
            else if (request.method == kGetUnreadCountMethod)
            {
                rpc_error = co_await handle_get_unread_count(request, response);
            }
            else if (request.method == kMarkReadMethod)
            {
                rpc_error = co_await handle_mark_read(request, response);
            }
            else if (request.method == "set_typing")
            {
                rpc_error = co_await handle_set_typing(request, response);
            }
            else if (request.method == kRegisterMethod)
            {
                rpc_error = co_await handle_register(request, response);
            }
            else if (request.method == "begin_attachment" || request.method == "upload_attachment" || request.method == "cancel_attachment" ||
                     request.method == "get_attachment")
            {
                rpc_error = co_await handle_attachment(request, response);
            }
            else if (request.method == kSendMessageMethod || request.method == "finish_attachment")
            {
                rpc_error = co_await handle_send_message(request, response);
            }
            else if (request.method == kSearchUsersMethod)
            {
                rpc_error = co_await handle_search_users(request, response);
            }
            else if (request.id.present)
            {
                rpc_error = serialize_json_rpc_method_not_found(std::move(request.id), response);
            }
        }

        if (rpc_error)
        {
            break;
        }
        if (response.empty())
        {
            continue;
        }

        auto [send_ec] = co_await connection_.send_text(response);
        if (send_ec)
        {
            break;
        }
    }

    if (user_id_)
    {
        users_.remove(*user_id_, *this);
        co_await publish_presence(false);
    }
}
