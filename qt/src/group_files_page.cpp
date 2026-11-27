#include "empty_state.hpp"
#include "group_files_page.hpp"
#include "client_bridge.hpp"
#include "theme.hpp"
#include "icons.hpp"
#include "chat_widget.hpp"
#include "download_tasks.hpp"
#include <QToolButton>
#include <QMenu>

#include <QTabBar>
#include <QCursor>
#include <QDateTime>
#include <QFileDialog>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include "theme_manager.hpp"
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
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
    details_role,
    heading_role,
    transfer_role,
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
class group_file_delegate final : public QStyledItemDelegate
{
   public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const& index) const override
    {
        return {0, index.data(heading_role).toBool() ? 34 : file_row_height + 8};
    }

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        auto const name = index.data().toString();
        if (index.data(heading_role).toBool())
        {
            painter->save();
            painter->setPen(themed("text_secondary"));
            painter->drawText(option.rect.adjusted(10, 0, -10, 0), Qt::AlignVCenter, name);
            painter->restore();
            return;
        }
        if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))
        {
            painter->fillRect(option.rect, themed(option.state & QStyle::State_Selected ? "selected" : "canvas_hover"));
        }
        auto const transfer = index.data(transfer_role).value<file_transfer>();
        auto details = index.data(details_role).toString();
        if (!transfer.status.isEmpty())
        {
            details = details.isEmpty() ? transfer.status : transfer.status + QStringLiteral(" · ") + details;
        }
        paint_file_row(*painter, option.rect.adjusted(10, 4, -10, -4), option.font, name, details, transfer);
    }
};
}    // namespace
group_files_page::group_files_page(client_bridge& client, qint64 conversation, qint64 user, QWidget* parent)
    : QWidget(parent), client_(client), conversation_(conversation), user_(user)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(7);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("groupFilesSearch"));
    style_search_field(*search_);
    search_->setPlaceholderText(QStringLiteral("搜索文件名"));
    layout->addWidget(search_);

    category_ = new QTabBar(this);
    category_->setObjectName(QStringLiteral("groupFileCategories"));
    category_->setDrawBase(false);
    category_->setExpanding(false);
    category_->setElideMode(Qt::ElideNone);
    category_->addTab(QStringLiteral("全部"));
    for (auto const* key : {"image", "document", "archive", "other"})
    {
        auto const index = category_->addTab(category_name(key));
        category_->setTabData(index, QString::fromLatin1(key));
    }
    layout->addWidget(category_);

    header_actions_ = new QWidget(this);
    auto* actions = new QHBoxLayout(header_actions_);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(3);
    auto tool = [this, actions](QString name, QString text, QStringView symbol)
    {
        auto* button = new QToolButton(header_actions_);
        button->setObjectName(std::move(name));
        button->setAccessibleName(text);
        button->setToolTip(std::move(text));
        button->setFixedSize(32, 32);
        button->setIconSize(chat_theme::action_icon);
        button->setProperty("navigationTool", true);
        auto refresh = [button, icon = symbol.toString()] { button->setIcon(svg_icon(icon, themed("text_secondary"), chat_theme::action_icon)); };
        refresh();
        connect(&theme_manager::instance(), &theme_manager::changed, button, refresh);
        actions->addWidget(button);
        return button;
    };
    upload_ = tool(QStringLiteral("groupFileUpload"), QStringLiteral("上传文件"), u"attach");
    upload_->setPopupMode(QToolButton::InstantPopup);
    auto* uploads = new QMenu(upload_);
    upload_->setMenu(uploads);
    uploads->addAction(QStringLiteral("仅上传到群文件"), this, [this] { emit upload_requested(true); });
    uploads->addAction(QStringLiteral("上传并发送到聊天"), this, [this] { emit upload_requested(false); });
    auto* reload = tool(QStringLiteral("groupFileReload"), QStringLiteral("刷新"), u"refresh");
    header_actions_->hide();

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("groupFileList"));
    list_->setItemDelegate(new group_file_delegate(list_));
    list_->setMouseTracking(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_, 1);
    empty_ = new empty_state(*list_->viewport(), QStringLiteral("groupFilesEmpty"));
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->hide();
    layout->addWidget(status_);
    more_ = new QPushButton(QStringLiteral("加载更多"), this);
    more_->setObjectName(QStringLiteral("groupFilesMore"));
    more_->hide();
    layout->addWidget(more_, 0, Qt::AlignHCenter);

    connect(list_, &QListWidget::customContextMenuRequested, this, &group_files_page::show_menu);
    connect(list_,
            &QListWidget::itemClicked,
            this,
            [this](QListWidgetItem* item)
            {
                auto const point = list_->viewport()->mapFromGlobal(QCursor::pos());
                if (list_->itemAt(point) == item && point.x() < 58)
                {
                    activate(*item);
                }
            });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { activate(*item); });
    connect(reload, &QToolButton::clicked, this, &group_files_page::refresh);
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
    connect(category_, &QTabBar::currentChanged, this, &group_files_page::refresh);
    connect(more_, &QPushButton::clicked, this, [this] { request_page(true); });
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
                    set_status(error);
                    more_->setVisible(older);
                    more_->setEnabled(older);
                    return;
                }
                for (auto const& f : page.files)
                {
                    auto* item = new QListWidgetItem(QString::fromStdString(f.filename), list_);
                    auto const key = QString::fromStdString(f.object_key);
                    item->setToolTip(item->text());
                    item->setData(key_role, key);
                    item->setData(created_role, static_cast<qint64>(f.created_at));
                    item->setData(uploader_role, static_cast<qint64>(f.uploader_id));
                    item->setData(details_role,
                                  QStringLiteral("%1 · %2 · %3")
                                      .arg(file_size(f.size),
                                           QString::fromStdString(f.username),
                                           QDateTime::fromMSecsSinceEpoch(f.created_at).toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
                    item->setData(transfer_role, QVariant::fromValue(client_.downloads().transfer({conversation_, 0, 0, key})));
                }
                more_->setVisible(page.has_more);
                more_->setEnabled(page.has_more);
                const bool has_files = list_->count() && list_->item(list_->count() - 1)->data(key_role).isValid();
                set_status(has_files                                                               ? QString{}
                           : search_->text().trimmed().isEmpty() && category_->currentIndex() == 0 ? QStringLiteral("暂无群文件")
                                                                                                   : QStringLiteral("未找到匹配文件"),
                           !has_files);
            });
    connect(&client_,
            &client_bridge::files_changed,
            this,
            [this](qint64 group, QString const& error)
            {
                if (group != conversation_ || !isEnabled())
                {
                    return;
                }
                if (!error.isEmpty())
                {
                    set_status(error);
                }
                else if (!search_delay_.isActive())
                {
                    if (isVisible())
                    {
                        refresh();
                    }
                    else
                    {
                        request_ = 0;
                    }
                }
            });
    connect(&client_.downloads(),
            &download_tasks::changed,
            this,
            [this](QList<download_source> const& sources)
            {
                for (int i = 0; i < list_->count(); ++i)
                {
                    auto* item = list_->item(i);
                    auto const key = item->data(key_role).toString();
                    download_source const source{conversation_, 0, 0, key};
                    if (!key.isEmpty() && (sources.isEmpty() || sources.contains(source)))
                    {
                        item->setData(transfer_role, QVariant::fromValue(client_.downloads().transfer(source)));
                    }
                }
            });
    setEnabled(false);
}

