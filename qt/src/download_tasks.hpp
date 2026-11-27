#pragma once
#include "file_presentation.hpp"
#include <QObject>
#include <QMap>
#include <QString>
#include <chat/attachment.hpp>
#include <functional>
#include <memory>

class client_bridge;
class QMenu;
class QToolButton;
struct download_source
{
    qint64 conversation = 0;
    qint64 message = 0;
    qint64 item = 0;
    bool operator==(download_source const&) const = default;
};

// Downloads outlive their source widgets, but belong to just one logged-in client.
class download_tasks final : public QObject
{
    Q_OBJECT
   public:
    explicit download_tasks(client_bridge& client);
    void clear_tasks();
    QString summary(download_source const& source) const;
    file_transfer transfer(download_source const& source) const;
    QString saved_file(download_source const& source) const;
    bool activate(download_source const& source, QWidget* parent);
    void save(download_source const& source, QString const& filename, QWidget* parent, bool open = false);
    void add_actions(QMenu& menu, download_source const& source);
    QToolButton* source_button(QWidget* parent, std::function<download_source()> source);
   signals:
    // Empty sources denotes a cleared task list.
    void changed(QList<download_source> const& sources);

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
        download_source source;
        QString filename, path, progress, error;
        bool open, replace;
        quint64 attempt = 0;
        state status = state::running;
        std::shared_ptr<chat::download_control> control;
        int percent = 0;
        QString description() const;
    };
    QMap<qint64, task>::const_iterator primary(download_source const& source) const;
    bool available_file(task const& value) const;
    void enqueue(qint64 conversation, qint64 message, QString filename, QString path, bool open, qint64 item);
    void start(qint64 id, task& value);
    void finish(qint64 id, quint64 attempt, QString const& error);
    void progress(qint64 id, quint64 attempt, qint64 bytes, qint64 total);
    void cancel(qint64 id);
    void retry(qint64 id, QWidget* parent);
    bool path_busy(QString const& path) const;
    client_bridge& client_;
    qint64 next_id_ = 0;
    QMap<qint64, task> tasks_;
};
