#ifndef CHAT_QT_SRC_CLIENT_BRIDGE_HPP
#define CHAT_QT_SRC_CLIENT_BRIDGE_HPP

#include <memory>
#include <atomic>
#include <optional>
#include <cstdint>
#include <expected>
#include <functional>
#include <utility>
#include <chat/client.hpp>
#include <chat/conversation.hpp>
#include <chat/error.hpp>
#include <chat/friendship.hpp>
#include <chat/message.hpp>

#include <QObject>
#include <QString>
#include <QtGlobal>
#include <QByteArray>

#include "conversation_data.hpp"
#include "avatar.hpp"
#include "message_data.hpp"
#include "presence_data.hpp"
#include "user_data.hpp"
#include "member_data.hpp"

namespace chat
{
class client;
struct pending_text;
}    // namespace chat

class client_bridge final : public QObject
{
    Q_OBJECT

   public:
    explicit client_bridge(QObject* parent = nullptr);
    ~client_bridge() override;

    void connect_to_server(QString const& url);
    void close(bool keep_local_account = false);
    void authenticate(QString const& username, QString const& password);
    quint64 get_login_sessions();
    void revoke_login_session(qint64 id);
    void revoke_other_login_sessions();
    void logout_session();
    void change_password(QString const& current, QString const& replacement);
    void register_user(QString const& username, QString const& password);
    void get_conversations();
    void clear_history(qint64 conversation, qint64 through);
    void change_history_view(qint64 conversation, bool hide);
    void set_notification_level(qint64 conversation, chat::notification_level level, qint64 until);
    void create_conversation_folder(QString name);
    void rename_conversation_folder(qint64 folder, QString name);
    void delete_conversation_folder(qint64 folder);
    void reorder_conversation_folder(qint64 folder, bool up);
    void move_conversation_to_folder(qint64 conversation, qint64 folder);
    void mark_read_scope(qint64 conversation, qint64 folder);
    void set_unread_mark(qint64 conversation, bool marked);
    void set_conversation_pinned(qint64 conversation, bool pinned);
    void open_direct_conversation(qint64 user, QString username);
    void create_group(QString title, QList<qint64> members);
    void join_group(QString token);
    void group_invite(qint64 conversation, std::optional<bool> create = {});
    void set_group_join_approval(qint64 conversation, bool required);
    void get_group_join_requests(qint64 conversation, qint64 before = 0);
    void respond_group_join_request(qint64 conversation, qint64 user, bool accept);
    void get_members(qint64 conversation);
    void set_group_admin(qint64 conversation, qint64 user, bool admin);
    void transfer_group_owner(qint64 conversation, qint64 user);
    void set_group_mute(qint64 conversation, bool muted);
    void set_group_member_mute(qint64 conversation, qint64 user, qint64 seconds);
    void dissolve_group(qint64 conversation);
    void remove_group_member(qint64 conversation, qint64 user);
    void rename_group(qint64 conversation, QString title);
    void set_group_avatar(qint64 group, QByteArray data);
    void set_group_description(qint64 conversation, QString text);
    void set_group_announcement(qint64 conversation, QString text);
    void invite_group_members(qint64 conversation, QList<qint64> members);
    void leave_group(qint64 conversation);
    void set_group_pinned_message(qint64 conversation, std::optional<qint64> message);
    void get_contacts();
    void set_friend_remark(qint64 user, QString remark);
    void move_friend(qint64 user, QString group);
    void create_friend_group(QString name);
    void rename_friend_group(QString group, QString name);
    void delete_friend_group(QString group);
    void get_friend_requests();
    void respond_friend_request(qint64 user, bool accept);
    void cancel_friend_request(qint64 user);
    std::function<QString(qint64)> friend_remark = [](qint64) { return QString{}; };
    void get_user_profile(qint64 user);
    void set_user_profile(QString nickname, QString signature);
    void get_presence();
    void get_messages(qint64 conversation, std::optional<qint64> before = {}, std::optional<qint64> after = {}, bool capture_unread = false);
    void create_poll(qint64 room, QString question, QStringList options);
    void refresh_live_poll(qint64 room, qint64 message);
    void get_poll(qint64 room, qint64 message);
    void vote_poll(qint64 room, qint64 message, qint64 choice);
    void close_poll(qint64 room, qint64 message);
    void get_poll_voters(qint64 room, qint64 message, qint64 choice, qint64 after);
    void get_bundle(qint64 conversation, qint64 message, qint64 after);
    void forward_bundle(qint64 target, QList<qint64> messages);
    void forward_messages(qint64 target, QList<qint64> messages);
    void send_message(qint64 conversation, QString text, qint64 reply_to, qint64 client_id, qint64 revision, chat::text_options options);
    void save_draft(qint64 user, qint64 conversation, qint64 revision, qint64 reply, QString text, chat::text_options options);
    void restore_drafts(qint64 user);
    void validate_draft_reply(qint64 conversation, qint64 anchor, qint64 revision);
    void restore_texts();
    void change_pending_text(qint64 client_id, bool discard);
    void delete_message(qint64 conversation, qint64 message);
    void edit_message(qint64 conversation, qint64 message, QString text);
    void set_message_reaction(qint64 conversation, qint64 message, QString emoji);
    void search_users(QString query, quint64 request);
    quint64 get_message_bookmark(qint64 message);
    void set_message_bookmark(qint64 message, bool bookmarked);
    quint64 list_message_bookmarks(QString const& query, qint64 before);
    quint64 search_all_messages(chat::message_search_filter const& filter, qint64 before);
    quint64 get_message_search_scopes();
    quint64 find_search_senders(QString query);
    quint64 get_search_context(qint64 conversation, qint64 anchor);
    void search_messages(qint64 conversation, QString query, qint64 before = 0);
    // Browsing and searching the history window share one request sequence.
    void get_history(qint64 conversation, chat::message_category category, QString query, qint64 before);
    void add_contact(qint64 user);
    void remove_contact(qint64 user);
    void send_attachment(qint64 conversation, QString path, qint64 reply_to, qint64 client_id, bool group_only);
    void change_file(qint64 conversation, QString key, bool add);
    void list_files(qint64 conversation, QString search, QString category, qint64 before, QString key);
    void download_file(qint64 conversation, QString key, QString filename, QString path, bool open);
    void get_attachment(qint64 conversation, qint64 message, QString filename, QString target, qint64 item = 0);
    void start_download(qint64 id,
                        quint64 attempt,
                        qint64 conversation,
                        qint64 message,
                        QString key,
                        QString path,
                        std::shared_ptr<chat::download_control> control,
                        bool replace,
                        qint64 item = 0);
    void restore_attachments();
    void interrupt_attachment(qint64 id, bool cancel, QString path);
    void get_message_image(qint64 conversation, qint64 message, qint64 item = 0);
    void mark_read(qint64 user, qint64 message);
    void get_avatar(qint64 user, qint64 revision);
    void set_avatar(QByteArray data);
    void clear_avatar();
    void set_typing(qint64 conversation, bool typing);

