#ifndef CHAT_QT_SRC_BOOKMARKS_DIALOG_HPP
#define CHAT_QT_SRC_BOOKMARKS_DIALOG_HPP

#include <QDialog>

#include <chat/message.hpp>

class client_bridge;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTimer;

class bookmarks_dialog final : public QDialog
{
   public:
    explicit bookmarks_dialog(client_bridge& client, QWidget* parent);

   private:
    void refresh();
    void fetch(qint64 before);
    void set_page(quint64 request, chat::bookmark_page const& page, QString const& error);
    void update_actions();
    void locate();

    client_bridge& client_;
    QLineEdit* query_;
    QTreeWidget* results_;
    QPushButton* locate_;
    QPushButton* remove_;
    QPushButton* more_;
    QLabel* status_;
    QLabel* feedback_;
    QTimer* refresh_;
    quint64 request_ = 0;
    qint64 before_ = 0;
    qint64 removing_ = 0;
};

#endif
