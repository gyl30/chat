#ifndef CHAT_QT_SRC_CHAT_WIDGET_HPP
#define CHAT_QT_SRC_CHAT_WIDGET_HPP

#include <QHash>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>
#include <QtGlobal>
#include <QByteArray>
#include <optional>
#include <chat/friendship.hpp>

#include "conversation_data.hpp"
#include "avatar.hpp"
#include "message_data.hpp"
#include "presence_data.hpp"
#include "user_data.hpp"
#include "member_data.hpp"
#include "message_images.hpp"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QListView;
class QListWidget;
class QModelIndex;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QToolButton;
class QTimer;
class conversation_model;
class feedback_label;
class message_model;
class user_model;

class chat_widget final : public QWidget
{
    Q_OBJECT

   public:
    explicit chat_widget(QWidget* parent = nullptr);

    avatar_cache& avatars() { return avatars_; }
    message_images& images() { return images_; }
    void finish_avatar_update(QString error);
    void set_user(QString const& username, qint64 user = 0);
    void open_conversation(conversation_data conversation);
    void close_conversation(qint64 conversation);
    void locate_message(qint64 conversation, qint64 message);
    void show_contact_card(qint64 user, QString const& username);
    void refresh_theme();
    void set_members(qint64 conversation, QList<member_data> members, QString const& error);
    void set_loading();
    void set_error(QString message);
    void set_conversations_error(QString message);
    void set_connection_available(bool available);
    void set_connection_status(QString text, bool retry_enabled);
    void set_conversations(QList<conversation_data> conversations);
    void set_contacts(QList<user_data> contacts);
    void set_friend_requests(QList<user_data> incoming, QList<user_data> outgoing, QString error);
    void set_presences(QList<presence_data> users);
    void set_presence(presence_data user);
    void set_contacts_error(QString message);
    void set_add_contact_search_results(QList<user_data> users);
    void set_add_contact_search_error(QString message);
    void finish_add_contact(qint64 user, QString error);
    void set_messages(qint64 conversation, QList<message_data> messages, read_positions positions, bool older,
                      bool recovering, bool has_more);
    void set_read_message(qint64 conversation, qint64 user, qint64 message);
    void reset_read_positions(qint64 conversation);
    void set_typing(qint64 conversation, qint64 user, QString username, bool typing);
    void add_message(qint64 user, message_data message);
    void update_message(message_data message);
    bool set_reactions(qint64 conversation, qint64 message, qint64 revision, QList<reaction_data> reactions);
    void finish_message_send(qint64 user, qint64 message, qint64 timestamp, QString text, quoted_message_data reply,
                             QList<mention_data> mentions, QString error);
    void set_message_error(qint64 user, QString message);
    void finish_attachment_send(qint64 conversation, QString error_message);

    qint64 active_conversation() const noexcept;
    qint64 latest_message_id() const;
    std::optional<qint64> recovery_cursor() const;
    bool messages_ready() const;
    bool viewing_latest() const;
    void mark_visible_messages();
    qint64 self_user() const { return self_user_; }
    std::optional<conversation_data> conversation(qint64 id) const;
    void set_conversation_muted(qint64 conversation, bool muted);
    void set_conversation_pinned(qint64 conversation, bool pinned);
    void show_user_details(qint64 user, QString const& username);

   signals:
    void avatar_set_requested(QByteArray data);
    void avatar_clear_requested();
    void avatar_update_finished(QString error);
    void conversation_selected(qint64 user, bool group);
    void older_messages_requested(qint64 user, qint64 before);
    void send_message_requested(qint64 user, QString text, qint64 reply_to);
    void delete_message_requested(qint64 conversation, qint64 message);
    void edit_message_requested(qint64 conversation, qint64 message, QString text);
    void reaction_requested(qint64 conversation, qint64 message, QString emoji);
    void add_contact_search_requested(QString query);
    void contact_add_requested(qint64 user);
    void friend_request_respond_requested(qint64 user, bool accept);
    void friend_request_cancel_requested(qint64 user);
    void friendship_updated();
    void contact_remove_requested(qint64 user);
    void contact_add_finished(qint64 user, QString error);
    void logout_requested();
    void reconnect_requested();
    void direct_conversation_requested(qint64 user, QString username);
    void group_create_requested(QString title, QList<qint64> members);
    void group_join_requested(QString token);
    void members_requested(qint64 conversation, qint64 self_user, QString title);
    void message_search_requested(qint64 conversation, qint64 self_user, bool group, QString title, QString query);
    void group_message_pin_requested(qint64 conversation, std::optional<qint64> message);
    void attachment_send_requested(qint64 conversation, QString filename, QByteArray data, qint64 reply_to);
    void attachment_open_requested(qint64 conversation, qint64 message, QString filename, bool preview);
    void typing_requested(qint64 conversation, bool typing);
    void read_requested(qint64 conversation, qint64 message);
    void mute_requested(qint64 conversation, bool muted);
    void pin_requested(qint64 conversation, bool pinned);

   protected:
    bool eventFilter(QObject* object, QEvent* event) override;