   signals:
    void avatar_changed(qint64 user, chat::avatar_state state);
    void avatar_received(qint64 user, chat::avatar_state state, QByteArray data);
    void avatar_update_finished(QString error_message);
    void connected();
    void disconnected();
    void error(QString message);
    void authentication_finished(bool authenticated, qint64 user, QString error_message, bool retryable_error, chat::avatar_state avatar);
    void registration_finished(QString error_message);
    void login_revoked();
    void login_sessions_changed();
    void login_sessions_received(quint64 request, std::vector<chat::login_session> sessions, QString error_message);
    void login_management_finished(QString message, bool success);
    void logout_finished(QString error_message);
    void conversation_folders_received(conversation_folder_list folders);
    void conversations_received(QList<conversation_data> conversations, QString error_message);
    void pin_finished(qint64 conversation, bool pinned, QString error_message);
    void friendship_changed();
    void friend_requests_received(QList<user_data> incoming, QList<user_data> outgoing, QString error_message);
    void friend_groups_received(QStringList groups);
    void profile_changed(qint64 user);
    void profile_received(qint64 user, QString username, QString nickname, QString signature);
    void contacts_received(QList<user_data> contacts, QString error_message);
    void presences_received(QList<presence_data> users, QString error_message);
    void presence_changed(presence_data user);
    void messages_received(qint64 conversation,
                           QList<message_data> messages,
                           read_positions positions,
                           bool older,
                           bool recovering,
                           bool has_more,
                           std::optional<qint64> first_unread,
                           QString error_message);
    void message_received(message_data message);
    void message_updated(qint64 conversation, message_data message, QString error_message);
    void reaction_changed(qint64 conversation, qint64 message, qint64 revision, QList<reaction_data> reactions, QString error_message);
    void messages_read(qint64 conversation, qint64 user, qint64 message);
    void typing_changed(qint64 conversation, qint64 user, QString username, bool typing);
    void conversation_opened(conversation_data conversation, QString error_message);
    void members_received(qint64 conversation, QList<member_data> members, QString error_message);
    void group_action_finished(qint64 conversation, bool left, QString error_message);
    void group_invite_received(qint64 conversation, QString token, QString error_message);
    void group_join_pending(QString title);
    void group_join_request_changed(qint64 conversation, qint64 user, chat::group_join_request_state state);
    void group_join_requests_received(qint64 conversation, QList<user_data> users, qint64 next, bool older, QString error_message);
    void group_pin_finished(qint64 conversation, QString error_message);
    void notifications_changed(qint64 conversation, chat::notification_level level, qint64 until);
    void personal_view_changed(qint64 conversation, QString action, qint64 through);
    void clear_boundary_ready(qint64 conversation, qint64 through);
    void history_boundary(qint64 conversation, qint64 floor, bool hidden);
    void conversation_changed(qint64 conversation, bool removed);
    // A text or attachment send finished: the stored message, or why it failed. lost means the
    // connection dropped before an answer; unavailable means the server denied conversation access.
    void draft_conversations_unavailable(QList<qint64> conversations);
    void drafts_restored(qint64 user, std::vector<chat::compose_draft> drafts);
    void draft_reply_validated(qint64 conversation, qint64 anchor, qint64 revision, message_data message, bool unavailable);
    void text_saved(qint64 client_id, QString error);
    void pending_text_changed(qint64 conversation,
                              qint64 client_id,
                              QString text,
                              qint64 reply,
                              qint64 created,
                              bool sending,
                              QString rejection,
                              bool unavailable,
                              chat::text_options options);
    void pending_text_discarded(qint64 client_id);
    void pending_texts_restored(QList<qint64> client_ids);
    void send_finished(qint64 conversation, qint64 client_id, message_data message, QString error_message, send_status status);
    void users_received(QList<user_data> users, QString error_message, quint64 request);
    void friend_request_finished(qint64 user, QString error_message);
    void contact_removed(QString error_message);
    void attachment_progress(qint64 id, qint64 bytes, qint64 total);
    void attachment_restored(qint64 conversation, qint64 id, QString path, qint64 size, qint64 reply, qint64 confirmed, bool group_only);
    void group_file_finished(qint64 id, QString error, bool unavailable);
    void files_changed(qint64 conversation, QString error);
    void files_received(qint64 conversation, chat::conversation_file_page page, bool older, QString error);
    void poll_changed(chat::poll_update update);
    void poll_received(qint64 room, qint64 message, chat::poll_state state, QString error);
    void voters_received(qint64 message, qint64 choice, qint64 after, chat::poll_voters page, QString error);
    void bundle_received(qint64 conversation, qint64 message, chat::forwarded_page page, QString error);
    void bundle_image_received(qint64 message, qint64 item, QByteArray bytes, QString error);
    void download_requested(qint64 conversation, qint64 message, QString key, QString filename, QString path, bool open, qint64 item = 0);
    void download_progress(qint64 id, quint64 attempt, qint64 bytes, qint64 total);
    void download_finished(qint64 id, quint64 attempt, QString error);
    void file_downloaded(qint64 conversation, QString path, bool open, QString error);
    void attachment_cancelled(qint64 id);
    void attachment_received(qint64 conversation, qint64 message, QString path, QString error_message);
    void message_image_received(qint64 conversation, qint64 message, QByteArray data, QString error_message);
    void message_search_scopes_received(quint64 request, std::vector<chat::message_search_scope> scopes, QString error);
    void bookmark_changed(qint64 message);
    void bookmark_state_received(quint64 request, bool bookmarked, QString error);
    void bookmark_saved(qint64 message, bool bookmarked, QString error);
    void bookmarks_received(quint64 request, chat::bookmark_page page, QString error);
    void global_search_received(quint64 request, chat::message_search_page page, QString error);
    void search_context_received(quint64 request, qint64 anchor, QList<message_data> messages, QString error, bool unavailable);
    void history_received(qint64 conversation, QList<message_data> messages, bool has_more, QString error_message);
    void message_search_received(qint64 conversation,
                                 QString query,
                                 qint64 before,
                                 QList<message_data> messages,
                                 read_positions positions,
                                 bool has_more,
                                 QString error_message);
    void read_marked(qint64 user, qint64 message, QString error_message);

