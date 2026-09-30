#ifndef CHAT_QT_SRC_MAIN_WINDOW_HPP
#define CHAT_QT_SRC_MAIN_WINDOW_HPP

#include <memory>

#include <QMainWindow>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QWidget;
class client_bridge;

class main_window final : public QMainWindow
{
   public:
    explicit main_window(QString server_url, QWidget* parent = nullptr);
    ~main_window() override;

   private:
    void start_login();
    void authenticate();
    void set_login_busy(bool busy);
    void show_login_error(QString message);

    QStackedWidget* pages_ = nullptr;
    QWidget* login_page_ = nullptr;
    QWidget* authenticated_page_ = nullptr;
    QLineEdit* server_edit_ = nullptr;
    QLineEdit* username_edit_ = nullptr;
    QLineEdit* password_edit_ = nullptr;
    QPushButton* login_button_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* authenticated_label_ = nullptr;

    bool connected_ = false;
    bool login_pending_ = false;
    QString pending_username_;
    QString pending_password_;

    std::unique_ptr<client_bridge> client_;
};

#endif
