#include "theme.hpp"
#include "download_tasks.hpp"
#include "client_bridge.hpp"
#include <algorithm>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMenu>
#include <QToolButton>
#include <QUrl>

download_tasks::download_tasks(client_bridge& client) : QObject(&client), client_(client)
{
    connect(&client_, &client_bridge::download_requested, this, &download_tasks::enqueue);
    connect(&client_, &client_bridge::download_progress, this, &download_tasks::progress);
    connect(&client_, &client_bridge::download_finished, this, &download_tasks::finish);
}
QString download_tasks::task::description() const
{
    switch (status)
    {
        case state::running:
            return progress.isEmpty() ? QStringLiteral("正在申请下载…") : QStringLiteral("下载中");
        case state::completed:
            return QStringLiteral("已完成");
        case state::replaced:
            return QStringLiteral("已被其他下载覆盖");
        case state::cancelled:
            return QStringLiteral("已取消");
        case state::failed:
            return QStringLiteral("失败：%1").arg(error);
    }
    return {};
}
QString download_tasks::summary(download_source const& source) const
{
    QStringList result;
    for (auto const& task : tasks_)
    {
        if (task.source == source)
        {
            result << (task.status == state::running && !task.progress.isEmpty() ? QStringLiteral("下载中 · ") + task.progress : task.description());
        }
    }
    return result.join(QStringLiteral(" · "));
}
QToolButton* download_tasks::source_button(QWidget* parent, std::function<download_source()> source)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("attachmentDownloadState"));
    button->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(button);
    button->setMenu(menu);
    auto update = [this, button, source]
    {
        auto label = summary(source());
        button->setVisible(!label.isEmpty());
        button->setText(label);
        button->setToolTip(label);
    };
    connect(this,
            &download_tasks::changed,
            button,
            [source, update](QList<download_source> const& sources)
            {
                if (sources.isEmpty() || sources.contains(source()))
                {
                    update();
                }
            });
    connect(menu,
            &QMenu::aboutToShow,
            button,
            [this, menu, source]
            {
                menu->clear();
                add_actions(*menu, source());
            });
    update();
    return button;
}
void download_tasks::add_actions(QMenu& menu, download_source const& source)
{
    for (auto it = tasks_.cbegin(); it != tasks_.cend(); ++it)
    {
        if (it->source != source)
        {
            continue;
        }
        auto* target = menu.addMenu(it->path + QStringLiteral(" · ") + it->description());
        auto id = it.key();
        target->menuAction()->setData(id);
        if (it->status == state::running)
        {
            target->addAction(QStringLiteral("取消"), this, [this, id] { cancel(id); });
        }
        if (it->status == state::failed)
        {
            target->addAction(QStringLiteral("重试"), this, [this, id, parent = menu.parentWidget()] { retry(id, parent); })
                ->setEnabled(!path_busy(it->path));
        }
        if (it->status == state::completed)
        {
            target
                ->addAction(QStringLiteral("打开文件"),
                            this,
                            [this, id]
                            {
                                auto current = tasks_.constFind(id);
                                if (current != tasks_.cend() && current->status == state::completed && !path_busy(current->path))
                                {
                                    QDesktopServices::openUrl(QUrl::fromLocalFile(current->path));
                                }
                            })
                ->setEnabled(!path_busy(it->path));
        }
        if (it->status == state::completed || it->status == state::replaced)
        {
            target->addAction(QStringLiteral("打开所在目录"),
                              this,
                              [path = it->path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
        }
    }
}
void download_tasks::enqueue(qint64 conversation, qint64 message, QString key, QString filename, QString path, bool open, qint64 item)
{
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (path_busy(path))
    {
        auto error = QStringLiteral("已有下载正在使用此保存位置");
        if (message)
        {
            emit client_.attachment_received(conversation, message, path, error, item);
        }
        else
        {
            emit client_.file_downloaded(conversation, key, path, error);
        }
        return;
    }
    auto id = ++next_id_;
    auto it = tasks_.insert(
        id, {{conversation, message, item, std::move(key)}, std::move(filename), path, {}, {}, open, QFileInfo::exists(path), 0, state::running, {}});
    start(id, *it);
}
void download_tasks::start(qint64 id, task& value)
{
    value.control = std::make_shared<chat::download_control>();
    value.status = state::running;
    value.progress.clear();
    value.error.clear();
    const auto src = value.source;
    client_.start_download(id, ++value.attempt, src.conversation, src.message, src.key, value.path, value.control, value.replace, src.item);
    emit changed({src});
}
void download_tasks::finish(qint64 id, quint64 attempt, QString const& error)
{
    auto it = tasks_.find(id);
    if (it == tasks_.end() || attempt != it->attempt || it->status != state::running)
    {
        return;
    }
    it->status = error.isEmpty() ? state::completed : state::failed;
    it->error = error;
    QList<download_source> affected{it->source};
    if (error.isEmpty())
    {
        for (auto& other : tasks_)
        {
            if (&other != &it.value() && other.status == state::completed && other.path == it->path)
            {
                other.status = state::replaced;
                if (!affected.contains(other.source))
                {
                    affected.append(other.source);
                }
            }
        }
    }
    if (it->source.message)
    {
        emit client_.attachment_received(it->source.conversation, it->source.message, it->path, error, it->source.item);
    }
    else
    {
        auto result = error;
        if (result.isEmpty() && it->open && !QDesktopServices::openUrl(QUrl::fromLocalFile(it->path)))
        {
            result = QStringLiteral("文件已保存，系统未能打开它");
        }
        emit client_.file_downloaded(it->source.conversation, it->source.key, it->path, result);
    }
    emit changed(affected);
}
void download_tasks::progress(qint64 id, quint64 attempt, qint64 bytes, qint64 total)
{
    auto it = tasks_.find(id);
    if (it == tasks_.end() || attempt != it->attempt || it->status != state::running)
    {
        return;
    }
    const QLocale locale;
    auto progress = QStringLiteral("%1 / %2").arg(locale.formattedDataSize(bytes), locale.formattedDataSize(total));
    if (it->progress != progress)
    {
        it->progress = std::move(progress);
        emit changed({it->source});
    }
}
bool download_tasks::path_busy(QString const& path) const
{
    return std::ranges::any_of(tasks_, [&](task const& t) { return t.path == path && t.status == state::running; });
}
void download_tasks::cancel(qint64 id)
{
    auto it = tasks_.find(id);
    if (it == tasks_.end() || it->status != state::running || !it->control->cancel())
    {
        return;
    }
    it->status = state::cancelled;
    const auto source = it->source;
    auto status = it->description();
    if (it->source.message)
    {
        emit client_.attachment_received(it->source.conversation, it->source.message, it->path, status, it->source.item);
    }
    else
    {
        emit client_.file_downloaded(it->source.conversation, it->source.key, it->path, status);
    }
    emit changed({source});
}
void download_tasks::retry(qint64 id, QWidget* parent)
{
    auto it = tasks_.find(id);
    if (it == tasks_.end() || it->status != state::failed || path_busy(it->path))
    {
        return;
    }
    auto path = it->path;
    auto replace = QFileInfo::exists(path);
    if (replace && !confirm_action(parent, QStringLiteral("覆盖文件"), QStringLiteral("“%1”已存在，是否覆盖？").arg(path), QStringLiteral("覆盖")))
    {
        return;
    }
    it = tasks_.find(id);
    if (it != tasks_.end() && it->status == state::failed && !path_busy(path))
    {
        it->replace = replace;
        start(id, *it);
    }
}
void download_tasks::clear_tasks()
{
    for (auto& item : tasks_)
    {
        if (item.status == state::running)
        {
            item.control->cancel();
        }
    }
    if (!tasks_.isEmpty())
    {
        tasks_.clear();
        emit changed({});
    }
}