   private:
    chat::client::poll_handler poll_result(qint64 room, qint64 message, bool write);
    std::function<void(std::expected<bool, chat::error>)> settings_result();
    void publish_pending_text(chat::pending_text const& item);
    std::function<void(std::expected<chat::friendship_result, chat::error>)> friend_request_result(qint64 user);
    std::function<void(std::expected<bool, chat::error>)> group_action_result(qint64 conversation, bool leaving = false);
    std::function<void(std::expected<chat::message, chat::error>)> send_result(qint64 conversation, qint64 client_id, QString path);
    chat::client::group_action_handler login_management_result(QString success);
    quint64 login_sessions_request_ = 0;

    template <class F>
    void post_result(std::uint64_t generation, F result)
    {
        QMetaObject::invokeMethod(
            this,
            [this, generation, result = std::move(result)]() mutable
            {
                if (generation == connection_generation_)
                {
                    result();
                }
            },
            Qt::QueuedConnection);
    }
    QList<message_data> to_messages(std::vector<chat::message> const& values);
    message_data to_message_data(chat::message const& value);
    quoted_message_data to_reply_data(std::optional<chat::quoted_message> const& reply);
    void get_conversations_page(std::optional<chat::conversation_cursor> before,
                                QList<conversation_data> conversations,
                                std::uint64_t generation,
                                std::uint64_t connection);
    std::uint64_t conversations_generation_ = 0;
    std::uint64_t files_generation_ = 0;
    std::uint64_t contacts_generation_ = 0;
    std::uint64_t friend_requests_generation_ = 0;
    std::uint64_t messages_generation_ = 0;
    std::uint64_t search_generation_ = 0;
    quint64 bookmark_request_ = 0;
    quint64 global_search_request_ = 0;
    std::uint64_t history_generation_ = 0;
    std::atomic_uint64_t connection_generation_ = 0;
    std::unique_ptr<chat::client> client_;
};

#endif
