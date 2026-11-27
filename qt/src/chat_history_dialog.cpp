#include "empty_state.hpp"
#include "chat_history_dialog.hpp"

#include <algorithm>
#include <utility>
#include <utf8proc.h>

#include <QAction>
#include <QDateTime>
#include <QDesktopServices>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabBar>
#include <QVBoxLayout>

#include "message_preview.hpp"
#include "file_presentation.hpp"
#include "download_tasks.hpp"
#include "message_images.hpp"
#include "message_links.hpp"
#include "theme.hpp"
#include "theme_manager.hpp"
#include "icons.hpp"

namespace
{

enum media_role
{
    filename_role = preview_role::more + 1,
    size_role,
    date_role,
    link_role,
    category_role,
};

QUrl first_link(QString const& text)
{
    auto const links = message_links(text);
    return links.isEmpty() ? QUrl{} : QUrl(links.front().format.anchorHref());
}

QDate sent_date(message_data const& message) { return QDateTime::fromMSecsSinceEpoch(message.timestamp).date(); }

bool date_header(QListWidgetItem const& item) { return item.data(date_role).isValid(); }

int tile_side(int width)
{
    auto const columns = std::max(1, (width + 24) / 126);
    return std::max(1, width / columns);
}

class history_media_delegate final : public QStyledItemDelegate
{
   public:
    history_media_delegate(QListWidget& list, QAbstractItemDelegate& previews, message_images* images, download_tasks* downloads)
        : QStyledItemDelegate(&list), list_(list), previews_(previews), images_(images), downloads_(downloads)
    {
    }

    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        switch (index.data(category_role).toInt())
        {
            case int(chat::message_category::images):
            {
                // The icon view can reserve a scrollbar while it lays out the next row.
                auto const gutter = list_.style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, &list_) + 4;
                auto const width = std::max(1, list_.viewport()->width() - gutter);
                return index.data(date_role).isValid() ? QSize(width, 38) : QSize(tile_side(width), tile_side(width));
            }
            case int(chat::message_category::files):
                return {0, file_row_height + 8};
            case int(chat::message_category::links):
                return {0, 72};
            default:
                return previews_.sizeHint(option, index);
        }
    }

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        auto const category = static_cast<chat::message_category>(index.data(category_role).toInt());
        if (category == chat::message_category::all)
        {
            previews_.paint(painter, option, index);
            return;
        }

        painter->save();
        auto const selected = option.state & QStyle::State_Selected;
        if (selected || option.state & QStyle::State_MouseOver)
        {
            painter->fillRect(option.rect, themed(selected ? "selected" : "canvas_hover"));
        }
        if (category == chat::message_category::images)
        {
            if (const auto date = index.data(date_role); date.isValid())
            {
                auto font = option.font;
                font.setWeight(QFont::DemiBold);
                painter->setFont(font);
                painter->setPen(themed("text_secondary"));
                painter->drawText(option.rect.adjusted(6, 0, -6, 0), Qt::AlignVCenter, date.toString());
                painter->restore();
                return;
            }
            const QRect tile = option.rect.adjusted(2, 2, -2, -2);
            painter->fillRect(tile, themed("subtle"));
            auto const image = images_ ? images_->image(index.data(preview_role::message).toLongLong()) : QPixmap{};
            if (image.isNull())
            {
                svg_icon(u"image", themed("text_muted"), {24, 24}).paint(painter, {tile.center() - QPoint(12, 12), QSize(24, 24)});
            }
            else
            {
                auto const source = tile.size().scaled(image.size(), Qt::KeepAspectRatio);
                QRect crop(QPoint{}, source);
                crop.moveCenter(image.rect().center());
                painter->setRenderHint(QPainter::SmoothPixmapTransform);
                painter->drawPixmap(tile, image, crop);
            }
            if (selected || option.state.testFlag(QStyle::State_HasFocus))
            {
                painter->setPen(QPen(themed("focus"), 2));
                painter->setBrush(Qt::NoBrush);
                painter->drawRect(tile.adjusted(1, 1, -1, -1));
            }
        }
        else if (category == chat::message_category::files)
        {
            auto const transfer =
                downloads_ ? downloads_->transfer(
                                 {index.data(preview_role::conversation).toLongLong(), index.data(preview_role::message).toLongLong(), 0, {}})
                           : file_transfer{file_action::download, {}};
            auto details = transfer.status.isEmpty() ? file_size(index.data(size_role).toLongLong()) : transfer.status;
            details += QStringLiteral(" · ") + index.data(preview_role::sender).toString() + QStringLiteral(" · ") +
                       index.data(preview_role::time).toString();
            paint_file_row(*painter, option.rect.adjusted(10, 4, -10, -4), option.font, index.data(filename_role).toString(), details, transfer);
        }
        else
        {
            auto const url = index.data(link_role).toUrl();
            auto const icon = QRect(option.rect.left() + 10, option.rect.center().y() - 21, 42, 42);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(themed("subtle"));
            painter->drawRoundedRect(icon, 10, 10);
            svg_icon(u"link", themed("text_secondary"), {22, 22}).paint(painter, {icon.center() - QPoint(11, 11), QSize(22, 22)});
            auto const rect = option.rect.adjusted(62, 7, -10, -6);
            auto font = option.font;
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            painter->setPen(themed("text"));
            auto const host = url.host().isEmpty() ? QStringLiteral("链接") : url.host();
            painter->drawText(
                QRect(rect.topLeft(), QSize(rect.width(), 20)), Qt::AlignVCenter, QFontMetrics(font).elidedText(host, Qt::ElideRight, rect.width()));
            font.setWeight(QFont::Normal);
            painter->setFont(font);
            painter->setPen(themed("link"));
            painter->drawText(QRect(rect.topLeft() + QPoint(0, 20), QSize(rect.width(), 18)),
                              Qt::AlignVCenter,
                              QFontMetrics(font).elidedText(url.toDisplayString(), Qt::ElideMiddle, rect.width()));
            painter->setPen(themed("text_secondary"));
            auto const source = index.data(preview_role::sender).toString() + QStringLiteral(" · ") + index.data(preview_role::time).toString();
            painter->drawText(QRect(rect.topLeft() + QPoint(0, 39), QSize(rect.width(), 18)),
                              Qt::AlignVCenter,
                              QFontMetrics(font).elidedText(source, Qt::ElideRight, rect.width()));
        }
        painter->restore();
    }

   private:
    QListWidget& list_;
    QAbstractItemDelegate& previews_;
    message_images* images_;
    download_tasks* downloads_;
};

