#ifndef CHAT_QT_SRC_CHAT_WIDGET_HPP
#define CHAT_QT_SRC_CHAT_WIDGET_HPP

#include <QHash>
#include <QPoint>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QList>
#include <QModelIndex>
#include <QSet>
#include <QString>
#include <QWidget>
#include <QtGlobal>
#include <QByteArray>
#include <optional>
#include <chat/friendship.hpp>
#include <chat/client.hpp>

#include "conversation_data.hpp"
#include "avatar.hpp"
#include "message_data.hpp"
#include "presence_data.hpp"
#include "user_data.hpp"
#include "member_data.hpp"
#include "message_images.hpp"

class QAction;
class QDialog;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QListView;
class QListWidget;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QToolButton;
class QTimer;
class conversation_model;
class feedback_label;
class message_model;
class user_model;

class QVBoxLayout;

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
    void hide_conversation(qint64 conversation);
    qint64 history_floor(qint64 conversation) const { return history_floors_.value(conversation); }
    void discard_history(qint64 conversation, qint64 floor);
    void close_conversation(qint64 conversation);
    void invalidate_draft_replies(QList<qint64> const& conversations);
    void locate_message(qint64 conversation, qint64 message);
    void refresh_theme();
    void set_members(qint64 conversation, QList<member_data> members, QString const& error);
    void set_loading();
    void set_error(QString message);
    void set_conversations_error(QString message);
    void set_connection_available(bool available);
    void set_connection_status(QString text, bool retry_enabled);
    void set_conversations(QList<conversation_data> conversations);
    void set_friend_groups(QStringList const& groups);
    void set_contacts(QList<user_data> contacts);
    void set_friend_requests(QList<user_data> incoming, QList<user_data> outgoing, QString error);
    void set_presences(QList<presence_data> users);
    void set_presence(presence_data user);
    void set_contacts_error(QString message);
    void set_add_contact_search_results(QList<user_data> users, quint64 request);
    void set_add_contact_search_error(QString message, quint64 request);
    void finish_add_contact(qint64 user, QString error);
    void set_messages(qint64 conversation, QList<message_data> messages, read_positions positions, bool older, bool recovering, bool has_more);
    void set_read_message(qint64 conversation, qint64 user, qint64 message);
    void reset_read_positions(qint64 conversation);
    void set_typing(qint64 conversation, qint64 user, QString username, bool typing);
    void add_message(qint64 user, message_data message);
    void update_message(message_data message);
    bool set_reactions(qint64 conversation, qint64 message, qint64 revision, QList<reaction_data> reactions);
    // The text or attachment result distinguishes sent, lost, refused, and unavailable.
    void finish_send(qint64 conversation, qint64 client_id, message_data message, QString error, send_status status);
    // After signing in again: the sends the link dropped go out again, in the order they were made.
    void finish_text_save(qint64 client_id, QString error);
    void flush_draft();
    void restore_drafts(qint64 user, std::vector<chat::compose_draft> const& drafts);
    void validate_draft_reply(qint64 conversation, qint64 anchor, qint64 revision, message_data const& message, bool unavailable);
    void update_pending_text(qint64 conversation, qint64 client_id, QString text, qint64 reply, qint64 created, bool sending, QString rejection);
    void discard_pending_text(qint64 client_id);
    void reconcile_pending_texts(QList<qint64> client_ids);
    void choose_attachment(qint64 conversation, bool group_only, QWidget* parent);
    void finish_group_file(qint64 client_id, QString error);
    void restore_attachment(qint64 conversation, qint64 id, QString path, qint64 size, qint64 reply, qint64 confirmed, bool group_only);
    void attachment_progress(qint64 id, qint64 bytes, qint64 total);
    void attachment_cancelled(qint64 id);
    // Only an unfinished local write still needs a quit warning.
    bool saving_text() const { return saving_text_.has_value(); }
    void set_message_error(qint64 user, QString message);

    qint64 active_conversation() const noexcept;
    qint64 latest_message_id() const;
    std::optional<qint64> recovery_cursor() const;
    bool messages_ready() const;
    bool viewing_latest() const;
    void mark_visible_messages();
    qint64 self_user() const { return self_user_; }
    std::optional<conversation_data> conversation(qint64 id) const;
    void set_notifications(qint64 conversation, chat::notification_level level, qint64 until);
    void set_conversation_pinned(qint64 conversation, bool pinned);
    void show_user_details(qint64 user, QString const& username, QWidget* profile_parent = nullptr);
    void set_control_enter_send(bool enabled);
    bool control_enter_send() const { return control_enter_send_; }

   signals:
    void avatar_set_requested(QByteArray data);
    void avatar_clear_requested();
    void avatar_update_finished(QString error);
    void conversation_selected(qint64 user, bool group);
    void older_messages_requested(qint64 user, qint64 before);
    void send_message_requested(qint64 conversation, QString text, qint64 reply_to, qint64 client_id, qint64 revision);
    void draft_save_requested(qint64 user, qint64 conversation, qint64 revision, qint64 reply, QString text);
    void draft_reply_requested(qint64 conversation, qint64 anchor, qint64 revision);
    void pending_text_action_requested(qint64 client_id, bool discard);
    void delete_message_requested(qint64 conversation, qint64 message);
    void edit_message_requested(qint64 conversation, qint64 message, QString text);
    void reaction_requested(qint64 conversation, qint64 message, QString emoji);
    void add_contact_search_requested(QString query, quint64 request);
    void set_friend_remark_requested(qint64 user, QString remark);
    void move_friend_requested(qint64 user, QString group);
    void create_friend_group_requested(QString name);
    void rename_friend_group_requested(QString group, QString name);
    void delete_friend_group_requested(QString group);
    void contact_add_requested(qint64 user);
    void friend_request_respond_requested(qint64 user, bool accept);
    void friend_request_cancel_requested(qint64 user);
    void friendship_updated();
    void contact_remove_requested(qint64 user);
    void contact_add_finished(qint64 user, QString error);
    void settings_requested();
    void download_tasks_requested();
    void reconnect_requested();
    void direct_conversation_requested(qint64 user, QString username);
    void group_create_requested(QString title, QList<qint64> members);
    void group_join_requested(QString token);
    void members_requested(qint64 conversation, qint64 self_user, QString title);
    void message_search_requested(qint64 conversation, qint64 self_user, bool group, QString title, QString query);
    void global_search_requested();
    void message_context_requested(qint64 conversation, qint64 anchor);
    void chat_history_requested(qint64 conversation, QString title);
    void group_message_pin_requested(qint64 conversation, std::optional<qint64> message);
    void add_file_requested(qint64 conversation, QString key);
    void forward_requested(qint64 target, QList<qint64> messages);
    void attachment_send_requested(qint64 conversation, QString path, qint64 reply_to, qint64 client_id, bool group_only);
    void attachment_interrupt_requested(qint64 id, bool cancel, QString path);
    void attachment_open_requested(qint64 conversation, qint64 message, QString filename, bool preview);
    void typing_requested(qint64 conversation, bool typing);
    void read_requested(qint64 conversation, qint64 message);
    void history_discarded(qint64 conversation, qint64 through);
    void history_view_requested(qint64 conversation, bool hide);
    void notification_level_requested(qint64 conversation, chat::notification_level level, qint64 until);
    void unread_mark_requested(qint64 conversation, bool marked);
    void pin_requested(qint64 conversation, bool pinned);

   protected:
    bool eventFilter(QObject* object, QEvent* event) override;

   private:
    QWidget* create_sidebar();
    void setup_contacts_dialog();
    void setup_add_contact_dialog();
    void setup_friend_requests();
    QWidget* create_chat_panel();
    void setup_profile_friendship(
        QDialog& dialog, QVBoxLayout* info_layout, QLabel* relationship, QToolButton* message_button, qint64 user, QString const& username);
    void setup_profile_avatar(QDialog& dialog, QVBoxLayout* info_layout, qint64 user);
    void setup_chat_header(QVBoxLayout* chat_layout);
    void setup_message_list(QVBoxLayout* chat_layout);
    void setup_composer(QVBoxLayout* chat_layout);
    void set_message_selection(bool enabled);
    QModelIndexList selected_messages() const;
    void choose_forward_target(QList<qint64> messages);
    void copy_selected_messages();
    void show_message_menu(QPoint position);
    void connect_chat_controls();

    // A send owns its text or file and its reply from the moment it is made (docs/outgoing-messages-design.md).
    struct outgoing
    {
        qint64 client_id = 0;
        qint64 conversation = 0;
        QString text;
        // The SDK reads the original file one part at a time.
        QString filename;
        QString path;
        qint64 size = 0;
        quoted_message_data reply;
        qint64 created = 0;
        bool group_only = false;
        bool failed = false;
        QString error;
        bool waiting = false;
    };
    void show_conversations_section();
    void show_contacts_dialog();
    void show_contacts_home();
    void show_friend_requests();
    void update_request_actions();
    void show_add_contact_dialog();
    void filter_contacts(QString const& query);
    void search_users();
    void select_contact(QModelIndex const& index);
    void request_friend_action(qint64 user, QString const& username, int action);
    void filter_conversations();
    void set_list_status(QString text);
    void update_main_menu();
    void show_loading_status();
    void show_emoji_picker();
    void select_add_user(QModelIndex const& index);
    void select_conversation(QModelIndex const& index);
    void open_chat(qint64 user, QString username);
    bool is_contact(qint64 user) const;
    chat::friendship_state friend_state(qint64 user) const;
    bool can_queue_text() const;
    bool can_send() const;
    void update_compose_state();
    void create_group();
    void request_older_messages();
    void send_current_message();
    void draft_edited();
    void save_active_draft();
    void apply_draft();
    void load_draft_reply();
    void reset_reply(qint64 reply = 0);
    void show_reply(quoted_message_data reply);
    void discard_draft_reply(qint64 conversation, qint64 through, bool exact = false);
    bool handle_file_drop(QEvent* event);
    void send_image(QImage const& image);
    bool paste_image();
    void send_files(qint64 target, QStringList const& paths, qint64 clipboard_id = 0, bool group_only = false);
    void queue_outgoing(outgoing item);
    void submit(outgoing const& item);
    // Shows the open conversation's unconfirmed sends after its messages.
    void show_outgoing();
    // A stored message carrying an unconfirmed send's client_id finishes it.
    void settle(message_data const& message);
    void drop_outgoing(qint64 conversation);
    void leave_conversation(qint64 conversation);
    void show_outgoing_menu(QModelIndex const& index, QPoint position);
    quoted_message_data quote(qint64 message) const;
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
    QToolButton* main_menu_button_ = nullptr;
    QAction* self_profile_action_ = nullptr;
    QAction* chats_action_ = nullptr;
    QAction* contacts_action_ = nullptr;
    QAction* message_search_action_ = nullptr;
    QPushButton* pinned_message_button_ = nullptr;
    QToolButton* unpin_message_button_ = nullptr;
    QLabel* file_drop_hint_ = nullptr;
    QToolButton* attachment_button_ = nullptr;
    QDialog* contacts_dialog_ = nullptr;
    QStackedWidget* contacts_pages_ = nullptr;
    QLabel* contacts_title_ = nullptr;
    QToolButton* contacts_back_button_ = nullptr;
    QPushButton* contacts_requests_button_ = nullptr;
    QLabel* contacts_requests_icon_ = nullptr;
    QLabel* contacts_requests_arrow_ = nullptr;
    QLabel* contacts_requests_summary_ = nullptr;
    QLabel* contacts_requests_badge_ = nullptr;
    feedback_label* contacts_feedback_ = nullptr;
    QPushButton* incoming_accept_button_ = nullptr;
    QPushButton* incoming_reject_button_ = nullptr;
    QPushButton* outgoing_cancel_button_ = nullptr;
    QDialog* add_contact_dialog_ = nullptr;
    QToolButton* contacts_add_button_ = nullptr;
    QPushButton* add_search_button_ = nullptr;
    quint64 add_search_request_ = 0;
    QListView* conversations_view_ = nullptr;
    QLabel* conversations_status_ = nullptr;
    QToolButton* cancel_reply_button_ = nullptr;
    QLabel* main_menu_badge_ = nullptr;
    QLineEdit* conversation_search_ = nullptr;
    QToolButton* chat_more_button_ = nullptr;
    QToolButton* screenshot_button_ = nullptr;
    QToolButton* emoji_button_ = nullptr;
    QToolButton* history_button_ = nullptr;
    // The action awaiting its result per user, so the result can be reported in words.
    QHash<qint64, QPair<int, QString>> pending_friend_actions_;
    feedback_label* notice_ = nullptr;
    QTimer* notice_timer_ = nullptr;
    QTimer* loading_status_timer_ = nullptr;
    QLineEdit* contact_search_ = nullptr;
    QListView* contacts_view_ = nullptr;
    QLabel* contacts_status_ = nullptr;
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
    QTimer* mute_expiry_timer_ = nullptr;
    QTimer* typing_idle_timer_ = nullptr;
    QElapsedTimer last_typing_sent_;
    struct peer_typing
    {
        QString username;
        QDeadlineTimer expiry;
    };
    QHash<qint64, peer_typing> typing_users_;
    QToolButton* connection_status_ = nullptr;
    QLabel* message_status_ = nullptr;
    QWidget* selection_bar_ = nullptr;
    QListView* messages_view_ = nullptr;
    QWidget* input_bar_ = nullptr;
    QLabel* speech_notice_ = nullptr;
    QTimer* speech_expiry_ = nullptr;
    QPlainTextEdit* message_edit_ = nullptr;
    QHash<qint64, qint64> history_floors_;
    bool visible_message(message_data& message) const;
    struct draft
    {
        QString text;
        qint64 reply = 0;
        qint64 revision = 0;
    };
    QHash<qint64, draft> drafts_;
    QTimer* draft_timer_ = nullptr;
    QList<outgoing> outgoing_;
    struct text_save
    {
        qint64 client_id;
        qint64 draft_conversation;
        qint64 revision;
    };
    std::optional<text_save> saving_text_;
    QWidget* reply_bar_ = nullptr;
    QLabel* reply_preview_ = nullptr;
    qint64 reply_to_ = 0;
    quoted_message_data reply_snapshot_;
    qint64 draft_reply_request_ = 0;
    QToolButton* send_button_ = nullptr;
    conversation_model* conversations_ = nullptr;
    user_model* contacts_ = nullptr;
    QComboBox* friend_groups_ = nullptr;
    QSortFilterProxyModel* contacts_filter_ = nullptr;
    user_model* add_users_ = nullptr;
    message_model* messages_ = nullptr;
    QHash<qint64, presence_data> presence_;
    qint64 self_user_ = 0;
    QString self_username_;
    qint64 active_conversation_ = 0;
    qint64 active_peer_ = 0;
    bool active_group_ = false;
    quint64 active_member_count_ = 0;
    QString active_username_;
    bool messages_loaded_ = false;
    bool messages_loading_ = false;
    bool history_exhausted_ = false;
    bool control_enter_send_ = false;
    bool connection_available_ = true;
};

#endif
