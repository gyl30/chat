#include "group_files_page.hpp"
#include "client_bridge.hpp"
#include "theme.hpp"
#include "chat_widget.hpp"
#include "download_tasks.hpp"
#include <QToolButton>
#include <QMenu>
#include <QSignalBlocker>

#include <QComboBox>
#include <QDateTime>
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
    local_upload_role,
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
    style_search_field(*search_);
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
    auto selected_source = [this]
    {
        auto const* row = list_->currentItem();
        return download_source{conversation_, 0, 0, row ? row->data(0, key_role).toString() : QString{}};
    };
    download_actions_ = client_.downloads().source_button(this, selected_source);
    layout->addWidget(download_actions_, 0, Qt::AlignLeft);
    upload_actions_ = new QToolButton(this);
    upload_actions_->setObjectName(QStringLiteral("uploadRowActions"));
    upload_actions_->setText(QStringLiteral("上传操作"));
    upload_actions_->setPopupMode(QToolButton::InstantPopup);
    auto* upload_menu = new QMenu(upload_actions_);
    upload_actions_->setMenu(upload_menu);
    connect(upload_menu,
            &QMenu::aboutToShow,
            this,
            [this, upload_menu]
            {
                upload_menu->clear();
                if (auto* row = list_->currentItem(); chat_ && row)
                {
                    chat_->add_upload_actions(*upload_menu, row->data(0, local_upload_role).toLongLong());
                }
            });
    actions->addWidget(upload_actions_);
    auto* uploads = new QHBoxLayout;
    auto* upload = upload_only_ = new QPushButton(QStringLiteral("只上传到群文件"), this);
    auto* send = upload_send_ = new QPushButton(QStringLiteral("发送到聊天"), this);
    uploads->addWidget(upload);
    uploads->addWidget(send);
    layout->addLayout(uploads);
    connect(upload, &QPushButton::clicked, this, [this] { emit upload_requested(true); });
    connect(send, &QPushButton::clicked, this, [this] { emit upload_requested(false); });
    connect(reload, &QPushButton::clicked, this, &group_files_page::refresh);
    search_delay_.setSingleShot(true);
    search_delay_.setInterval(220);
    connect(&search_delay_, &QTimer::timeout, this, [this] { request_page(false); });
    connect(search_,
            &QLineEdit::textChanged,
            this,
            [this]
            {
                reset_files();
                search_delay_.start();
            });
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
            [this](quint64 request, chat::conversation_file_page const& page, bool older, QString const& error)
            {
                if (request != request_ || !isEnabled())
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
                                                      QDateTime::fromMSecsSinceEpoch(f.created_at).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))});
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
                if (group == conversation_ && isVisible() && isEnabled())
                {
                    if (error.isEmpty())
                    {
                        if (!search_delay_.isActive())
                        {
                            refresh();
                        }
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
            [this](qint64 group, QString const& key, QString const& path, QString const& error)
            {
                auto const* selected = list_->currentItem();
                if (group != conversation_ || !isEnabled() || !selected || selected->data(0, key_role).toString() != key)
                {
                    return;
                }
                if (!error.isEmpty())
                {
                    status_->setText(error);
                    return;
                }
                status_->setText(QStringLiteral("已保存到 %1").arg(path));
            });
    setEnabled(false);
    update_selection();
    more_->setEnabled(false);
}
void group_files_page::bind_uploads(chat_widget& chat)
{
    chat_ = &chat;
    connect(&chat, &chat_widget::uploads_changed, this, &group_files_page::refresh_uploads);
    refresh_uploads();
}
void group_files_page::refresh_uploads()
{
    if (!isEnabled() || !chat_)
    {
        return;
    }
    auto rows = chat_->group_uploads(conversation_);
    for (int i = list_->topLevelItemCount() - 1; i >= 0; --i)
    {
        auto* row = list_->topLevelItem(i);
        auto id = row->data(0, local_upload_role).toLongLong();
        if (!id)
        {
            continue;
        }
        auto found = std::ranges::find(rows, id, &chat_widget::upload_row::id);
        if (found == rows.end())
        {
            delete list_->takeTopLevelItem(i);
            continue;
        }
        row->setText(1, found->status);
        rows.erase(found);
    }
    for (auto const& task : rows)
    {
        auto* row =
            new QTreeWidgetItem(QStringList{task.filename, task.status, QLocale().formattedDataSize(task.size), QStringLiteral("本机待上传")});
        row->setData(0, local_upload_role, task.id);
        list_->insertTopLevelItem(0, row);
    }
    update_selection();
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
        request_ = 0;
        search_delay_.stop();
        more_->setEnabled(false);
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
    bool const local = item && item->data(0, local_upload_role).isValid();
    save_->setEnabled(item && !local);
    open_->setEnabled(item && !local);
    remove_->setEnabled(item && !local && (manager_ || item->data(0, uploader_role).toLongLong() == user_));
    upload_actions_->setVisible(local);
    auto state = client_.downloads().summary({conversation_, 0, 0, item ? item->data(0, key_role).toString() : QString{}});
    download_actions_->setText(state);
    download_actions_->setVisible(!local && !state.isEmpty());
}
void group_files_page::reset_files()
{
    request_ = 0;
    more_->setEnabled(false);
    status_->setText(QStringLiteral("正在加载…"));
    for (int row = list_->topLevelItemCount() - 1; row >= 0; --row)
    {
        if (!list_->topLevelItem(row)->data(0, local_upload_role).isValid())
        {
            delete list_->takeTopLevelItem(row);
        }
    }
    refresh_uploads();
}
void group_files_page::refresh()
{
    if (!isEnabled())
    {
        return;
    }
    search_delay_.stop();
    reset_files();
    request_page(false);
}
void group_files_page::request_page(bool older)
{
    if (!isEnabled())
    {
        return;
    }
    auto const* last = older ? list_->topLevelItem(list_->topLevelItemCount() - 1) : nullptr;
    more_->setEnabled(false);
    status_->setText(QStringLiteral("正在加载…"));
    request_ = client_.list_files(conversation_,
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
