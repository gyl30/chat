#ifndef CHAT_QT_SRC_MAIN_WINDOW_HPP
#define CHAT_QT_SRC_MAIN_WINDOW_HPP

#include <memory>

#include <QMainWindow>
#include <QString>
#include <QStringList>
#if defined(Q_OS_LINUX)
#include <QSet>
#include <QAbstractNativeEventFilter>
#endif
#include "message_data.hpp"

class QAction;
class QDialog;
class QCloseEvent;
class settings_dialog;
class QLabel;
class feedback_label;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;
class QWidget;
class QSystemTrayIcon;
class chat_widget;
class title_bar;
class client_bridge;

class main_window final : public QMainWindow
#if defined(Q_OS_LINUX)
    , private QAbstractNativeEventFilter
#endif
{
    Q_OBJECT
   public:
    explicit main_window(QString server_url, QWidget* parent = nullptr);
    ~main_window() override;

   signals:
    void notification_requested(qint64 conversation, QString title, QString summary);

   protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    // Unsent messages are lost on quitting: asks first when there are any.
    bool confirm_quit();
    bool eventFilter(QObject* object, QEvent* event) override;
#if defined(Q_OS_LINUX)
    bool nativeEventFilter(QByteArray const& event_type, void* message, qintptr* result) override;
#endif

   private slots:
#if defined(Q_OS_LINUX)
    void desktop_notification_activated(uint id, QString const& action);
    void desktop_notification_closed(uint id, uint reason);
#endif

   private:
    enum class pending_action
    {
        none,
        login,
        registration,
        reconnect,
    };

    void start_login();
    void show_registration_dialog();
    void start_registration();
    void authenticate();
    void register_user();
    void logout();
    void show_settings();
    void set_login_busy(bool busy);
    void set_registration_busy(bool busy);
    void show_login_error(QString message);
    void show_registration_error(QString message);
    void show_authenticated_page(qint64 user);
    void begin_reconnect();
    void reconnect_now();
    void schedule_reconnect();
    void update_reconnect_status();
    void finish_reconnect();
    void stop_reconnect();
    void return_to_login(QString message);
    bool reading_conversation(qint64 conversation) const;
    void notify_message(message_data const& message);
    void show_desktop_notification(QString const& title, QString const& summary);
    void clear_desktop_notifications();
    void restore_window();
    QStringList recent_accounts() const;
    void remember_login(QString const& username, QString const& server);
    void update_login_identity();
    void update_window_chrome();
    void place_resize_grips();

    QWidget* frame_ = nullptr;
    title_bar* title_bar_ = nullptr;
    QList<QWidget*> resize_grips_;
    QStackedWidget* pages_ = nullptr;
    QWidget* login_page_ = nullptr;
    chat_widget* chat_page_ = nullptr;
    QLineEdit* server_edit_ = nullptr;
    QLineEdit* username_edit_ = nullptr;
    QLineEdit* password_edit_ = nullptr;
    QLabel* login_avatar_ = nullptr;
    QAction* recent_accounts_action_ = nullptr;
    QToolButton* server_settings_ = nullptr;
    QPushButton* login_button_ = nullptr;
    QPushButton* register_button_ = nullptr;
    feedback_label* status_label_ = nullptr;
    QDialog* registration_dialog_ = nullptr;
    QLineEdit* registration_username_edit_ = nullptr;
    QLineEdit* registration_password_edit_ = nullptr;
    QLineEdit* registration_password_confirm_edit_ = nullptr;
    QPushButton* registration_submit_button_ = nullptr;
    QPushButton* registration_cancel_button_ = nullptr;
    feedback_label* registration_status_label_ = nullptr;
    QTimer* reconnect_timer_ = nullptr;
    QTimer* reconnect_countdown_timer_ = nullptr;
    QTimer* reconnect_notice_timer_ = nullptr;
    QTimer* reconnect_recovered_timer_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    settings_dialog* settings_ = nullptr;
    bool desktop_notifications_ = true;
    bool message_preview_ = true;
    bool close_to_tray_ = false;
    // The tray's Quit already asked about unsent messages; closing the window then does not ask again.
    bool quit_confirmed_ = false;
    QList<message_data> pending_notifications_;
#if defined(Q_OS_LINUX)
    QSet<uint> desktop_notification_ids_;
    QString desktop_notification_owner_;
#endif

    bool connected_ = false;
    bool logout_pending_ = false;
    bool reconnecting_ = false;
    bool reconnect_notice_visible_ = false;
    int reconnect_attempt_ = 0;
    int reconnect_seconds_left_ = 0;
    pending_action pending_action_ = pending_action::none;
    QString pending_username_;
    QString pending_password_;
    QString session_server_;
    QString session_username_;
    QString session_password_;

    std::unique_ptr<client_bridge> client_;
};

#endif
