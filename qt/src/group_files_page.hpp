#ifndef CHAT_QT_SRC_GROUP_FILES_PAGE_HPP
#define CHAT_QT_SRC_GROUP_FILES_PAGE_HPP

#include <QTimer>
#include <QWidget>

class chat_widget;
class QToolButton;
class client_bridge;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

class group_files_page final : public QWidget
{
    Q_OBJECT
   public:
    group_files_page(client_bridge& client, qint64 conversation, qint64 user, QWidget* parent);
    void bind_uploads(chat_widget& chat);
    void set_access(bool member, bool manager, bool can_publish);
    void refresh();
   signals:
    void upload_requested(bool group_only);

   private:
    void request_page(bool older);
    void reset_files();
    void update_selection();
    void download(bool open);

    client_bridge& client_;
    qint64 conversation_;
    qint64 user_;
    chat_widget* chat_ = nullptr;
    QToolButton* upload_actions_;
    QToolButton* download_actions_;
    void refresh_uploads();
    bool manager_ = false;
    quint64 request_ = 0;
    QTimer search_delay_;
    QComboBox* category_;
    QLineEdit* search_;
    QTreeWidget* list_;
    QLabel* status_;
    QPushButton* more_;
    QPushButton* upload_only_;
    QPushButton* upload_send_;
    QPushButton* save_;
    QPushButton* open_;
    QPushButton* remove_;
};
#endif