void group_files_page::set_status(QString const& text, bool empty_result)
{
    status_->setText(text);
    empty_->set_message(text, empty_result ? u"folder" : QStringView{});
    update_status_layout();
}

void group_files_page::update_status_layout()
{
    bool const populated = list_->count() != 0;
    status_->setVisible(populated && !status_->text().isEmpty());
    empty_->setVisible(!populated && !status_->text().isEmpty());
}

void group_files_page::activate(QListWidgetItem const& item)
{
    if (!isEnabled())
    {
        return;
    }
    if (auto const id = item.data(local_upload_role).toLongLong(); chat_ && id)
    {
        chat_->activate_upload(id, item.data(transfer_role).value<file_transfer>().action);
    }
    else
    {
        auto const key = item.data(key_role).toString();
        if (!key.isEmpty() && !client_.downloads().activate({conversation_, 0, 0, key}, this))
        {
            download(item, false);
        }
    }
}

void group_files_page::show_menu(QPoint position)
{
    auto const* item = list_->itemAt(position);
    if (!item || !isEnabled())
    {
        return;
    }
    QMenu menu(this);
    auto const key = item->data(key_role).toString();
    if (auto const id = item->data(local_upload_role).toLongLong(); chat_ && id)
    {
        chat_->add_upload_actions(menu, id);
        menu.exec(list_->viewport()->mapToGlobal(position));
    }
    else if (!key.isEmpty())
    {
        auto const name = item->text();
        auto const uploader = item->data(uploader_role).toLongLong();
        auto* save = menu.addAction(QStringLiteral("另存为"));
        auto* open = menu.addAction(QStringLiteral("保存并打开"));
        client_.downloads().add_actions(menu, {conversation_, 0, 0, key});
        auto* remove = (manager_ || uploader == user_) ? menu.addAction(QStringLiteral("移除群文件")) : nullptr;
        auto* selected = menu.exec(list_->viewport()->mapToGlobal(position));
        if (selected == save || selected == open)
        {
            if (auto const* current = find_file(key))
            {
                download(*current, selected == open);
            }
        }
        else if (remove && selected == remove &&
                 confirm_action(this,
                                QStringLiteral("移除群文件"),
                                QStringLiteral("从本群的文件列表移除“%1”？聊天消息和其他群的引用会保留。").arg(name),
                                QStringLiteral("移除群文件")) &&
                 isEnabled() && find_file(key) && (manager_ || uploader == user_))
        {
            client_.change_file(conversation_, key, false);
        }
    }
}

