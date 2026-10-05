#ifndef CHAT_QT_SRC_GROUP_DIALOG_HPP
#define CHAT_QT_SRC_GROUP_DIALOG_HPP

#include <QDialog>
#include <optional>
#include "member_data.hpp"
#include "conversation_data.hpp"
#include "user_data.hpp"

class avatar_cache;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QListWidget;
class QPushButton;
class QCheckBox;
class QTabWidget;

class group_dialog final : public QDialog
{
    Q_OBJECT

   public:
    group_dialog(qint64 conversation, qint64 self_user, QString const& title, QString const& announcement, bool join_approval,
                 QWidget* parent, avatar_cache* avatars = nullptr);
    void set_members(qint64 conversation, QList<member_data> members, QString const& error);
    void finish_action(qint64 conversation, bool left, QString const& error);
    void set_contacts(QList<user_data> contacts, QString const& error);
    void set_conversations(QList<conversation_data> conversations, QString const& error);
    void set_error(QString const& error);
    void set_invite(qint64 conversation, QString const& token, QString const& error);
    void set_requests(qint64 conversation, QList<user_data> users, qint64 next, bool older, QString const& error);
    void refresh_requests();

   signals:
    void user_requested(qint64 user, QString username);
    void admin_requested(qint64 user, bool admin);
    void rename_requested(QString title);
    void announcement_requested(QString text);
    void invite_requested(QList<qint64> members);
    void invite_link_requested(std::optional<bool> create);
    void approval_requested(bool required);
    void requests_requested(qint64 before);
    void request_response_requested(qint64 user, bool accept);
    void leave_requested();
    void transfer_requested(qint64 user);
    void remove_requested(qint64 user);

   protected:
    bool eventFilter(QObject* object, QEvent* event) override;

   private:
    void update_actions();

    avatar_cache* avatars_;
    qint64 conversation_;
    qint64 self_user_;
    QList<member_data> members_;
    QList<user_data> contacts_;
    QString title_;
    QString announcement_;
    bool contacts_ready_ = false;
    bool pending_ = false;
    bool available_ = false;
    QListWidget* list_;
    QPushButton* admin_button_;
    QPushButton* rename_button_;
    QPushButton* invite_button_;
    QPushButton* leave_button_;
    QPushButton* transfer_button_;
    QPushButton* remove_button_;
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
    QTabWidget* tabs_;
    QListWidget* requests_;
    QPushButton* accept_request_button_;
    QPushButton* reject_request_button_;
    QPushButton* more_requests_button_;
    qint64 next_request_ = 0;
    QLabel* status_;
    QLabel* overview_title_;
    QLabel* overview_avatar_;
    QLabel* overview_count_;
    QLabel* overview_announcement_;
    QPushButton* read_announcement_button_;
    QLabel* overview_pin_;
    QLabel* overview_invite_;
    QListWidget* preview_;
    QPushButton* manage_button_;
    QLabel* requests_status_;
};

#endif
