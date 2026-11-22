#include "download_dialog.hpp"

#include <QDesktopServices>
#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "client_bridge.hpp"
#include "chat_widget.hpp"

download_dialog::download_dialog(client_bridge& client, chat_widget& chat, QWidget* parent) : QDialog(parent), client_(client), chat_(chat)
{
    setObjectName(QStringLiteral("downloadTasksDialog"));
    setWindowTitle(QStringLiteral("下载任务"));
    resize(800, 380);
    auto* layout = new QVBoxLayout(this);
    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("downloadTaskList"));
    list_->setRootIsDecorated(false);
    list_->setHeaderLabels(
        {QStringLiteral("文件"), QStringLiteral("来源"), QStringLiteral("保存位置"), QStringLiteral("进度"), QStringLiteral("状态")});
    list_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(list_);
    auto* actions = new QHBoxLayout;
    cancel_ = new QPushButton(QStringLiteral("取消"), this);
    retry_ = new QPushButton(QStringLiteral("重试"), this);
    open_ = new QPushButton(QStringLiteral("打开文件"), this);
    folder_ = new QPushButton(QStringLiteral("打开所在目录"), this);
    actions->addWidget(cancel_);
    actions->addWidget(retry_);
    actions->addStretch();
    actions->addWidget(open_);
    actions->addWidget(folder_);
    layout->addLayout(actions);
    connect(list_, &QTreeWidget::itemSelectionChanged, this, &download_dialog::update_actions);
    connect(cancel_, &QPushButton::clicked, this, &download_dialog::cancel);
    connect(retry_, &QPushButton::clicked, this, &download_dialog::retry);
    connect(open_, &QPushButton::clicked, this, [this] { open(false); });
    connect(folder_, &QPushButton::clicked, this, [this] { open(true); });
    connect(&client_, &client_bridge::download_requested, this, &download_dialog::enqueue);
    connect(&client_, &client_bridge::download_progress, this, &download_dialog::progress);
    connect(&client_, &client_bridge::download_finished, this, &download_dialog::finish);
    update_actions();
}

void download_dialog::enqueue(qint64 conversation, qint64 message, QString key, QString filename, QString path, bool open)
{
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (std::ranges::any_of(tasks_, [&](task const& item) { return item.status == state::running && item.path == path; }))
    {
        auto error = QStringLiteral("已有下载正在使用此保存位置");
        if (message)
        {
            emit client_.attachment_received(conversation, message, path, error);
        }
        else
        {
            emit client_.file_downloaded(conversation, path, false, error);
        }
        return;
    }
    auto const id = chat::client::new_client_id();
    auto const room = chat_.conversation(conversation);
    auto const name = room && !room->username.isEmpty() ? room->username : QStringLiteral("会话 #%1").arg(conversation);
    auto const source = message ? QStringLiteral("%1 · 消息 #%2").arg(name).arg(message) : QStringLiteral("%1 · 群文件").arg(name);
    auto* row = new QTreeWidgetItem(list_, {filename, source, path, {}, {}});
    row->setData(0, Qt::UserRole, static_cast<qint64>(id));
    row->setToolTip(2, path);
    auto it = tasks_.insert(id, {conversation, message, std::move(key), path, open, QFileInfo::exists(path), 0, state::running, {}, row});
    list_->setCurrentItem(row);
    start(id, it.value());
}

void download_dialog::start(qint64 id, task& value)
{
    value.control = std::make_shared<chat::download_control>();
    value.status = state::running;
    value.row->setText(3, {});
    value.row->setText(4, QStringLiteral("正在申请下载…"));
    value.row->setToolTip(4, {});
    client_.start_download(id, ++value.attempt, value.conversation, value.message, value.key, value.path, value.control, value.replace);
    update_actions();
}

void download_dialog::finish(qint64 id, quint64 attempt, QString const& error)
{
    auto it = tasks_.find(id);
    if (it == tasks_.end() || attempt != it->attempt || it->status != state::running)
    {
        return;
    }
    it->status = error.isEmpty() ? state::completed : state::failed;
    it->row->setText(4, error.isEmpty() ? QStringLiteral("已完成") : QStringLiteral("失败：%1").arg(error));
    it->row->setToolTip(4, error);
    if (it->message)
    {
        emit client_.attachment_received(it->conversation, it->message, it->path, error);
    }
    else
    {
        emit client_.file_downloaded(it->conversation, it->path, it->open, error);
    }
    update_actions();
}

void download_dialog::progress(qint64 id, quint64 attempt, qint64 bytes, qint64 total)
{
    auto it = tasks_.find(id);
    if (it == tasks_.end() || attempt != it->attempt || it->status != state::running)
    {
        return;
    }
    QLocale const locale;
    it->row->setText(3, QStringLiteral("%1 / %2").arg(locale.formattedDataSize(bytes), locale.formattedDataSize(total)));
    it->row->setText(4, QStringLiteral("下载中"));
}

qint64 download_dialog::selected() const
{
    auto const* row = list_->currentItem();
    return row ? row->data(0, Qt::UserRole).toLongLong() : 0;
}

void download_dialog::cancel()
{
    auto it = tasks_.find(selected());
    if (it == tasks_.end() || it->status != state::running || !it->control->cancel())
    {
        return;
    }
    it->status = state::cancelled;
    auto const status = QStringLiteral("已取消");
    it->row->setText(4, status);
    if (it->message)
    {
        emit client_.attachment_received(it->conversation, it->message, it->path, status);
    }
    else
    {
        emit client_.file_downloaded(it->conversation, it->path, false, status);
    }
    update_actions();
}

void download_dialog::retry()
{
    auto const id = selected();
    auto it = tasks_.find(id);
    if (it == tasks_.end() || it->status != state::failed ||
        std::ranges::any_of(tasks_, [&](task const& item) { return item.status == state::running && item.path == it->path; }))
    {
        return;
    }
    auto const path = it->path;
    auto const replace = QFileInfo::exists(path);
    if (replace && QMessageBox::question(this, QStringLiteral("覆盖文件"), QStringLiteral("“%1”已存在，是否覆盖？").arg(path)) != QMessageBox::Yes)
    {
        return;
    }
    it = tasks_.find(id);
    if (it != tasks_.end() && it->status == state::failed &&
        !std::ranges::any_of(tasks_, [&](task const& item) { return item.status == state::running && item.path == path; }))
    {
        it->replace = replace;
        start(id, it.value());
    }
}

void download_dialog::open(bool directory)
{
    auto const it = tasks_.constFind(selected());
    if (it != tasks_.end() && it->status == state::completed)
    {
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory ? QFileInfo(it->path).absolutePath() : it->path));
    }
}

void download_dialog::update_actions()
{
    auto const it = tasks_.constFind(selected());
    auto const valid = it != tasks_.end();
    cancel_->setEnabled(valid && it->status == state::running);
    retry_->setEnabled(valid && it->status == state::failed);
    open_->setEnabled(valid && it->status == state::completed);
    folder_->setEnabled(open_->isEnabled());
}

void download_dialog::clear_tasks()
{
    for (auto& item : tasks_)
    {
        if (item.status == state::running)
        {
            item.control->cancel();
        }
    }
    tasks_.clear();
    list_->clear();
    hide();
}