QString content_text(message_data const& message)
{
    if (message.attachment)
    {
        auto const image = message.attachment->media_type.startsWith(QStringLiteral("image/"));
        return (image ? QStringLiteral("[图片] ") : QStringLiteral("[文件] ")) + message.attachment->filename +
               (image ? QString{} : QStringLiteral(" · ") + file_size(message.attachment->size));
    }
    return message.text.simplified();
}

bool supersedes(message_data const& change, message_data const& old) { return !old.deleted && (change.deleted || change.edited_at > old.edited_at); }

}    // namespace

chat_history_dialog::chat_history_dialog(
    qint64 conversation, QString const& title, QWidget* parent, avatar_cache* avatars, message_images* images, download_tasks* downloads)
    : QDialog(parent), conversation_(conversation), images_(images), downloads_(downloads)
{
    setObjectName(QStringLiteral("chatHistoryDialog"));
    setWindowTitle(QStringLiteral("聊天记录 · %1").arg(title));
    resize(560, 640);
    auto* layout = new QVBoxLayout(this);
    frame_dialog(*this);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    auto* search = new QLineEdit(this);
    search->setObjectName(QStringLiteral("historySearchEdit"));
    style_search_field(*search);
    search->setPlaceholderText(QStringLiteral("搜索聊天记录"));
    search->setAccessibleName(QStringLiteral("搜索聊天记录"));
    layout->addWidget(search);
    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("historyTabs"));
    tabs_->setDrawBase(false);
    tabs_->setExpanding(false);
    for (auto const* name : {"全部", "图片", "文件", "链接"})
    {
        tabs_->addTab(QString::fromUtf8(name));
    }
    layout->addWidget(tabs_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("historyStatus"));
    status_->setAlignment(Qt::AlignCenter);
    status_->hide();
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("historyList"));
    list_->setAccessibleName(QStringLiteral("聊天记录"));
    style_message_previews(*list_, *search, avatars);
    auto* previews = list_->itemDelegate();
    list_->setItemDelegate(new history_media_delegate(*list_, *previews, images_, downloads_));
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->viewport()->installEventFilter(this);
    if (images_)
    {
        connect(images_, &message_images::changed, list_->viewport(), qOverload<>(&QWidget::update));
    }
    if (downloads_)
    {
        connect(downloads_,
                &download_tasks::changed,
                this,
                [this](QList<download_source> const& sources)
                {
                    if (sources.isEmpty() || std::ranges::any_of(sources, [this](auto const& src) { return src.conversation == conversation_; }))
                    {
                        list_->viewport()->update();
                    }
                });
    }
    list_->setFrameShape(QFrame::NoFrame);
    list_->setUniformItemSizes(true);
    connect(&theme_manager::instance(), &theme_manager::changed, this, [this] { list_->viewport()->update(); });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_, 1);
    empty_ = new empty_state(*list_->viewport(), QStringLiteral("historyEmpty"));
    layout->addWidget(status_);
    more_ = new QPushButton(QStringLiteral("加载更早的记录"), this);
    more_->setObjectName(QStringLiteral("historyMoreButton"));
    more_->hide();
    layout->addWidget(more_, 0, Qt::AlignHCenter);

    connect(search, &QLineEdit::returnPressed, this, [this, search] { restart_search(search->text().trimmed()); });
    connect(search,
            &QLineEdit::textChanged,
            this,
            [this](QString const& text)
            {
                // Clearing the search returns to browsing the whole history.
                if (text.trimmed().isEmpty() && !query_.isEmpty())
                {
                    restart_search({});
                }
            });
    connect(tabs_, &QTabBar::currentChanged, this, [this] { restart_search(query_); });
    connect(more_, &QPushButton::clicked, this, [this] { request_more(); });
    connect(list_->verticalScrollBar(),
            &QScrollBar::valueChanged,
            this,
            [this](int value)
            {
                observe_visible_images();
                if (value == list_->verticalScrollBar()->maximum() && value > 0)
                {
                    request_more();
                }
            });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { activate(*item); });
    connect(list_,
            &QListWidget::itemClicked,
            this,
            [this](QListWidgetItem* item)
            {
                if (tabs_->currentIndex() == int(category::images) &&
                    !list_->style()->styleHint(QStyle::SH_ItemView_ActivateItemOnSingleClick, nullptr, list_))
                {
                    activate(*item);
                }
            });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](QPoint position) { show_menu(position); });
    search->setFocus(Qt::OtherFocusReason);
    update_status();
}

