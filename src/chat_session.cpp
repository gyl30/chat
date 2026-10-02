#include <charconv>
#include <cstdint>
#include <string>
#include <utility>
#include <string_view>
#include <system_error>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

constexpr std::string_view kAddContactMethod = "add_contact";
constexpr std::string_view kAuthenticateMethod = "authenticate";
constexpr std::string_view kEchoMethod = "echo";
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

chat_session::chat_session(websocket_connection& connection, online_users& users, pg_connection_pool& database)
    : connection_(connection), users_(users), database_(database)
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
    if (!user_id_)
    {
        co_return;
    }

    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return;
    }

    auto const user = std::to_string(*user_id_);
    std::string last_seen = "0";
    if (!online)
    {
        auto update_result = co_await lease.connection().execute_scalar(
            "UPDATE users SET last_seen_at = CURRENT_TIMESTAMP WHERE id = $1::bigint "
            "RETURNING ((extract(epoch from last_seen_at) * 1000)::bigint)::text",
            {user});
        auto& [update_ec, value] = update_result;
        if (update_ec)
        {
            co_return;
        }
        last_seen = std::move(value);
    }

    auto watchers_result = co_await lease.connection().execute_scalar(
        "SELECT COALESCE(string_agg(peer::text, ',' ORDER BY peer), '') FROM ("
        "SELECT owner_id AS peer FROM contacts WHERE contact_id = $1::bigint "
        "UNION "
        "SELECT CASE WHEN direct_user_low=$1::bigint THEN direct_user_high ELSE direct_user_low END AS peer "
        "FROM conversations c WHERE kind='direct' AND (direct_user_low=$1::bigint OR direct_user_high=$1::bigint) "
        "AND EXISTS(SELECT 1 FROM messages WHERE conversation_id=c.id)"
        ") AS peers WHERE peer <> $1::bigint",
        {user});
    auto& [watchers_ec, watchers] = watchers_result;
    if (watchers_ec)
    {
        co_return;
    }

    std::string notification = R"({"jsonrpc":"2.0","method":"presence","params":{"user":)";
    notification.append(user);
    notification.append(R"(,"online":)");
    notification.append(online ? "true" : "false");
    notification.append(R"(,"last_seen":)");
    notification.append(last_seen);
    notification.append("}}");

    std::string_view remaining = watchers;
    while (!remaining.empty())
    {
        auto const separator = remaining.find(',');
        auto const token = remaining.substr(0, separator);
        std::int64_t watcher = 0;
        auto const [end, parse_error] = std::from_chars(token.data(), token.data() + token.size(), watcher);
        if (parse_error == std::errc{} && end == token.data() + token.size())
        {
            if (auto* session = users_.find(watcher))
            {
                session->enqueue_message(notification);
            }
        }

        if (separator == std::string_view::npos)
        {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
}

boost::capy::task<bool> chat_session::publish_conversation(std::int64_t conversation, std::string notification,
                                                        bool require_sender_membership)
{
    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return false;
    }
    auto& connection = lease.connection();
    // 取得锁后查询当前成员，并在释放锁前入队，避免移除后仍使用旧收件人快照。
    auto begun = co_await connection.execute_row("BEGIN");
    auto locked = co_await connection.execute_row(
        "SELECT id::text FROM conversations WHERE id=$1::bigint FOR UPDATE", {std::to_string(conversation)});
    if (std::get<0>(begun) || std::get<0>(locked))
    {
        connection.close();
        co_return false;
    }
    auto query_result = co_await lease.connection().execute_scalar(
        "SELECT COALESCE(string_agg(m.user_id::text, ',' ORDER BY m.user_id),'') "
        "FROM conversation_members m JOIN conversations c ON c.id=m.conversation_id "
        "WHERE m.conversation_id=$1::bigint AND (m.user_id<>$2::bigint OR "
        "(c.kind='direct' AND c.direct_user_low=c.direct_user_high)) "
        "AND (NOT $3::boolean OR EXISTS(SELECT 1 FROM conversation_members "
        "WHERE conversation_id=$1::bigint AND user_id=$2::bigint))",
        {std::to_string(conversation), std::to_string(*user_id_), require_sender_membership ? "true" : "false"});
    auto& [ec, recipients] = query_result;
    if (ec)
    {
        connection.close();
        co_return false;
    }
    bool realtime = false;
    std::string_view remaining = recipients;
    while (!remaining.empty())
    {
        auto const separator = remaining.find(',');
        auto const token = remaining.substr(0, separator);
        std::int64_t recipient = 0;
        auto const [end, parse_error] = std::from_chars(token.data(), token.data() + token.size(), recipient);
        if (parse_error == std::errc{} && end == token.data() + token.size())
        {
            if (auto* session = users_.find(recipient))
            {
                realtime = session->enqueue_message(notification) || realtime;
            }
        }
        if (separator == std::string_view::npos)
        {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    auto committed = co_await connection.execute_row("COMMIT");
    if (std::get<0>(committed))
    {
        connection.close();
    }
    co_return realtime;
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
            if (request.method == kAddContactMethod || request.method == "remove_contact")
            {
                rpc_error = co_await handle_contact_change(request, response);
            }
            else if (request.method == kAuthenticateMethod)
            {
                rpc_error = co_await handle_authenticate(request, response);
            }
            else if (request.method == "begin_avatar_upload" || request.method == "upload_avatar_chunk" ||
                     request.method == "finish_avatar_upload" || request.method == "cancel_avatar_upload" ||
                     request.method == "get_avatar" || request.method == "clear_avatar")
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
            else if (request.method == kEchoMethod)
            {
                rpc_error = co_await handle_echo(request, response);
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
            else if (request.method == "set_group_admin" || request.method == "rename_group" ||
                     request.method == "invite_group_members" || request.method == "leave_group" ||
                     request.method == "transfer_group_owner" || request.method == "remove_group_member" ||
                     request.method == "pin_group_message" || request.method == "unpin_group_message" ||
                     request.method == "set_group_announcement")
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
            else if (request.method == "begin_attachment" || request.method == "upload_attachment" ||
                     request.method == "cancel_attachment" || request.method == "get_attachment")
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
