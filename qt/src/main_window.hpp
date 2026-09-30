#ifndef CHAT_QT_SRC_MAIN_WINDOW_HPP
#define CHAT_QT_SRC_MAIN_WINDOW_HPP

#include <memory>

#include <QMainWindow>
#include <QString>

class QDialog;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QWidget;
class chat_widget;
class client_bridge;

class main_window final : public QMainWindow
{
   public:
    explicit main_window(QString server_url, QWidget* parent = nullptr);
    ~main_window() override;

   private:
    enum class pending_action
    {
        none,
        login,
        registration,
    };

    void start_login();
    void show_registration_dialog();
    void start_registration();
    void authenticate();
    void register_user();
    void logout();
    void set_login_busy(bool busy);
    void set_registration_busy(bool busy);
    void show_login_error(QString message);
    void show_registration_error(QString message);
    void show_authenticated_page();

    QStackedWidget* pages_ = nullptr;
    QWidget* login_page_ = nullptr;
    chat_widget* chat_page_ = nullptr;
    QLineEdit* server_edit_ = nullptr;
    QLineEdit* username_edit_ = nullptr;
    QLineEdit* password_edit_ = nullptr;
    QPushButton* login_button_ = nullptr;
    QPushButton* register_button_ = nullptr;
    QLabel* status_label_ = nullptr;
    QDialog* registration_dialog_ = nullptr;
    QLineEdit* registration_username_edit_ = nullptr;
    QLineEdit* registration_password_edit_ = nullptr;
    QLineEdit* registration_password_confirm_edit_ = nullptr;
    QPushButton* registration_submit_button_ = nullptr;
    QPushButton* registration_cancel_button_ = nullptr;
    QLabel* registration_status_label_ = nullptr;

    bool connected_ = false;
    bool logout_pending_ = false;
    pending_action pending_action_ = pending_action::none;
    QString pending_username_;
    QString pending_password_;

    std::unique_ptr<client_bridge> client_;
};

#endif
