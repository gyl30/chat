#ifndef CHAT_QT_SRC_GROUP_DIALOG_HPP
#define CHAT_QT_SRC_GROUP_DIALOG_HPP

#include <QDialog>
#include <QPointer>
#include <optional>
#include "member_data.hpp"
#include "conversation_data.hpp"
#include "user_data.hpp"

class avatar_cache;
class client_bridge;
class chat_widget;
class group_files_page;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QListWidget;
class QPushButton;
class QCheckBox;
class QStackedWidget;
class QToolButton;
class empty_state;
class QTimer;
class QScrollArea;

class QVBoxLayout;

class group_dialog final : public QDialog
{
    Q_OBJECT

   public:
    group_dialog(client_bridge& client,
                 qint64 conversation,
                 qint64 self_user,
                 QString const& title,
                 QString const& announcement,
                 bool join_approval,
                 QWidget* parent,
                 chat_widget& chat);
    void open_requests_on_load(bool requests) { open_requests_ = requests; }
    void set_members(qint64 conversation, QList<member_data> members, QString const& error);
    void finish_action(qint64 conversation, bool left, QString const& error);
    void set_contacts(QList<user_data> contacts, QString const& error);
    void set_conversations(QList<conversation_data> conversations, QString const& error);
    void discard_history(qint64 conversation, qint64 through);
    void set_error(QString const& error);
    void set_invite(qint64 conversation, QString const& token, QString const& error);
    void set_requests(qint64 conversation, QList<user_data> users, qint64 next, bool older, QString const& error);
    void refresh_requests();
    void refresh_notifications();
    void refresh_member_status();

   signals:
    void upload_requested(bool group_only);
    void history_requested(int category);
    void pinned_requested(qint64 message);
    void notification_requested(bool mute);
    void user_requested(qint64 user, QString username);
    void admin_requested(qint64 user, bool admin);
    void rename_requested(QString title);
    void description_requested(QString text);
    void group_avatar_requested(QByteArray data);
    void announcement_requested(QString text);
    void invite_requested(QList<qint64> members);
    void invite_link_requested(std::optional<bool> create);
    void approval_requested(bool required);
    void requests_requested(qint64 before);
    void request_response_requested(qint64 user, bool accept);
    void leave_requested();
    void transfer_requested(qint64 user);
    void remove_requested(qint64 user);
    void member_mute_requested(qint64 user, qint64 seconds);
    void group_mute_requested(bool muted);
    void dissolve_requested();

   protected:
    bool eventFilter(QObject* object, QEvent* event) override;

   private:
    void setup_overview(client_bridge& client);
    void setup_management(bool join_approval);
    void setup_members(QVBoxLayout* overview);
    void setup_join_requests();
    void connect_member_actions();
    void connect_approval_actions();
    void connect_group_settings();

    void filter_members();
    void refresh_speech();
    void update_actions();
    void update_title();
    void show_page(QWidget* page);

    bool open_requests_ = false;
    group_files_page* files_;
    chat_widget& chat_;
    avatar_cache* avatars_;
    qint64 conversation_;
    qint64 self_user_;
    qint64 cleared_through_ = 0;
    qint64 pin_id_ = 0;
    QList<member_data> members_;
    QList<user_data> contacts_;
    QString title_;
    QString announcement_;
    bool contacts_ready_ = false;
    bool pending_ = false;
    bool available_ = false;
    bool group_muted_ = false;
    qint64 own_speech_until_ = 0;
    QTimer* speech_expiry_;
    qint64 server_clock_delta_ = 0;
    QListWidget* list_;
    QPushButton* admin_button_;
    QPushButton* rename_button_;
    QToolButton* invite_button_;
    QWidget* details_;
    QToolButton* leave_button_;
    QPushButton* transfer_button_;
    QPushButton* remove_button_;
    QPushButton* member_mute_button_;
    QPushButton* dissolve_button_;
    QCheckBox* all_mute_;
    QLineEdit* title_edit_;
    QPlainTextEdit* announcement_edit_;
    QPushButton* announcement_button_;
    QPushButton* clear_announcement_button_;
    QWidget* invite_controls_;
    QLineEdit* invite_edit_;
    QPushButton* create_invite_button_;
    QPushButton* copy_invite_button_;
    QPushButton* revoke_invite_button_;
    QCheckBox* approval_;
    QStackedWidget* pages_;
    QPointer<QWidget> overview_focus_;
    QScrollArea* overview_scroll_;
    QWidget* requests_page_;
    QToolButton* back_;
    QToolButton* notification_;
    QToolButton* management_button_;
    QPushButton* requests_button_;
    QLabel* description_heading_;
    QLabel* announcement_heading_;
    QScrollArea* management_scroll_;
    QListWidget* requests_;
    QPushButton* accept_request_button_;
    QPushButton* reject_request_button_;
    QPushButton* more_requests_button_;
    qint64 next_request_ = 0;
    QLabel* status_;
    QLabel* overview_title_;
    QLabel* overview_avatar_;
    QLabel* overview_count_;
    QLabel* description_;
    QPushButton* edit_description_;
    QPushButton* change_avatar_;
    QPushButton* clear_avatar_;
    QLineEdit* member_search_;
    QLabel* members_heading_;
    empty_state* members_empty_;
    QLabel* overview_announcement_;
    QPushButton* read_announcement_button_;
    QPushButton* overview_pin_;
    QLabel* overview_invite_;
    QPushButton* overview_copy_invite_ = nullptr;
    bool copy_after_create_ = false;
    QLabel* requests_status_;
};

#endif
