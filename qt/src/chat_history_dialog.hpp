#ifndef CHAT_QT_SRC_CHAT_HISTORY_DIALOG_HPP
#define CHAT_QT_SRC_CHAT_HISTORY_DIALOG_HPP

#include <QDialog>
#include <QList>
#include <QHash>

#include <chat/message.hpp>

#include "message_data.hpp"

class avatar_cache;
class message_images;
class download_tasks;
class QLabel;
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
    using category = chat::message_category;

    chat_history_dialog(qint64 conversation,
                        QString const& title,
                        QWidget* parent,
                        avatar_cache* avatars = nullptr,
                        message_images* images = nullptr,
                        download_tasks* downloads = nullptr);
    void set_history(qint64 conversation, QList<message_data> messages, bool has_more, QString const& error);
    // Requests the newest page; call once the request signals are connected.
    void start(category initial = category::all);
    void update_message(qint64 conversation, message_data message, QString const& error);
    void interrupt_loading();

   signals:
    void history_requested(chat::message_category category, QString query, qint64 before);
    void message_activated(qint64 message);
    void image_requested(qint64 message, QString filename);

   private:
    void restart_search(QString query);
    void request_more();
    void update_status();
    bool matches(message_data const& message) const;
    void add_messages(QList<message_data> messages, bool has_more);
    void show_menu(QPoint position);
    void activate(QListWidgetItem const& item);
    void set_row(QListWidgetItem& item, message_data const& message, bool starts_day);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void observe_visible_images();

    qint64 conversation_;
    message_images* images_;
    download_tasks* downloads_;
    QTabBar* tabs_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* more_ = nullptr;
    QString query_;
    // Keep the server boundary even if the matching row disappears locally.
    qint64 before_ = 0;
    QHash<qint64, message_data> page_updates_;
    // Newest first, with a corresponding row at each index.
    QList<message_data> messages_;
    bool has_more_ = true;
    bool loading_ = false;
};

#endif
