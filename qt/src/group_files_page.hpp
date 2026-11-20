#ifndef CHAT_QT_SRC_GROUP_FILES_PAGE_HPP
#define CHAT_QT_SRC_GROUP_FILES_PAGE_HPP

#include <QWidget>

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
    void set_access(bool member, bool manager);
    void refresh();
   signals:
    void upload_requested(bool group_only);

   private:
    void request_page(bool older);
    void update_selection();
    void download(bool open);

    client_bridge& client_;
    qint64 conversation_;
    qint64 user_;
    bool manager_ = false;
    QComboBox* category_;
    QLineEdit* search_;
    QTreeWidget* list_;
    QLabel* status_;
    QPushButton* more_;
    QPushButton* save_;
    QPushButton* open_;
    QPushButton* remove_;
};
#endif
