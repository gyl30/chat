#ifndef CHAT_QT_SRC_GLOBAL_SEARCH_DIALOG_HPP
#define CHAT_QT_SRC_GLOBAL_SEARCH_DIALOG_HPP

#include <QDialog>
#include <QHash>

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
    void discard(qint64 conversation, qint64 through);
    void updated(qint64 conversation, message_data const& message, QString const& error);

    client_bridge& client_;
    QLineEdit* query_;
    QTreeWidget* results_;
    QListWidget* context_;
    QPushButton* more_;
    QPushButton* back_;
    QLabel* status_;
    // Deltas received while a results page is in flight also apply to that page.
    QHash<qint64, QString> page_edits_;
    QHash<qint64, qint64> page_floors_;
    quint64 request_ = 0;
    qint64 next_before_ = 0;
};

#endif
