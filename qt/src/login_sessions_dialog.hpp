#ifndef CHAT_QT_SRC_LOGIN_SESSIONS_DIALOG_HPP
#define CHAT_QT_SRC_LOGIN_SESSIONS_DIALOG_HPP

#include <QDialog>

class client_bridge;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

class login_sessions_dialog final : public QDialog
{
   public:
    explicit login_sessions_dialog(client_bridge& client, QWidget* parent);

   private:
    void refresh();
    void update_actions();

    client_bridge& client_;
    QTreeWidget* sessions_;
    QLabel* status_;
    QPushButton* revoke_;
    QPushButton* revoke_others_;
    QPushButton* change_password_;
    QLineEdit* current_password_;
    QLineEdit* new_password_;
    QLineEdit* repeat_password_;
    quint64 request_ = 0;
    bool pending_ = false;
};

#endif
