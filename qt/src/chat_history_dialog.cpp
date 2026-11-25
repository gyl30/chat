#include "chat_history_dialog.hpp"

#include <algorithm>
#include <utility>
#include <utf8proc.h>

#include <QAction>
#include <QClipboard>
#include <QDateTime>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QVBoxLayout>

#include "avatar.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "theme_manager.hpp"

namespace
{

constexpr int id_role = Qt::UserRole;
constexpr int sender_role = Qt::UserRole + 1;
constexpr int time_role = Qt::UserRole + 2;
constexpr int content_role = Qt::UserRole + 3;
constexpr int from_role = Qt::UserRole + 4;
constexpr int row_height = 70;

QString time_text(qint64 timestamp)
{
    auto const value = QDateTime::fromMSecsSinceEpoch(timestamp);
    auto const today = QDate::currentDate();
    if (value.date() == today)
    {
        return value.toString(QStringLiteral("HH:mm"));
    }
    if (value.date().year() == today.year())
    {
        return value.toString(QStringLiteral("M月d日 HH:mm"));
    }
    return value.toString(QStringLiteral("yyyy年M月d日 HH:mm"));
}

QString size_text(qint64 bytes)
{
    if (bytes < 1024)
    {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024)
    {
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
}

QString content_text(message_data const& message)
{
    if (message.attachment)
    {
        auto const image = message.attachment->media_type.startsWith(QStringLiteral("image/"));
        return (image ? QStringLiteral("[图片] ") : QStringLiteral("[文件] ")) + message.attachment->filename +
               (image ? QString{} : QStringLiteral(" · ") + size_text(message.attachment->size));
    }
    return message.text.simplified();
}

void set_row(QListWidgetItem& item, message_data const& message)
{
    item.setData(id_role, message.id);
    item.setData(sender_role, message.username);
    item.setData(from_role, message.from);
    item.setData(time_role, time_text(message.timestamp));
    item.setData(content_role, content_text(message));
    item.setData(Qt::AccessibleTextRole,
                 message.username + QStringLiteral("，") + time_text(message.timestamp) + QStringLiteral("：") + content_text(message));
    item.setToolTip(content_text(message));
}

bool supersedes(message_data const& change, message_data const& old) { return !old.deleted && (change.deleted || change.edited_at > old.edited_at); }

// One record per row, as in WeChat's history: avatar, sender and time, then the content.
class history_delegate final : public QStyledItemDelegate
{
   public:
    history_delegate(avatar_cache* avatars, QObject* parent) : QStyledItemDelegate(parent), avatars_(avatars) {}

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        auto const rect = option.rect;
        if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(option.state & QStyle::State_Selected ? themed("selected") : themed("subtle"));
            painter->drawRoundedRect(QRectF(rect).adjusted(4, 2, -4, -2), 10, 10);
        }
        auto const sender = index.data(sender_role).toString();
        auto const from = index.data(from_role).toLongLong();
        QRect avatar(rect.left() + 14, rect.top() + 12, 36, 36);
        paint_avatar(*painter, avatar, sender, 15, avatars_ ? avatars_->image(from) : QPixmap{});
        auto const left = avatar.right() + 12;
        auto const right = rect.right() - 14;
        QFont name_font = option.font;
        name_font.setPixelSize(13);
        name_font.setBold(true);
        QFont time_font = option.font;
        time_font.setPixelSize(12);
        auto const time = index.data(time_role).toString();
        auto const time_width = QFontMetrics(time_font).horizontalAdvance(time);
        painter->setFont(time_font);
        painter->setPen(themed("text_muted"));
        painter->drawText(QRect(right - time_width, rect.top() + 12, time_width, 18), Qt::AlignRight | Qt::AlignVCenter, time);
        painter->setFont(name_font);
        painter->setPen(themed("text_secondary"));
        QRect name(left, rect.top() + 12, right - time_width - 12 - left, 18);
        painter->drawText(name, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(name_font).elidedText(sender, Qt::ElideRight, name.width()));
        QFont content_font = option.font;
        content_font.setPixelSize(14);
        painter->setFont(content_font);
        painter->setPen(themed("text"));
        QRect content(left, rect.top() + 34, right - left, 22);
        painter->drawText(content,
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(content_font).elidedText(index.data(content_role).toString(), Qt::ElideRight, content.width()));
        painter->restore();
    }

    QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const&) const override { return {480, row_height}; }

   private:
    avatar_cache* avatars_;
};

}    // namespace

chat_history_dialog::chat_history_dialog(qint64 conversation, QString const& title, QWidget* parent, avatar_cache* avatars)
    : QDialog(parent), conversation_(conversation), avatars_(avatars)
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
    search->setPlaceholderText(QStringLiteral("搜索"));
    search->setAccessibleName(QStringLiteral("搜索聊天记录"));
    search->setClearButtonEnabled(true);
    auto* search_icon = search->addAction(svg_icon(u"search", themed("text_muted"), QSize(16, 16)), QLineEdit::LeadingPosition);
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
    list_->setItemDelegate(new history_delegate(avatars_, list_));
    list_->setFrameShape(QFrame::NoFrame);
    list_->setUniformItemSizes(true);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMouseTracking(true);
    connect(&theme_manager::instance(),
            &theme_manager::changed,
            this,
            [this, search_icon]
            {
                search_icon->setIcon(svg_icon(u"search", themed("text_muted"), QSize(16, 16)));
                list_->viewport()->update();
            });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_, 1);
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
                if (value == list_->verticalScrollBar()->maximum() && value > 0)
                {
                    request_more();
                }
            });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { emit message_activated(item->data(id_role).toLongLong()); });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](QPoint position) { show_menu(position); });
    if (avatars_)
    {
        connect(avatars_, &avatar_cache::changed, list_->viewport(), [this] { list_->viewport()->update(); });
    }
    update_status();
}

void chat_history_dialog::start() { request_more(); }

void chat_history_dialog::interrupt_loading()
{
    if (!loading_)
    {
        return;
    }
    loading_ = false;
    page_updates_.clear();
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
        set_row(*new QListWidgetItem(list_), message);
        messages_.push_back(std::move(message));
    }
    page_updates_.clear();
    update_status();
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
    auto const row = static_cast<int>(found - messages_.begin());
    if (matches(message))
    {
        *found = std::move(message);
        set_row(*list_->item(row), *found);
    }
    else
    {
        messages_.erase(found);
        QSignalBlocker const blocker(list_->verticalScrollBar());
        delete list_->takeItem(row);
        if (messages_.isEmpty())
        {
            update_status();
        }
    }
}

void chat_history_dialog::update_status()
{
    auto const kind = static_cast<category>(tabs_->currentIndex());
    if (loading_ && list_->count() == 0)
    {
        status_->setText(QStringLiteral("正在加载…"));
    }
    else if (list_->count() == 0)
    {
        status_->setText(!query_.isEmpty()          ? QStringLiteral("没有找到相关记录")
                         : kind == category::images ? QStringLiteral("没有图片")
                         : kind == category::files  ? QStringLiteral("没有文件")
                         : kind == category::links  ? QStringLiteral("没有链接")
                                                    : QStringLiteral("还没有聊天记录"));
    }
    else
    {
        status_->clear();
    }
    status_->setVisible(!status_->text().isEmpty());
    more_->setVisible(has_more_ && !loading_);
}

void chat_history_dialog::show_menu(QPoint position)
{
    auto* item = list_->itemAt(position);
    if (!item)
    {
        return;
    }
    list_->setCurrentItem(item);
    auto const id = item->data(id_role).toLongLong();
    auto const found = std::ranges::find(messages_, id, &message_data::id);
    QMenu menu(this);
    connect(this, &QDialog::finished, &menu, &QMenu::close);
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
    else if (copy && chosen == copy)
    {
        QGuiApplication::clipboard()->setText(current->text);
    }
}
