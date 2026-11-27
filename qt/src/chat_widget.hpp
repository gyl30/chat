#ifndef CHAT_QT_SRC_CHAT_WIDGET_HPP
#define CHAT_QT_SRC_CHAT_WIDGET_HPP

#include <QHash>
#include <QPoint>
#include <QPointer>
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

class client_bridge;
class download_tasks;
class QAction;
class QDialog;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QListView;
class QListWidget;
class QPushButton;
class QMenu;
class friend_list_model;
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

    struct upload_row
    {
        qint64 id;
        QString filename;
        qint64 size;
        QString status;
    };
    QList<upload_row> group_uploads(qint64 conversation) const;
    void add_upload_actions(QMenu& menu, qint64 id);
    void set_downloads(download_tasks& downloads);
    void setup_unified_search(client_bridge& client);
    avatar_cache& avatars() { return avatars_; }
    message_images& images() { return images_; }
    void finish_avatar_update(QString error);
    void set_user(QString const& username, qint64 user = 0, QString const& server = {});
    void open_conversation(conversation_data conversation);
    void hide_conversation(qint64 conversation);
    qint64 history_floor(qint64 conversation) const { return history_floors_.value(conversation); }
    void discard_history(qint64 conversation, qint64 floor);
    void close_conversation(qint64 conversation);
    void invalidate_draft_replies(QList<qint64> const& conversations);
    void locate_message(qint64 conversation, qint64 message);
    void jump_to_message(qint64 id);
    void refresh_theme();
    void set_members(qint64 conversation, QList<member_data> members, QString const& error);
    void set_loading();
    void set_error(QString message);
    void set_conversations_error(QString message);
    void set_connection_available(bool available);
    void set_connection_status(QString text, bool retry_enabled);
    void set_folders(conversation_folder_list folders);
    void set_conversations(QList<conversation_data> conversations);
    QString friend_remark(qint64 user) const;
    void set_friend_groups(QStringList const& groups);
    void set_contacts(QList<user_data> contacts);
    void set_friend_requests(QList<user_data> incoming, QList<user_data> outgoing, QString error);
    void set_presences(QList<presence_data> users);
    void set_presence(presence_data user);
    void set_contacts_error(QString message);
    void set_add_contact_search_results(QList<user_data> users, quint64 request);
    void set_add_contact_search_error(QString message, quint64 request);
    void finish_add_contact(qint64 user, QString error);
    void set_messages(qint64 conversation,
                      QList<message_data> messages,
                      read_positions positions,
                      history_page page,
                      bool has_more,
                      bool has_newer,
                      std::optional<qint64> first_unread);
    void set_read_message(qint64 conversation, qint64 user, qint64 message);
    void invalidate_members(qint64 conversation);
    void set_typing(qint64 conversation, qint64 user, QString username, bool typing);
    void open_poll_results(client_bridge& client, qint64 room, qint64 message);
    void finish_poll_write(qint64 room, qint64 message, QString error);
    void update_poll(chat::poll_update const& update);
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
    void update_pending_text(qint64 conversation,
                             qint64 client_id,
                             QString text,
                             qint64 reply,
                             qint64 created,
                             bool sending,
                             QString rejection,
                             bool unavailable,
                             chat::text_options options);
    void discard_pending_text(qint64 client_id);
    void reconcile_pending_texts(QList<qint64> client_ids);
    void choose_attachment(qint64 conversation, bool group_only, QWidget* parent);
    void finish_group_file(qint64 client_id, QString error, bool unavailable);
    void restore_attachment(qint64 conversation, qint64 id, QString path, qint64 size, qint64 reply, qint64 confirmed, bool group_only);
    void attachment_progress(qint64 id, qint64 bytes, qint64 total);
    void attachment_cancelled(qint64 id);
    // Only an unfinished local write still needs a quit warning.
    bool saving_text() const { return saving_text_.has_value(); }
    void set_message_error(qint64 user, QString message);
    void set_history_error(qint64 conversation, QString message);

    void return_to_latest();
    qint64 active_conversation() const noexcept;
    qint64 latest_message_id() const;
    bool messages_ready() const;
    bool viewing_latest() const;
    void read_failed(qint64 conversation);
    void begin_history_refresh();
    void mark_visible_messages();
    qint64 self_user() const { return self_user_; }
    std::optional<conversation_data> conversation(qint64 id) const;
    void set_notifications(qint64 conversation, chat::notification_level level, qint64 until);
    void set_conversation_pinned(qint64 conversation, bool pinned);
    void show_user_details(qint64 user, QString const& username, QWidget* profile_parent = nullptr);
    void set_control_enter_send(bool enabled);
    bool control_enter_send() const { return control_enter_send_; }

   signals:
    void uploads_changed();
    void avatar_set_requested(QByteArray data);
    void avatar_clear_requested();
    void avatar_update_finished(QString error);
    void conversation_selected(qint64 user, bool group);
    void history_requested(qint64 conversation, history_page page, qint64 cursor = 0);
    void send_message_requested(qint64 conversation, QString text, qint64 reply_to, qint64 client_id, qint64 revision, chat::text_options options);
    void draft_save_requested(qint64 user, qint64 conversation, qint64 revision, qint64 reply, QString text, chat::text_options options);
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

    void profile_access_changed();
    void profile_requested(qint64 user);
    void profile_received(qint64 user, QString username, QString nickname, QString signature);
    void profile_edit_requested(QString nickname, QString signature);
    void friendship_updated();
    void poll_creation_availability(qint64 room, bool allowed);
    void contact_remove_requested(qint64 user);
    void contact_add_finished(qint64 user, QString error);
    void settings_requested();
    void reconnect_requested();
    void direct_conversation_requested(qint64 user, QString username);
    void group_create_requested(QString title, QList<qint64> members);
    void group_join_requested(QString token);
    void members_requested(qint64 conversation, qint64 self_user, QString title, bool requests = false);
    void message_search_requested(qint64 conversation, qint64 self_user, bool group, QString title, QString query);
    void profile_bookmarks_requested(QWidget* parent);
    void profile_devices_requested(QWidget* parent);
    void logout_requested(QWidget* parent);
    void bookmark_menu_requested(qint64 message, QAction* action);
    void bookmark_requested(qint64 message, bool bookmarked);
    void global_search_requested(QString query);
    void message_context_requested(qint64 conversation, qint64 anchor);
    void chat_history_requested(qint64 conversation, QString title);
    void group_message_pin_requested(qint64 conversation, std::optional<qint64> message);
    void add_file_requested(qint64 conversation, QString key);
    void poll_write_requested(qint64 room, qint64 message, qint64 choice);
    void live_poll_added(qint64 room, qint64 message);
    void create_poll_requested(qint64 room);
    void content_requested(qint64 conversation, qint64 message, QString kind);
    void bundle_requested(qint64 target, QList<qint64> messages);
    void forward_to_contact_requested(qint64 peer, QList<qint64> messages, bool bundle);
    void forward_requested(qint64 target, QList<qint64> messages);
    void attachment_send_requested(qint64 conversation, QString path, qint64 reply_to, qint64 client_id, bool group_only);
    void attachment_interrupt_requested(qint64 id, bool cancel, QString path);
    void attachment_open_requested(qint64 conversation, qint64 message, QString filename, bool preview);
    void typing_requested(qint64 conversation, bool typing);
    void read_requested(qint64 conversation, qint64 message);
    void history_discarded(qint64 conversation, qint64 through);
    void history_view_requested(qint64 conversation, bool hide);
    void notification_level_requested(qint64 conversation, chat::notification_level level, qint64 until);
    void create_conversation_folder(QString name);
    void rename_conversation_folder(qint64 folder, QString name);
    void delete_conversation_folder(qint64 folder);
    void reorder_conversation_folder(qint64 folder, bool up);
    void move_conversation_to_folder(qint64 conversation, qint64 folder);
    void mark_read_scope(qint64 conversation, qint64 folder);
    void unread_mark_requested(qint64 conversation, bool marked);
    void pin_requested(qint64 conversation, bool pinned);

   protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

   private:
    QWidget* create_sidebar();
    void add_list_actions(QMenu& menu);
    void add_local_content(QMenu& menu);
    void show_conversation_menu(QPoint point);
    void setup_contacts_dialog();
    void friend_group_menu(QPoint point, QString const& group);
    void setup_add_contact_dialog();
    void setup_friend_requests();
    QWidget* create_chat_panel();
    void close_poll_results();
    void layout_poll_results();
    QWidget* timeline_;
    QWidget* poll_page_ = nullptr;
    QPointer<QWidget> poll_return_focus_;
    void setup_profile_friendship(
        QDialog& dialog, QVBoxLayout* info_layout, QLabel* relationship, QToolButton* message_button, qint64 user, QString const& username);
    void setup_profile_avatar(QDialog& dialog, QVBoxLayout* info_layout, qint64 user);
    void setup_chat_header(QVBoxLayout* chat_layout);
    void setup_message_list(QVBoxLayout* chat_layout);
    void setup_composer(QVBoxLayout* chat_layout);
    void set_message_selection(bool enabled);
    void update_message_selection();
    QModelIndexList selected_messages() const;
    void choose_forward_target(QList<QPersistentModelIndex> const& rows);
    void copy_selected_messages();
    void retract_selected_messages();
    void show_message_menu(QPoint position);
    void connect_chat_controls();

    // A send owns its text or file and its reply from the moment it is made (docs/outgoing-messages-design.md).
    download_tasks* downloads_ = nullptr;
    struct outgoing
    {
        qint64 client_id = 0;
        qint64 conversation = 0;
        QString text;
        // The SDK reads the original file one part at a time.
        chat::text_options options{};
        QString filename;
        QString path;
        qint64 size = 0;
        quoted_message_data reply;
        qint64 created = 0;
        bool group_only = false;
        bool failed = false;
        QString error;
        bool waiting = false;
        bool unavailable = false;
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
    QString folder_settings_key_;

    void edit_folder_name(qint64 folder, QString current = {});
    void folder_menu(QPoint point, QModelIndex const& index);
    void toggle_folder(qint64 folder);
    void update_author(qint64 user, QString const& name);
    void filter_conversations();
    void set_list_status(QString text);
    void update_main_menu();
    void show_loading_status();
    void set_messages_loading(bool loading);
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
    void update_reading_navigation();
    void resume_reading_after_menu(QMenu& menu);
    void request_newer_messages();
    void navigate_down();
    quint64 conversation_unread() const;
    bool reading_enabled() const;
    qint64 effective_read_position() const;
    void scroll_to_unread(QModelIndex const& index);
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
    QToolButton* screenshot_button_ = nullptr;
    QToolButton* mention_button_ = nullptr;
    QToolButton* mention_all_button_ = nullptr;
    QAction* create_poll_action_ = nullptr;
    QToolButton* markdown_button_ = nullptr;
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
    QLabel* empty_conversation_hint_ = nullptr;
    QStackedWidget* chat_headers_ = nullptr;
    QWidget* selection_bar_ = nullptr;
    QLabel* selection_count_ = nullptr;
    QToolButton* selection_retract_ = nullptr;
    QToolButton* selection_copy_ = nullptr;
    QToolButton* selection_forward_ = nullptr;
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
        chat::text_options options{};
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
    friend_list_model* friends_list_ = nullptr;
    user_model* add_users_ = nullptr;
    message_model* messages_ = nullptr;
    QHash<qint64, presence_data> presence_;
    qint64 self_user_ = 0;
    QString self_username_;
    qint64 active_conversation_ = 0;
    qint64 active_peer_ = 0;
    bool active_group_ = false;
    QString active_username_;
    QPushButton* history_down_ = nullptr;
    quint64 conversation_visit_ = 0;
    qint64 observed_tail_ = 0;
    qint64 requested_read_ = 0;
    bool tail_loaded_ = false;

    bool messages_loaded_ = false;
    struct reading_position
    {
        qint64 message = 0;
        std::optional<int> offset = 0;
        qint64 client_id = 0;
    };
    QHash<qint64, reading_position> reading_positions_;
    struct history_target
    {
        history_page page;
        reading_position position{};
    };
    std::optional<history_target> history_target_;
    void request_navigation(history_target target);
    QList<reading_position> reply_returns_;
    void request_entry_history();
    void request_position(reading_position position);
    reading_position capture_reading_position() const;
    void restore_reading_position(reading_position position);
    void preserve_reading(const std::function<void()>& change);
    bool changing_layout_ = false;
    bool messages_loading_ = false;
    bool history_exhausted_ = false;
    bool control_enter_send_ = false;
    bool connection_available_ = true;
};

#endif
