#ifndef CHAT_QT_SRC_GROUP_FILES_PAGE_HPP
#define CHAT_QT_SRC_GROUP_FILES_PAGE_HPP

#include <QTimer>
#include <QWidget>

class chat_widget;
class QToolButton;
class client_bridge;
class QTabBar;
class empty_state;
class QLabel;
class QLineEdit;
class QPushButton;
class QListWidget;
class QListWidgetItem;

class group_files_page final : public QWidget
{
    Q_OBJECT
   public:
    group_files_page(client_bridge& client, qint64 conversation, qint64 user, QWidget* parent);
    void bind_uploads(chat_widget& chat);
    void set_access(bool member, bool manager, bool can_publish);
    void refresh();
    void show_files();
    QWidget* header_actions() const { return header_actions_; }
   signals:
    void upload_requested(bool group_only);

   private:
    void request_page(bool older);
    void reset_files();
    void activate(QListWidgetItem const& item);
    void show_menu(QPoint position);
    void set_status(QString const& text, bool empty_result = false);
    void update_status_layout();
    QListWidgetItem* find_file(QString const& key) const;
    void download(QListWidgetItem const& item, bool open);

    client_bridge& client_;
    qint64 conversation_;
    qint64 user_;
    chat_widget* chat_ = nullptr;
    QWidget* header_actions_;
    QToolButton* upload_;
    void refresh_uploads();
    bool manager_ = false;
    quint64 request_ = 0;
    QTimer search_delay_;
    QTabBar* category_;
    QLineEdit* search_;
    QListWidget* list_;
    QLabel* status_;
    empty_state* empty_;
    QPushButton* more_;
};
#endif
