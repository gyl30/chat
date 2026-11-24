#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <utility>
#include <string_view>
#include <system_error>

#include "chat_session.hpp"
#include "message_payload.hpp"
#include "invite_attempts.hpp"
#include "database.hpp"
#include "conversation_store.hpp"
#include "user_store.hpp"
#include "timestamp.hpp"

namespace
{

constexpr std::string_view kAuthenticateMethod = "authenticate";
constexpr std::string_view kGetContactsMethod = "get_contacts";
constexpr std::string_view kGetConversationsMethod = "get_conversations";
constexpr std::string_view kGetMessagesMethod = "get_messages";
constexpr std::string_view kGetPresenceMethod = "get_presence";
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
    if (login_id_ && !revoked_ && epoch_milliseconds() >= login_expires_)
    {
        invalidate_login();
    }
    if (revoked_)
    {
        return false;
    }
    if (outgoing_messages_.size() >= kMaxQueuedMessages)
    {
        connection_.close();
        return false;
    }

    outgoing_messages_.push_back(std::move(message));
    connection_.interrupt_receive();
    return true;
}

void chat_session::invalidate_login()
{
    revoked_ = true;
    outgoing_messages_.clear();
    connection_.interrupt_receive();
}

void chat_session::publish_login_changes(std::span<std::int64_t const> revoked)
{
    for (auto* connection : users_.sessions(*user_id_))
    {
        if (std::ranges::find(revoked, connection->login_id_) != revoked.end())
        {
            connection->invalidate_login();
            // An in-flight operation can be blocked on object storage. Do not make
            // socket teardown wait for it. The database gate protects its continuation.
            if (connection != this && connection->exchanging_request_)
            {
                connection->connection_.close();
            }
        }
        else if (connection != this)
        {
            connection->enqueue_message(R"({"jsonrpc":"2.0","method":"login_sessions_changed","params":{}})");
        }
    }
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
    if (!online && !users_.sessions(user).empty())
    {
        co_return;
    }
    auto notification = R"({"jsonrpc":"2.0","method":"presence","params":{"user":)" + std::to_string(user) + R"(,"online":)" +
                        (online ? "true" : "false") + R"(,"last_seen":)" + std::to_string(presence->last_seen) + "}}";
    for (auto const watcher : presence->recipients)
    {
        for (auto* session : users_.sessions(watcher))
        {
            session->enqueue_message(notification);
        }
    }
}