QListWidgetItem* group_files_page::find_file(QString const& key) const
{
    for (int i = 0; i < list_->count(); ++i)
    {
        if (list_->item(i)->data(key_role).toString() == key)
        {
            return list_->item(i);
        }
    }
    return nullptr;
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
    bool const has_rows = !rows.isEmpty();
    auto* heading = list_->item(0);
    auto const has_heading = heading && heading->data(heading_role).toBool();
    if (has_rows && !has_heading)
    {
        heading = new QListWidgetItem(QStringLiteral("本机上传"));
        heading->setFlags(Qt::NoItemFlags);
        heading->setData(heading_role, true);
        list_->insertItem(0, heading);
    }
    else if (!has_rows && has_heading)
    {
        delete list_->takeItem(0);
    }
    for (int i = list_->count() - 1; i >= 0; --i)
    {
        auto* row = list_->item(i);
        auto id = row->data(local_upload_role).toLongLong();
        if (!id)
        {
            continue;
        }
        auto found = std::ranges::find(rows, id, &chat_widget::upload_row::id);
        if (found == rows.end())
        {
            delete list_->takeItem(i);
            continue;
        }
        row->setData(transfer_role, QVariant::fromValue(found->transfer));
        row->setToolTip(found->error.isEmpty() ? found->filename : found->filename + u'\n' + found->error);
        rows.erase(found);
    }
    for (auto const& task : rows)
    {
        auto* row = new QListWidgetItem(task.filename);
        row->setData(local_upload_role, task.id);
        row->setData(transfer_role, QVariant::fromValue(task.transfer));
        row->setToolTip(task.error.isEmpty() ? task.filename : task.filename + u'\n' + task.error);
        list_->insertItem(1, row);
    }
    update_status_layout();
}
void group_files_page::set_access(bool member, bool manager, bool can_publish)
{
    bool const gained = member && !isEnabled();
    setEnabled(member);
    manager_ = manager;
    header_actions_->setEnabled(member);
    upload_->setVisible(member && can_publish);
    if (!member)
    {
        request_ = 0;
        search_delay_.stop();
        more_->hide();
        list_->clear();
        set_status(QStringLiteral("当前无法访问群文件"));
    }
    if (gained && isVisible())
    {
        refresh();
    }
}
void group_files_page::reset_files()
{
    request_ = 0;
    more_->hide();
    for (int row = list_->count() - 1; row >= 0; --row)
    {
        if (list_->item(row)->data(key_role).isValid())
        {
            delete list_->takeItem(row);
        }
    }
    refresh_uploads();
    set_status(QStringLiteral("正在加载…"));
}
void group_files_page::show_files()
{
    if (!request_ && !search_delay_.isActive())
    {
        refresh();
    }
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
    auto const* last = older ? list_->item(list_->count() - 1) : nullptr;
    more_->hide();
    set_status(QStringLiteral("正在加载…"));
    request_ = client_.list_files(conversation_,
                                  search_->text(),
                                  category_->tabData(category_->currentIndex()).toString(),
                                  last ? last->data(created_role).toLongLong() : 0,
                                  last ? last->data(key_role).toString() : QString{});
}
void group_files_page::download(QListWidgetItem const& item, bool open)
{
    auto const key = item.data(key_role).toString();
    auto const filename = item.text();
    auto const path = QFileDialog::getSaveFileName(this,
                                                   open ? QStringLiteral("保存并打开") : QStringLiteral("下载群文件"),
                                                   QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + filename);
    if (path.isEmpty())
    {
        return;
    }
    if (isEnabled() && find_file(key))
    {
        client_.download_file(conversation_, key, filename, path, open);
    }
    else
    {
        set_status(QStringLiteral("群文件列表已变化，请刷新后重新选择文件"));
    }
}