void chat_history_dialog::start(category initial)
{
    if (int(initial) == tabs_->currentIndex())
    {
        request_more();
    }
    else
    {
        tabs_->setCurrentIndex(int(initial));
    }
}

bool chat_history_dialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == list_->viewport() && (event->type() == QEvent::Show || event->type() == QEvent::Resize))
    {
        QTimer::singleShot(0,
                           this,
                           [this]
                           {
                               if (tabs_->currentIndex() == int(category::images))
                               {
                                   list_->doItemsLayout();
                                   observe_visible_images();
                               }
                           });
    }
    return QDialog::eventFilter(watched, event);
}

void chat_history_dialog::observe_visible_images()
{
    if (!images_ || tabs_->currentIndex() != int(category::images) || !list_->isVisible())
    {
        return;
    }
    for (int i = 0; i < list_->count(); ++i)
    {
        auto const* item = list_->item(i);
        if (!date_header(*item) && list_->viewport()->rect().intersects(list_->visualItemRect(item)))
        {
            images_->observe(conversation_, item->data(preview_role::message).toLongLong());
        }
    }
}

void chat_history_dialog::set_row(QListWidgetItem& item, message_data const& message)
{
    chat::message_search_hit const hit{message.conversation, message.id, message.from, message.timestamp, {}, message.username.toStdString(), {}};
    set_message_preview(item, hit, message.attachment ? content_text(message) : matched_message_excerpt(message.text, query_));
    auto const kind = static_cast<category>(tabs_->currentIndex());
    item.setData(category_role, int(kind));
    if (message.attachment)
    {
        item.setData(filename_role, message.attachment->filename);
        item.setData(size_role, message.attachment->size);
        item.setToolTip(message.attachment->filename);
    }
    if (kind == category::links)
    {
        item.setData(link_role, first_link(message.text));
    }
}

void chat_history_dialog::activate(QListWidgetItem const& item)
{
    auto const id = item.data(preview_role::message).toLongLong();
    if (!id)
    {
        return;
    }
    auto const filename = item.data(filename_role).toString();
    switch (static_cast<category>(item.data(category_role).toInt()))
    {
        case category::images:
            emit image_requested(id, filename);
            break;
        case category::files:
            if (downloads_ && !downloads_->activate({conversation_, id, 0, {}}, this))
            {
                downloads_->save({conversation_, id, 0, {}}, filename, this);
            }
            break;
        case category::links:
            if (auto const url = item.data(link_role).toUrl(); !url.isEmpty())
            {
                QDesktopServices::openUrl(url);
            }
            else
            {
                emit message_activated(id);
            }
            break;
        default:
            emit message_activated(id);
            break;
    }
}

