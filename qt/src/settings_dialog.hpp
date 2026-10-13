#ifndef CHAT_QT_SRC_SETTINGS_DIALOG_HPP
#define CHAT_QT_SRC_SETTINGS_DIALOG_HPP

#include <QDialog>
#include <QIcon>

class QCheckBox;
class QComboBox;
class QKeyEvent;
class QLabel;
class QStackedWidget;
class QToolButton;

class settings_dialog final : public QDialog
{
    Q_OBJECT
   public:
    explicit settings_dialog(QWidget* parent = nullptr);
    void set_account(QString username, QIcon avatar);
    void set_options(bool desktop, bool preview, bool control_enter, bool close_to_tray, bool tray_available);

   signals:
    void account_requested();
    void logout_requested();
    void desktop_notifications_changed(bool enabled);
    void message_preview_changed(bool enabled);
    void control_enter_changed(bool enabled);
    void close_to_tray_changed(bool enabled);

   protected:
    void keyPressEvent(QKeyEvent* event) override;

   private:
    void show_page(int page);
    void refresh_icons();

    QStackedWidget* pages_ = nullptr;
    QLabel* title_ = nullptr;
    QToolButton* back_ = nullptr;
    QToolButton* close_ = nullptr;
    QToolButton* account_ = nullptr;
    QLabel* account_avatar_ = nullptr;
    QLabel* account_name_ = nullptr;
    QCheckBox* desktop_ = nullptr;
    QCheckBox* preview_ = nullptr;
    QCheckBox* control_enter_ = nullptr;
    QCheckBox* close_to_tray_ = nullptr;
    QLabel* tray_unavailable_ = nullptr;
    QComboBox* appearance_ = nullptr;
    QLabel* repository_ = nullptr;
};

#endif
