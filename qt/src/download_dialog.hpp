#ifndef CHAT_QT_SRC_DOWNLOAD_DIALOG_HPP
#define CHAT_QT_SRC_DOWNLOAD_DIALOG_HPP

#include <QDialog>
#include <QHash>
#include <chat/attachment.hpp>

class client_bridge;
class chat_widget;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class download_dialog final : public QDialog
{
   public:
    download_dialog(client_bridge& client, chat_widget& chat, QWidget* parent);
    void clear_tasks();

   private:
    enum class state
    {
        running,
        completed,
        replaced,
        failed,
        cancelled
    };
    struct task
    {
        qint64 conversation;
        qint64 message;
        QString key, path;
        bool open;
        bool replace;
        quint64 attempt = 0;
        state status = state::running;
        std::shared_ptr<chat::download_control> control;
        QTreeWidgetItem* row = nullptr;
    };

    void enqueue(qint64 conversation, qint64 message, QString key, QString filename, QString path, bool open);
    void start(qint64 id, task& value);
    void finish(qint64 id, quint64 attempt, QString const& error);
    void progress(qint64 id, quint64 attempt, qint64 bytes, qint64 total);
    void cancel();
    void retry();
    void open(bool directory);
    void update_actions();
    qint64 selected() const;
    bool path_busy(QString const& path) const;

    client_bridge& client_;
    chat_widget& chat_;
    QTreeWidget* list_;
    QPushButton* cancel_;
    QPushButton* retry_;
    QPushButton* open_;
    QPushButton* folder_;
    QHash<qint64, task> tasks_;
};

#endif