   private:
    void show_conversations_section();
    void show_contacts_section();
    void show_new_friends_section();
    void show_add_contact_section();
    void filter_contacts(QString const& query);
    void search_users();
    void select_contact(QModelIndex const& index);
    void update_contact_card();
    void contact_card_primary();
    void contact_card_secondary();
    void request_friend_action(qint64 user, QString const& username, int action);
    void find_user();
    void filter_conversations();
    void set_list_status(QString text);
    void update_unread_badge();
    void show_loading_status();
    void update_friend_badges();
    void select_add_user(QModelIndex const& index);
    void select_conversation(QModelIndex const& index);
    void open_chat(qint64 user, QString username);
    bool is_contact(qint64 user) const;
    chat::friendship_state friend_state(qint64 user) const;
    bool can_send() const;
    void update_compose_state();
    void create_group();
    void request_older_messages();
    void continue_locate();
    void send_current_message();
    void show_read_details(qint64 conversation, qint64 message);
    void update_chat_header(QString const& username);
    void update_pinned_message();
    void update_chat_presence();
    void set_message_status(QString message);
    void stop_typing();
    void update_typing_label();
    void load_visible_images();

    avatar_cache avatars_;
    message_images images_;
    bool avatar_updating_ = false;
    QToolButton* profile_avatar_ = nullptr;
    QToolButton* chats_navigation_ = nullptr;
    QToolButton* contacts_navigation_ = nullptr;
    QToolButton* chats_actions_ = nullptr;
    QToolButton* sidebar_back_button_ = nullptr;
    QToolButton* add_contact_button_ = nullptr;
    QToolButton* message_search_button_ = nullptr;
    QPushButton* pinned_message_button_ = nullptr;
    QToolButton* unpin_message_button_ = nullptr;
    QToolButton* attachment_button_ = nullptr;
    QLabel* section_title_ = nullptr;
    QStackedWidget* sidebar_pages_ = nullptr;
    enum class sidebar_parent { chats, contacts, new_friends };
    sidebar_parent add_friend_parent_ = sidebar_parent::contacts;
    QListView* conversations_view_ = nullptr;
    QLabel* conversations_status_ = nullptr;
    QStackedWidget* right_pages_ = nullptr;
    QLabel* contact_placeholder_ = nullptr;
    QToolButton* cancel_reply_button_ = nullptr;
    QStackedWidget* contact_detail_ = nullptr;
    QLabel* contact_card_avatar_ = nullptr;
    QLabel* contact_card_name_ = nullptr;
    QLabel* contact_card_status_ = nullptr;
    QPushButton* contact_card_message_ = nullptr;
    QPushButton* contact_card_profile_ = nullptr;
    qint64 contact_card_user_ = 0;
    QString contact_card_username_;
    QPushButton* find_user_button_ = nullptr;
    QLabel* new_friends_badge_ = nullptr;
    QLabel* navigation_badge_ = nullptr;
    QLabel* chats_badge_ = nullptr;
    QLineEdit* conversation_search_ = nullptr;
    QToolButton* chat_more_button_ = nullptr;
    QPushButton* empty_add_friend_button_ = nullptr;
    // The action awaiting its result per user, so the result can be reported in words.
    QHash<qint64, QPair<int, QString>> pending_friend_actions_;
    // Requests handled on the New friends page stay listed with their outcome until it is reopened.
    QList<QPair<user_data, QString>> handled_requests_;
    feedback_label* notice_ = nullptr;
    QTimer* notice_timer_ = nullptr;
    QTimer* loading_status_timer_ = nullptr;
    QLineEdit* contact_search_ = nullptr;
    QListView* contacts_view_ = nullptr;
    QLabel* contacts_status_ = nullptr;
    QPushButton* new_friends_button_ = nullptr;
    QListWidget* incoming_friends_ = nullptr;
    QListWidget* outgoing_friends_ = nullptr;
    QLabel* incoming_friends_title_ = nullptr;
    QLabel* outgoing_friends_title_ = nullptr;
    QLabel* friend_requests_status_ = nullptr;
    QList<user_data> incoming_requests_, outgoing_requests_;
    QLineEdit* add_user_search_ = nullptr;
    QListView* add_users_view_ = nullptr;
    QLabel* add_users_status_ = nullptr;
    QPushButton* chat_title_ = nullptr;
    QLabel* chat_presence_ = nullptr;
    QLabel* typing_label_ = nullptr;
    QTimer* typing_idle_timer_ = nullptr;
    QElapsedTimer typing_clock_;
    qint64 last_typing_sent_ = 0;
    struct peer_typing
    {
        QString username;
        QDeadlineTimer expiry;
    };
    QHash<qint64, peer_typing> typing_users_;
    QToolButton* connection_status_ = nullptr;
    QLabel* message_status_ = nullptr;
    QListView* messages_view_ = nullptr;
    QPlainTextEdit* message_edit_ = nullptr;
    QHash<qint64, QString> drafts_;
    QSet<qint64> message_sending_;
    QWidget* reply_bar_ = nullptr;
    QLabel* reply_preview_ = nullptr;
    qint64 reply_to_ = 0;
    QToolButton* send_button_ = nullptr;
    conversation_model* conversations_ = nullptr;
    user_model* contacts_ = nullptr;
    QSortFilterProxyModel* contacts_filter_ = nullptr;
    user_model* add_users_ = nullptr;
    message_model* messages_ = nullptr;
    QHash<qint64, presence_data> presence_;
    qint64 self_user_ = 0;
    qint64 active_conversation_ = 0;
    qint64 active_peer_ = 0;
    bool active_group_ = false;
    quint64 active_member_count_ = 0;
    QString active_username_;
    bool messages_loaded_ = false;
    bool messages_loading_ = false;
    bool history_exhausted_ = false;
    qint64 pending_locate_ = 0;
    qint64 attachment_reply_ = 0;
    QString attachment_reply_text_;
    int locate_pages_ = 0;
    bool connection_available_ = true;
    bool attachment_sending_ = false;
};

#endif
