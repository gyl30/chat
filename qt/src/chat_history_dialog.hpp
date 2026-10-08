#ifndef CHAT_QT_SRC_CHAT_HISTORY_DIALOG_HPP
#define CHAT_QT_SRC_CHAT_HISTORY_DIALOG_HPP

#include <QDialog>
#include <QList>

#include "message_data.hpp"

class avatar_cache;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTabBar;

// A conversation's history in its own window, as in WeChat and QQ: browse every message from the
// newest backwards, narrow to images, files or links, search, and jump back to a message in the chat.
class chat_history_dialog final : public QDialog
{
    Q_OBJECT

   public:
    enum class category
    {
        all,
        images,
        files,
        links,
    };

    chat_history_dialog(qint64 conversation, QString const& title, QWidget* parent, avatar_cache* avatars = nullptr);
    void set_history(qint64 conversation, qint64 before, QList<message_data> messages, bool has_more,
                     QString const& error);
    void set_search_results(qint64 conversation, QString const& query, qint64 before, QList<message_data> messages,
                            read_positions positions, bool has_more, QString const& error);
    static bool matches(message_data const& message, category kind);
    // Requests the newest page; call once the request signals are connected.
    void start();

   signals:
    void history_requested(qint64 before);
    void search_requested(QString query, qint64 before);
    void message_activated(qint64 message);

   private:
    void request_more();
    void rebuild();
    void add_messages(QList<message_data> messages, bool has_more);
    void show_menu(QPoint position);
    void fill_sparse_category();

    qint64 conversation_;
    avatar_cache* avatars_;
    QLineEdit* search_ = nullptr;
    QTabBar* tabs_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* more_ = nullptr;
    QString query_;
    // Newest first; requests are paged with the oldest loaded message as the cursor.
    QList<message_data> messages_;
    bool has_more_ = true;
    bool loading_ = false;
    int filling_pages_ = 0;
};

#endif
