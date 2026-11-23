#ifndef CHAT_QT_SRC_GLOBAL_SEARCH_DIALOG_HPP
#define CHAT_QT_SRC_GLOBAL_SEARCH_DIALOG_HPP

#include <QDialog>
#include <QHash>

#include <chat/message.hpp>

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
class QCheckBox;
class QComboBox;
class QDateEdit;

class global_search_dialog final : public QDialog
{
   public:
    global_search_dialog(client_bridge& client, QWidget* parent, qint64 self = 0, QString const& self_name = {});
    void locate(qint64 conversation, qint64 anchor);

   private:
    void search(qint64 before);
    void filter_changed();
    void open_context(QTreeWidgetItem* item);
    void show_context(qint64 conversation, qint64 anchor, QString const& title);
    void context_unavailable();
    void show_results();
    void show_empty_result();
    void set_page(quint64 request, chat::message_search_page const& page, QString const& error);
    void set_context(quint64 request, qint64 anchor, QList<message_data> const& messages, QString const& error);
    void discard(qint64 conversation, qint64 through);
    void remove_scope(qint64 conversation);
    void updated(qint64 conversation, message_data const& message, QString const& error);

    client_bridge& client_;
    QLineEdit* query_;
    QWidget* filters_;
    QComboBox* conversation_;
    QComboBox* category_;
    QComboBox* sender_;
    QDateEdit* start_date_;
    QDateEdit* end_date_;
    QCheckBox* start_enabled_;
    QCheckBox* end_enabled_;
    chat::message_search_filter submitted_filter_;
    quint64 scope_request_ = 0;
    quint64 sender_request_ = 0;
    bool submitted_ = false;
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
    qint64 context_conversation_ = 0;
    qint64 context_anchor_ = 0;
    bool standalone_ = false;
};

#endif