void chat_history_dialog::interrupt_loading()
{
    if (!loading_)
    {
        return;
    }
    loading_ = false;
    page_updates_.clear();
    empty_->set_message();
    status_->setText(QStringLiteral("连接已断开，恢复后可重新加载。"));
    status_->show();
    more_->setText(QStringLiteral("重新加载"));
    more_->setVisible(has_more_);
}

void chat_history_dialog::restart_search(QString query)
{
    query_ = std::move(query);
    messages_.clear();
    before_ = 0;
    has_more_ = true;
    loading_ = false;
    list_->clear();
    bool const images = tabs_->currentIndex() == int(category::images);
    list_->setViewMode(images ? QListView::IconMode : QListView::ListMode);
    list_->setMovement(QListView::Static);
    list_->setResizeMode(QListView::Adjust);
    list_->setFlow(images ? QListView::LeftToRight : QListView::TopToBottom);
    list_->setWrapping(images);
    list_->setUniformItemSizes(!images);
    list_->setGridSize({});
    request_more();
}

void chat_history_dialog::request_more()
{
    if (loading_ || !has_more_)
    {
        return;
    }
    loading_ = true;
    page_updates_.clear();
    more_->setText(QStringLiteral("加载更早的记录"));
    update_status();
    emit history_requested(static_cast<category>(tabs_->currentIndex()), query_, before_);
}

void chat_history_dialog::set_history(qint64 conversation, QList<message_data> messages, bool has_more, QString const& error)
{
    if (conversation != conversation_ || !loading_)
    {
        return;
    }
    loading_ = false;
    if (!error.isEmpty())
    {
        page_updates_.clear();
        empty_->set_message();
        status_->setText(error);
        status_->show();
        more_->setText(QStringLiteral("重新加载"));
        more_->show();
        return;
    }
    add_messages(std::move(messages), has_more);
}

void chat_history_dialog::add_messages(QList<message_data> messages, bool has_more)
{
    // Pages arrive oldest-first; the window lists the newest first.
    std::ranges::sort(messages, [](auto const& a, auto const& b) { return a.id > b.id; });
    auto const boundary = before_;
    if (!messages.isEmpty())
    {
        before_ = messages.back().id;
    }
    has_more_ = has_more && !messages.isEmpty();
    for (auto& message : messages)
    {
        if (boundary && message.id >= boundary)
        {
            continue;
        }
        auto const edited = page_updates_.constFind(message.id);
        if (edited != page_updates_.cend() && (edited->deleted || supersedes(*edited, message)))
        {
            if (!matches(*edited))
            {
                continue;
            }
            message = *edited;
        }
        if (tabs_->currentIndex() == int(category::images) && (messages_.isEmpty() || sent_date(messages_.last()) != sent_date(message)))
        {
            auto* heading = new QListWidgetItem(list_);
            heading->setFlags(Qt::NoItemFlags);
            heading->setData(category_role, int(category::images));
            auto const label = QLocale().toString(sent_date(message), QStringLiteral("yyyy-MM-dd"));
            heading->setData(date_role, label);
            heading->setText(label);
            heading->setData(Qt::AccessibleTextRole, label);
        }
        set_row(*new QListWidgetItem(list_), message);
        messages_.push_back(std::move(message));
    }
    page_updates_.clear();
    update_status();
    QTimer::singleShot(0, this, &chat_history_dialog::observe_visible_images);
}

bool chat_history_dialog::matches(message_data const& message) const
{
    if (message.deleted)
    {
        return false;
    }
    auto const kind = static_cast<category>(tabs_->currentIndex());
    auto const image = message.attachment && message.attachment->media_type.startsWith(QStringLiteral("image/"));
    if ((kind == category::images && !image) || (kind == category::files && (!message.attachment || image)) ||
        (kind == category::links && !message.text.contains(QStringLiteral("http://")) && !message.text.contains(QStringLiteral("https://"))))
    {
        return false;
    }
    if (query_.isEmpty())
    {
        return true;
    }
    // Same code-point lowercasing as SQLite's chat_lower, without Qt case folding.
    auto const lower = [](QString const& input)
    {
        auto points = input.toStdU32String();
        for (auto& point : points)
        {
            point = utf8proc_tolower(point);
        }
        return points;
    };
    return lower(message.text).contains(lower(query_));
}

