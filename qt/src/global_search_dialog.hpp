#ifndef CHAT_QT_SRC_GLOBAL_SEARCH_DIALOG_HPP
#define CHAT_QT_SRC_GLOBAL_SEARCH_DIALOG_HPP

#include <QDialog>

namespace chat
{
struct message_search_page;
}
struct message_data;
class client_bridge;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QListWidget;
class QPushButton;
class QLabel;

class global_search_dialog final : public QDialog
{
   public:
    global_search_dialog(client_bridge& client, QWidget* parent);

   private:
    void search(qint64 before);
    void open_context(QTreeWidgetItem* item);
    void show_results();
    void set_page(quint64 request, chat::message_search_page const& page, QString const& error);
    void set_context(quint64 request, qint64 anchor, QList<message_data> const& messages, QString const& error);
    void refresh(qint64 conversation, bool lost_access);

    client_bridge& client_;
    QLineEdit* query_;
    QTreeWidget* results_;
    QListWidget* context_;
    QPushButton* more_;
    QPushButton* back_;
    QLabel* status_;
    bool results_stale_ = false;
    quint64 request_ = 0;
    qint64 next_before_ = 0;
};

#endif
