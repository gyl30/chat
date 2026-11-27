#ifndef CHAT_QT_SRC_LOGIN_SESSIONS_DIALOG_HPP
#define CHAT_QT_SRC_LOGIN_SESSIONS_DIALOG_HPP

#include <QDialog>

class client_bridge;
class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QListWidgetItem;

class login_sessions_dialog final : public QDialog
{
   public:
    explicit login_sessions_dialog(client_bridge& client, QWidget* parent);

   private:
    void refresh();
    void update_actions();
    void revoke(QListWidgetItem const& item);

    client_bridge& client_;
    QListWidget* sessions_;
    QLabel* status_;
    QDialog* password_dialog_;
    QLabel* password_status_;
    QPushButton* password_entry_;
    QPushButton* revoke_others_;
    QPushButton* change_password_;
    QLineEdit* current_password_;
    QLineEdit* new_password_;
    QLineEdit* repeat_password_;
    quint64 request_ = 0;
    bool pending_ = false;
};

#endif