void chat_history_dialog::update_message(qint64 conversation, message_data message, QString const& error)
{
    if (conversation != conversation_ || !error.isEmpty() || message.acknowledgement_only)
    {
        return;
    }
    auto found = std::ranges::find(messages_, message.id, &message_data::id);
    if (found == messages_.end())
    {
        if (loading_ && (!before_ || message.id < before_))
        {
            auto const old = page_updates_.constFind(message.id);
            if (old == page_updates_.cend() || supersedes(message, *old))
            {
                auto const id = message.id;
                page_updates_.insert(id, std::move(message));
            }
        }
        return;
    }
    if (!supersedes(message, *found))
    {
        return;
    }
    auto* item = item_for_message(message.id);
    if (matches(message))
    {
        *found = std::move(message);
        set_row(*item, *found);
    }
    else
    {
        messages_.erase(found);
        QSignalBlocker const blocker(list_->verticalScrollBar());
        auto const row = list_->row(item);
        delete list_->takeItem(row);
        if (auto const* heading = list_->item(row - 1); heading && date_header(*heading))
        {
            auto const* next = list_->item(row);
            if (!next || date_header(*next))
            {
                delete list_->takeItem(row - 1);
            }
        }
        if (messages_.isEmpty())
        {
            update_status();
        }
    }
}

void chat_history_dialog::update_status()
{
    auto const kind = static_cast<category>(tabs_->currentIndex());
    auto const no_rows = list_->count() == 0;
    if (no_rows)
    {
        static const QStringView icons[]{u"history", u"image", u"file", u"link"};
        static const QStringView empty[]{u"暂无聊天记录", u"暂无图片", u"暂无文件", u"暂无链接"};
        static const QStringView no_match[]{u"未找到相关消息", u"未找到相关图片", u"未找到相关文件", u"未找到相关链接"};
        auto const i = int(kind);
        auto const message = loading_ ? QStringLiteral("正在加载…") : (query_.isEmpty() ? empty[i] : no_match[i]).toString();
        status_->setText(message);
        status_->hide();
        empty_->set_message(message, loading_ ? QStringView{} : icons[i]);
    }
    else
    {
        empty_->set_message();
        status_->clear();
        status_->hide();
    }
    more_->setVisible(has_more_ && !loading_);
}

QListWidgetItem* chat_history_dialog::item_for_message(qint64 id) const
{
    for (int row = 0; row < list_->count(); ++row)
    {
        auto* item = list_->item(row);
        if (item->data(preview_role::message).toLongLong() == id)
        {
            return item;
        }
    }
    return nullptr;
}

void chat_history_dialog::show_menu(QPoint position)
{
    auto* item = list_->itemAt(position);
    if (!item || date_header(*item))
    {
        return;
    }
    list_->setCurrentItem(item);
    auto const id = item->data(preview_role::message).toLongLong();
    auto const found = std::ranges::find(messages_, id, &message_data::id);
    QMenu menu(this);
    connect(this, &QDialog::finished, &menu, &QMenu::close);
    auto const category = static_cast<chat_history_dialog::category>(item->data(category_role).toInt());
    auto const url = item->data(link_role).toUrl();
    auto* open = !url.isEmpty()                 ? menu.addAction(QStringLiteral("打开链接"))
                 : category == category::images ? menu.addAction(QStringLiteral("查看图片"))
                                                : nullptr;
    auto* copy_link = !url.isEmpty() ? menu.addAction(QStringLiteral("复制链接")) : nullptr;
    auto* save = (category == category::images || category == category::files) && downloads_ ? menu.addAction(QStringLiteral("另存为")) : nullptr;
    if (save)
    {
        downloads_->add_actions(menu, {conversation_, id, 0, {}});
    }
    auto* locate = menu.addAction(QStringLiteral("定位到聊天位置"));
    auto* copy = found != messages_.end() && !found->text.isEmpty() ? menu.addAction(QStringLiteral("复制")) : nullptr;
    auto* chosen = menu.exec(list_->viewport()->mapToGlobal(position));
    // Live edits can change the cache while the nested menu loop is running.
    auto const current = std::ranges::find(messages_, id, &message_data::id);
    if (current == messages_.end() || current->deleted)
    {
        return;
    }
    if (chosen == locate)
    {
        emit message_activated(id);
    }
    else if (open && chosen == open)
    {
        activate(*item_for_message(id));
    }
    else if (copy_link && chosen == copy_link)
    {
        QGuiApplication::clipboard()->setText(first_link(current->text).toString(QUrl::FullyEncoded));
    }
    else if (save && chosen == save && current->attachment)
    {
        downloads_->save({conversation_, id, 0, {}}, current->attachment->filename, this);
    }
    else if (copy && chosen == copy)
    {
        QGuiApplication::clipboard()->setText(current->text);
    }
}