boost::capy::task<chat_session::notification_result> chat_session::publish_conversation(
    std::int64_t conversation, std::string notification, bool require_sender_permission, std::int64_t message, quoted_notification const* quote)
{
    using audience = std::vector<std::pair<std::int64_t, bool>>;
    auto recipients = co_await database_.run(
        [=, user = *user_id_, login = login_id_](sqlite_database& db) -> std::expected<std::optional<audience>, sqlite_error>
        {
            auto snapshot = sqlite_transaction::begin_read(db);
            if (!snapshot)
            {
                return std::unexpected(std::move(snapshot.error()));
            }
            if (require_sender_permission)
            {
                auto active = login_active(db, login, epoch_milliseconds());
                if (!active)
                {
                    return std::unexpected(active.error());
                }
                if (!*active)
                {
                    return std::nullopt;
                }
                auto allowed = can_send_to_conversation(db, conversation, user);
                if (!allowed)
                {
                    return std::unexpected(std::move(allowed.error()));
                }
                if (*allowed != send_access::allowed)
                {
                    return std::nullopt;
                }
            }
            auto rows = db.prepare(R"SQL(
            SELECT user_id, ?4>0 AND ?4<=visible_after FROM conversation_members
            WHERE conversation_id=?1 AND (?2=0 OR user_id<>?2) AND (?3=0 OR ?3>visible_after)
            ORDER BY user_id
        )SQL",
                                   conversation,
                                   require_sender_permission ? user : 0,
                                   message,
                                   quote ? quote->id : 0);
            if (!rows)
            {
                return std::unexpected(std::move(rows.error()));
            }
            audience result;
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
                result.emplace_back(rows->column_int64(0), rows->column_int64(1) != 0);
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
    if (!*recipients || (require_sender_permission && revoked_))
    {
        co_return notification_result::denied;
    }
    bool realtime = false;
    for (auto const& [recipient, omit_quote] : **recipients)
    {
        for (auto* session : users_.sessions(recipient))
        {
            if (session == this)
            {
                continue;
            }
            realtime = session->enqueue_message(omit_quote ? quote->without_quote : notification) || realtime;
        }
    }
    co_return realtime ? notification_result::delivered : notification_result::skipped;
}

boost::capy::task<void> chat_session::run()
{
    co_await exchange_messages();
    if (user_id_)
    {
        if (!device_.empty() &&
            std::ranges::none_of(users_.sessions(*user_id_), [this](auto* other) { return other != this && other->device_ == device_; }))
        {
            auto paused = co_await database_.run(
                [this](sqlite_database& db) -> std::expected<void, sqlite_error>
                {
                    if (auto err = db.exec("UPDATE attachment_uploads SET state='paused',touched=? WHERE user_id=? AND device=? AND state='active'",
                                           epoch_milliseconds(),
                                           *user_id_,
                                           device_);
                        err)
                    {
                        return std::unexpected(err);
                    }
                    return {};
                });
            if (!paused)
            {
                spdlog::warn("could not pause disconnected uploads (SQLite {})", paused.error().code);
            }
        }
        users_.remove(*user_id_, *this);
        if (users_.sessions(*user_id_).empty())
        {
            co_await publish_presence(false);
        }
    }
}

boost::capy::task<void> chat_session::exchange_messages()
{
    std::string response;
    for (;;)
    {
        if (revoked_)
        {
            co_await connection_.send_text(R"({"jsonrpc":"2.0","method":"login_session_revoked","params":{}})");
            connection_.close();
            co_return;
        }
        while (!outgoing_messages_.empty() && !revoked_)
        {
            auto message = std::move(outgoing_messages_.front());
            outgoing_messages_.pop_front();

            auto [send_ec] = co_await connection_.send_text(message);
            if (send_ec)
            {
                co_return;
            }
        }

        if (revoked_)
        {
            continue;
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
            exchanging_request_ = true;
            rpc_error = co_await dispatch_request(request, response);
            exchanging_request_ = false;
        }

        if (revoked_ && request.method != "logout_session")
        {
            response.clear();
            continue;
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
}

boost::capy::task<simdjson::error_code> chat_session::dispatch_request(json_rpc_request& request, std::string& response)
{
    if (login_id_)
    {
        auto const now = epoch_milliseconds();
        if (revoked_ || now >= login_expires_)
        {
            invalidate_login();
            co_return simdjson::SUCCESS;
        }
        if (now >= next_login_touch_)
        {
            auto activity = co_await run_db([id = login_id_, now](sqlite_database& db) { return touch_login(db, id, now); });
            if (!activity || revoked_)
            {
                invalidate_login();
                co_return simdjson::SUCCESS;
            }
            next_login_touch_ = now + kLoginActivityIntervalMs;
        }
    }
    if (request.method == "list_login_sessions" || request.method == "revoke_login_session" || request.method == "revoke_other_login_sessions" ||
        request.method == "logout_session" || request.method == "change_password")
    {
        co_return co_await handle_login_management(request, response);
    }
    if (request.method == "send_friend_request" || request.method == "respond_friend_request" || request.method == "cancel_friend_request" ||
        request.method == "get_friend_requests" || request.method == "remove_contact")
    {
        co_return co_await handle_contact_change(request, response);
    }
    if (request.method == kAuthenticateMethod || request.method == "resume_session")
    {
        co_return co_await handle_authenticate(request, response);
    }
    if (request.method == "begin_avatar_upload" || request.method == "upload_avatar_chunk" || request.method == "finish_avatar_upload" ||
        request.method == "cancel_avatar_upload" || request.method == "get_avatar" || request.method == "clear_avatar")
    {
        co_return co_await handle_avatar(request, response);
    }
    if (request.method == "edit_message" || request.method == "delete_message")
    {
        co_return co_await handle_update_message(request, response);
    }
    if (request.method == "set_message_reaction")
    {
        co_return co_await handle_message_reaction(request, response);
    }
    if (request.method == "mark_conversations_read")
    {
        co_return co_await handle_bulk_read(request, response);
    }
    if (request.method == "get_user_profile" || request.method == "set_user_profile")
    {
        co_return co_await handle_user_profile(request, response);
    }
    if (request.method == "set_notification_level" || request.method == "set_unread_mark" || request.method == "set_conversation_pinned" ||
        request.method == "clear_history" || request.method == "hide_conversation")
    {
        co_return co_await handle_conversation_preference(request, response);
    }
    if (request.method == "create_conversation_folder" || request.method == "rename_conversation_folder" ||
        request.method == "delete_conversation_folder" || request.method == "reorder_conversation_folder" ||
        request.method == "move_conversation_to_folder")
    {
        co_return co_await handle_conversation_folder(request, response);
    }
    if (request.method == "set_friend_remark" || request.method == "move_friend" || request.method == "create_friend_group" ||
        request.method == "rename_friend_group" || request.method == "delete_friend_group")
    {
        co_return co_await handle_contact_settings(request, response);
    }
    if (request.method == kGetContactsMethod)
    {
        co_return co_await handle_get_contacts(request, response);
    }
    if (request.method == "open_direct_conversation" || request.method == "create_group")
    {
        co_return co_await handle_create_conversation(request, response);
    }
    if (request.method == "get_members")
    {
        co_return co_await handle_get_members(request, response);
    }
    if (request.method == "get_group_invite" || request.method == "create_group_invite" || request.method == "revoke_group_invite" ||
        request.method == "join_group")
    {
        co_return co_await handle_group_invite(request, response);
    }
    if (request.method == "set_group_join_approval" || request.method == "get_group_join_requests" || request.method == "respond_group_join_request")
    {
        co_return co_await handle_group_join_request(request, response);
    }
    if (request.method == "set_group_member_mute" || request.method == "set_group_mute" || request.method == "dissolve_group" ||
        request.method == "set_group_admin" || request.method == "rename_group" || request.method == "invite_group_members" ||
        request.method == "leave_group" || request.method == "transfer_group_owner" || request.method == "remove_group_member" ||
        request.method == "pin_group_message" || request.method == "unpin_group_message" || request.method == "set_group_announcement" ||
        request.method == "set_group_description")
    {
        co_return co_await handle_group_management(request, response);
    }
    if (request.method == kGetConversationsMethod)
    {
        co_return co_await handle_get_conversations(request, response);
    }
    if (request.method == "set_message_bookmark" || request.method == "get_message_bookmark" || request.method == "list_message_bookmarks")
    {
        co_return co_await handle_message_bookmarks(request, response);
    }
    if (request.method == "get_poll" || request.method == "vote_poll" || request.method == "close_poll" || request.method == "get_poll_voters")
    {
        co_return co_await handle_poll(request, response);
    }
    if (request.method == "get_forwarded_messages")
    {
        co_return co_await handle_bundle(request, response);
    }
    if (request.method == kGetMessagesMethod || request.method == "search_messages" || request.method == "search_all_messages" ||
        request.method == "message_search_scopes")
    {
        co_return co_await handle_get_messages(request, response);
    }
    if (request.method == kGetPresenceMethod)
    {
        co_return co_await handle_get_presence(request, response);
    }
    if (request.method == kMarkReadMethod)
    {
        co_return co_await handle_mark_read(request, response);
    }
    if (request.method == "set_typing")
    {
        co_return co_await handle_set_typing(request, response);
    }
    if (request.method == kRegisterMethod)
    {
        co_return co_await handle_register(request, response);
    }
    if (request.method == "add_conversation_file" || request.method == "remove_conversation_file" || request.method == "list_conversation_files")
    {
        co_return co_await handle_conversation_file(request, response);
    }
    if (request.method == "complete_file" || request.method == "get_conversation_file" || request.method == "begin_attachment" ||
        request.method == "confirm_attachment" || request.method == "pause_attachment" || request.method == "resume_attachment" ||
        request.method == "query_attachment" || request.method == "cancel_attachment" || request.method == "get_attachment")
    {
        co_return co_await handle_attachment(request, response);
    }
    if (request.method == kSendMessageMethod || request.method == "finish_attachment")
    {
        co_return co_await handle_send_message(request, response);
    }
    if (request.method == kSearchUsersMethod)
    {
        co_return co_await handle_search_users(request, response);
    }
    if (request.id.present)
    {
        co_return serialize_json_rpc_method_not_found(std::move(request.id), response);
    }
    co_return simdjson::SUCCESS;
}
