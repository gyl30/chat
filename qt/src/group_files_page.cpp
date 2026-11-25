#include "group_files_page.hpp"
#include "client_bridge.hpp"
#include "theme.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
enum file_role
{
    key_role = Qt::UserRole,
    uploader_role,
    created_role,
};
QString category_name(std::string const& category)
{
    if (category == "image")
    {
        return QStringLiteral("图片");
    }
    if (category == "document")
    {
        return QStringLiteral("文档");
    }
    if (category == "archive")
    {
        return QStringLiteral("压缩包");
    }
    return QStringLiteral("其他");
}
}    // namespace
group_files_page::group_files_page(client_bridge& client, qint64 conversation, qint64 user, QWidget* parent)
    : QWidget(parent), client_(client), conversation_(conversation), user_(user)
{
    auto* layout = new QVBoxLayout(this);
    auto* filters = new QHBoxLayout;
    category_ = new QComboBox(this);
    category_->addItem(QStringLiteral("全部文件"), QString{});
    for (auto const* key : {"image", "document", "archive", "other"})
    {
        category_->addItem(category_name(key), QString::fromLatin1(key));
    }
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("搜索文件名"));
    search_->setClearButtonEnabled(true);
    auto* reload = new QPushButton(QStringLiteral("刷新"), this);
    filters->addWidget(category_);
    filters->addWidget(search_, 1);
    filters->addWidget(reload);
    layout->addLayout(filters);
    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("groupFileList"));
    list_->setHeaderLabels(
        {QStringLiteral("文件名"), QStringLiteral("类型"), QStringLiteral("大小"), QStringLiteral("上传者"), QStringLiteral("加入时间")});
    list_->setRootIsDecorated(false);
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(list_, 1);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    save_ = new QPushButton(QStringLiteral("下载"), this);
    open_ = new QPushButton(QStringLiteral("保存并打开"), this);
    remove_ = new QPushButton(QStringLiteral("移除群文件"), this);
    more_ = new QPushButton(QStringLiteral("加载更多"), this);
    for (auto* b : {save_, open_, remove_, more_})
    {
        actions->addWidget(b);
    }
    layout->addLayout(actions);
    auto* uploads = new QHBoxLayout;
    auto* upload = upload_only_ = new QPushButton(QStringLiteral("只上传到群文件"), this);
    auto* send = upload_send_ = new QPushButton(QStringLiteral("发送到聊天"), this);
    uploads->addWidget(upload);
    uploads->addWidget(send);
    layout->addLayout(uploads);
    connect(upload, &QPushButton::clicked, this, [this] { emit upload_requested(true); });
    connect(send, &QPushButton::clicked, this, [this] { emit upload_requested(false); });
    connect(reload, &QPushButton::clicked, this, &group_files_page::refresh);
    connect(search_, &QLineEdit::textChanged, this, &group_files_page::refresh);
    connect(category_, &QComboBox::currentIndexChanged, this, &group_files_page::refresh);
    connect(more_, &QPushButton::clicked, this, [this] { request_page(true); });
    connect(list_, &QTreeWidget::itemSelectionChanged, this, &group_files_page::update_selection);
    connect(save_, &QPushButton::clicked, this, [this] { download(false); });
    connect(open_, &QPushButton::clicked, this, [this] { download(true); });
    connect(remove_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const* item = list_->currentItem();
                if (!item)
                {
                    return;
                }
                auto const key = item->data(0, key_role).toString();
                auto const uploader = item->data(0, uploader_role).toLongLong();
                if (confirm_action(this,
                                   QStringLiteral("移除群文件"),
                                   QStringLiteral("从本群的文件列表移除“%1”？聊天消息和其他群的引用会保留。").arg(item->text(0)),
                                   QStringLiteral("移除群文件")) &&
                    isEnabled() && (manager_ || uploader == user_))
                {
                    client_.change_file(conversation_, key, false);
                }
            });
    connect(&client_,
            &client_bridge::files_received,
            this,
            [this](qint64 group, chat::conversation_file_page const& page, bool older, QString const& error)
            {
                if (group != conversation_ || !isEnabled())
                {
                    return;
                }
                if (!error.isEmpty())
                {
                    status_->setText(error);
                    more_->setEnabled(older);
                    return;
                }
                for (auto const& f : page.files)
                {
                    auto* item = new QTreeWidgetItem(list_,
                                                     {QString::fromStdString(f.filename),
                                                      category_name(f.category),
                                                      QLocale().formattedDataSize(f.size),
                                                      QString::fromStdString(f.username),
                                                      QLocale().toString(QDateTime::fromMSecsSinceEpoch(f.created_at), QLocale::ShortFormat)});
                    item->setToolTip(0, item->text(0));
                    item->setData(0, key_role, QString::fromStdString(f.object_key));
                    item->setData(0, created_role, static_cast<qint64>(f.created_at));
                    item->setData(0, uploader_role, static_cast<qint64>(f.uploader_id));
                }
                more_->setEnabled(page.has_more);
                status_->setText(list_->topLevelItemCount() ? QString{} : QStringLiteral("没有匹配的群文件"));
            });
    connect(&client_,
            &client_bridge::files_changed,
            this,
            [this](qint64 group, QString const& error)
            {
                if (group == conversation_ && isVisible())
                {
                    if (error.isEmpty())
                    {
                        refresh();
                    }
                    else
                    {
                        status_->setText(error);
                    }
                }
            });
    connect(&client_,
            &client_bridge::file_downloaded,
            this,
            [this](qint64 group, QString const& path, bool open, QString const& error)
            {
                if (group != conversation_ || !isEnabled())
                {
                    return;
                }
                if (!error.isEmpty())
                {
                    status_->setText(error);
                    return;
                }
                status_->setText(QStringLiteral("已保存到 %1").arg(path));
                if (open && !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                {
                    status_->setText(QStringLiteral("文件已保存，系统未能打开它"));
                }
            });
    setEnabled(false);
    update_selection();
    more_->setEnabled(false);
}
void group_files_page::set_access(bool member, bool manager, bool can_publish)
{
    bool const gained = member && !isEnabled();
    setEnabled(member);
    manager_ = manager;
    upload_only_->setEnabled(can_publish);
    upload_send_->setEnabled(can_publish);
    if (!member)
    {
        list_->clear();
    }
    update_selection();
    if (gained && isVisible())
    {
        refresh();
    }
}
void group_files_page::update_selection()
{
    auto const* item = list_->currentItem();
    save_->setEnabled(item != nullptr);
    open_->setEnabled(item != nullptr);
    remove_->setEnabled(item && (manager_ || item->data(0, uploader_role).toLongLong() == user_));
}
void group_files_page::refresh() { request_page(false); }
void group_files_page::request_page(bool older)
{
    if (!isEnabled())
    {
        return;
    }
    if (!older)
    {
        list_->clear();
    }
    auto const* last = list_->topLevelItem(list_->topLevelItemCount() - 1);
    more_->setEnabled(false);
    status_->setText(QStringLiteral("正在加载…"));
    client_.list_files(conversation_,
                       search_->text(),
                       category_->currentData().toString(),
                       last ? last->data(0, created_role).toLongLong() : 0,
                       last ? last->data(0, key_role).toString() : QString{});
}
void group_files_page::download(bool open)
{
    auto const* item = list_->currentItem();
    if (!item)
    {
        return;
    }
    auto const key = item->data(0, key_role).toString();
    auto const filename = item->text(0);
    auto const path = QFileDialog::getSaveFileName(this,
                                                   open ? QStringLiteral("保存并打开") : QStringLiteral("下载群文件"),
                                                   QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + filename);
    if (path.isEmpty())
    {
        return;
    }
    // The save dialog runs a nested event loop; a refresh can replace every row.
    for (int row = 0; isEnabled() && row < list_->topLevelItemCount(); ++row)
    {
        if (list_->topLevelItem(row)->data(0, key_role).toString() == key)
        {
            status_->setText(QStringLiteral("正在下载…"));
            client_.download_file(conversation_, key, filename, path, open);
            return;
        }
    }
    status_->setText(QStringLiteral("群文件列表已变化，请刷新后重新选择文件"));
}
