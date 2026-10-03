#ifndef CHAT_QT_SRC_CLIENT_BRIDGE_HPP
#define CHAT_QT_SRC_CLIENT_BRIDGE_HPP

#include <memory>
#include <atomic>
#include <optional>
#include <cstdint>
#include <utility>
#include <chat/conversation.hpp>

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
}

class client_bridge final : public QObject
{
    Q_OBJECT

   public:
    explicit client_bridge(QObject* parent = nullptr);
    ~client_bridge() override;

    void connect_to_server(QString const& url);
    void close();
    void authenticate(QString const& username, QString const& password);
    void register_user(QString const& username, QString const& password);
    void get_conversations();
    void set_conversation_muted(qint64 conversation, bool muted);
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
    void remove_group_member(qint64 conversation, qint64 user);
    void rename_group(qint64 conversation, QString title);
    void set_group_announcement(qint64 conversation, QString text);
    void invite_group_members(qint64 conversation, QList<qint64> members);
    void leave_group(qint64 conversation);
    void set_group_pinned_message(qint64 conversation, std::optional<qint64> message);
    void get_contacts();
    void get_friend_requests();
    void respond_friend_request(qint64 user, bool accept);
    void cancel_friend_request(qint64 user);
    void get_presence();
    void get_messages(qint64 conversation, std::optional<qint64> before = {}, std::optional<qint64> after = {});
    void send_message(qint64 user, QString text, qint64 reply_to = 0);
    void delete_message(qint64 conversation, qint64 message);
    void edit_message(qint64 conversation, qint64 message, QString text);
    void set_message_reaction(qint64 conversation, qint64 message, QString emoji);
    void search_users(QString query);
    void search_messages(qint64 conversation, QString query, qint64 before = 0);
    void add_contact(qint64 user);
    void remove_contact(qint64 user);
    void send_attachment(qint64 conversation, QString filename, QByteArray data, qint64 reply_to = 0);
    void get_attachment(qint64 conversation, qint64 message);
    void get_message_image(qint64 conversation, qint64 message);
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
    void registration_finished(qint64 user, QString error_message);
    void conversations_received(QList<conversation_data> conversations, QString error_message);
    void mute_finished(qint64 conversation, bool muted, QString error_message);
    void pin_finished(qint64 conversation, bool pinned, QString error_message);
    void friendship_changed(qint64 user);
    void friend_requests_received(QList<user_data> incoming, QList<user_data> outgoing, QString error_message);
    void contacts_received(QList<user_data> contacts, QString error_message);
    void presences_received(QList<presence_data> users, QString error_message);
    void presence_changed(presence_data user);
    void messages_received(qint64 conversation, QList<message_data> messages, read_positions positions, bool older,
                           bool recovering, bool has_more, QString error_message);
    void message_received(message_data message);
    void message_updated(qint64 conversation, message_data message, QString error_message);
    void reaction_changed(qint64 conversation, qint64 message, qint64 revision, QList<reaction_data> reactions,
                          QString error_message);
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
    void conversation_changed(qint64 conversation, bool removed);
    void message_sent(qint64 user, QString text, qint64 message, qint64 timestamp, bool realtime,
                      quoted_message_data reply, QList<mention_data> mentions, QString error_message);
    void users_received(QList<user_data> users, QString error_message);
    void contact_added(user_data user, QString error_message);
    void contact_removed(QString error_message);
    void attachment_sent(qint64 conversation, message_data message, QString error_message);
    void attachment_received(qint64 conversation, qint64 message, QByteArray data, QString error_message);
    void message_image_received(qint64 conversation, qint64 message, QByteArray data, QString error_message);
    void message_search_received(qint64 conversation, QString query, qint64 before, QList<message_data> messages,
                                 read_positions positions, bool has_more, QString error_message);
    void read_marked(qint64 user, qint64 message, QString error_message);

   private:
    template <class F> void post_result(std::uint64_t generation, F result)
    {
        QMetaObject::invokeMethod(this, [this, generation, result = std::move(result)]() mutable {
            if (generation == connection_generation_) { result(); }
        }, Qt::QueuedConnection);
    }
     void get_conversations_page(std::optional<chat::conversation_cursor> before,
                                 QList<conversation_data> conversations, std::uint64_t generation);
     std::uint64_t conversations_generation_ = 0;
     std::uint64_t contacts_generation_ = 0;
     std::uint64_t friend_requests_generation_ = 0;
     std::uint64_t messages_generation_ = 0;
     std::uint64_t search_generation_ = 0;
     std::uint64_t upload_generation_ = 0;
     std::uint64_t download_generation_ = 0;
    std::atomic_uint64_t connection_generation_ = 0;
    std::unique_ptr<chat::client> client_;
};

#endif
