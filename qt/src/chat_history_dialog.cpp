#include "chat_history_dialog.hpp"

#include <algorithm>
#include <utility>

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
// Enough records to fill the window before a filtered view stops loading older pages by itself.
constexpr int filled_rows = 15;
constexpr int max_filling_pages = 20;

QString time_text(qint64 timestamp)
{
    auto const value = QDateTime::fromMSecsSinceEpoch(timestamp);
    auto const today = QDate::currentDate();
    if (value.date() == today) { return value.toString(QStringLiteral("HH:mm")); }
    if (value.date().year() == today.year()) { return value.toString(QStringLiteral("M月d日 HH:mm")); }
    return value.toString(QStringLiteral("yyyy年M月d日 HH:mm"));
}

QString size_text(qint64 bytes)
{
    if (bytes < 1024) { return QStringLiteral("%1 B").arg(bytes); }
    if (bytes < 1024 * 1024) { return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1); }
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
            painter->setBrush(option.state & QStyle::State_Selected ? themed("#E7EEE9") : themed("#F1F3EF"));
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
        painter->setPen(themed("#8C948F"));
        painter->drawText(QRect(right - time_width, rect.top() + 12, time_width, 18), Qt::AlignRight | Qt::AlignVCenter, time);
        painter->setFont(name_font);
        painter->setPen(themed("#5D6C64"));
        QRect name(left, rect.top() + 12, right - time_width - 12 - left, 18);
        painter->drawText(name, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(name_font).elidedText(sender, Qt::ElideRight, name.width()));
        QFont content_font = option.font;
        content_font.setPixelSize(14);
        painter->setFont(content_font);
        painter->setPen(themed("#27332E"));
        QRect content(left, rect.top() + 34, right - left, 22);
        painter->drawText(content, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(content_font).elidedText(index.data(content_role).toString(), Qt::ElideRight,
                                                               content.width()));
        painter->restore();
    }

    QSize sizeHint(QStyleOptionViewItem const&, QModelIndex const&) const override { return {480, row_height}; }

   private:
    avatar_cache* avatars_;
};

}    // namespace

bool chat_history_dialog::matches(message_data const& message, category kind)
{
    if (message.deleted) { return false; }
    auto const image = message.attachment && message.attachment->media_type.startsWith(QStringLiteral("image/"));
    switch (kind)
    {
        case category::all: return true;
        case category::images: return image;
        case category::files: return message.attachment.has_value() && !image;
        case category::links:
            return message.text.contains(QStringLiteral("http://")) || message.text.contains(QStringLiteral("https://"));
    }
    return false;
}

chat_history_dialog::chat_history_dialog(qint64 conversation, QString const& title, QWidget* parent, avatar_cache* avatars)
    : QDialog(parent), conversation_(conversation), avatars_(avatars)
{
    setObjectName(QStringLiteral("chatHistoryDialog"));
    setWindowTitle(QStringLiteral("聊天记录 · %1").arg(title));
    resize(560, 640);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                              chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("historySearchEdit"));
    search_->setPlaceholderText(QStringLiteral("搜索"));
    search_->setAccessibleName(QStringLiteral("搜索聊天记录"));
    search_->setClearButtonEnabled(true);
    search_->addAction(svg_icon(u"search", QColor(QStringLiteral("#8B918D")), QSize(16, 16)), QLineEdit::LeadingPosition);
    layout->addWidget(search_);
    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("historyTabs"));
    tabs_->setDrawBase(false);
    tabs_->setExpanding(false);
    for (auto const* name : {"全部", "图片", "文件", "链接"}) { tabs_->addTab(QString::fromUtf8(name)); }
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
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_, 1);
    layout->addWidget(status_);
    more_ = new QPushButton(QStringLiteral("加载更早的记录"), this);
    more_->setObjectName(QStringLiteral("historyMoreButton"));
    more_->hide();
    layout->addWidget(more_, 0, Qt::AlignHCenter);

    connect(search_, &QLineEdit::returnPressed, this, [this] {
        query_ = search_->text().trimmed();
        messages_.clear();
        has_more_ = true;
        filling_pages_ = 0;
        rebuild();
        request_more();
    });
    connect(search_, &QLineEdit::textChanged, this, [this](QString const& text) {
        // Clearing the search returns to browsing the whole history.
        if (text.trimmed().isEmpty() && !query_.isEmpty())
        {
            query_.clear();
            messages_.clear();
            has_more_ = true;
            filling_pages_ = 0;
            rebuild();
            request_more();
        }
    });
    connect(tabs_, &QTabBar::currentChanged, this, [this] {
        filling_pages_ = 0;
        rebuild();
        fill_sparse_category();
    });
    connect(more_, &QPushButton::clicked, this, [this] { request_more(); });
    connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (value == list_->verticalScrollBar()->maximum() && value > 0) { request_more(); }
    });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        emit message_activated(item->data(id_role).toLongLong());
    });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](QPoint position) { show_menu(position); });
    if (avatars_) { connect(avatars_, &avatar_cache::changed, list_->viewport(), [this] { list_->viewport()->update(); }); }
    rebuild();
}

void chat_history_dialog::start() { request_more(); }

void chat_history_dialog::request_more()
{
    if (loading_ || !has_more_) { return; }
    loading_ = true;
    auto const before = messages_.isEmpty() ? 0 : messages_.back().id;
    rebuild();
    if (query_.isEmpty()) { emit history_requested(before); }
    else { emit search_requested(query_, before); }
}

void chat_history_dialog::set_history(qint64 conversation, qint64 before, QList<message_data> messages, bool has_more,
                                      QString const& error)
{
    auto const expected = messages_.isEmpty() ? 0 : messages_.back().id;
    if (conversation != conversation_ || !query_.isEmpty() || before != expected) { return; }
    loading_ = false;
    if (!error.isEmpty())
    {
        status_->setText(error);
        status_->show();
        return;
    }
    add_messages(std::move(messages), has_more);
}

void chat_history_dialog::set_search_results(qint64 conversation, QString const& query, qint64 before,
                                             QList<message_data> messages, read_positions, bool has_more,
                                             QString const& error)
{
    auto const expected = messages_.isEmpty() ? 0 : messages_.back().id;
    if (conversation != conversation_ || query != query_ || query_.isEmpty() || before != expected) { return; }
    loading_ = false;
    if (!error.isEmpty())
    {
        status_->setText(error);
        status_->show();
        return;
    }
    add_messages(std::move(messages), has_more);
}

void chat_history_dialog::add_messages(QList<message_data> messages, bool has_more)
{
    // Pages arrive oldest-first; the window lists the newest first.
    std::ranges::sort(messages, [](auto const& a, auto const& b) { return a.id > b.id; });
    for (auto& message : messages)
    {
        if (messages_.isEmpty() || message.id < messages_.back().id) { messages_.push_back(std::move(message)); }
    }
    has_more_ = has_more && !messages.isEmpty();
    rebuild();
    fill_sparse_category();
}

void chat_history_dialog::fill_sparse_category()
{
    // A narrow category may match little in the newest pages; keep reading back, within a bound.
    if (list_->count() < filled_rows && has_more_ && !loading_ && filling_pages_ < max_filling_pages)
    {
        ++filling_pages_;
        request_more();
    }
}

void chat_history_dialog::rebuild()
{
    auto const kind = static_cast<category>(tabs_->currentIndex());
    auto const selected = list_->currentItem() ? list_->currentItem()->data(id_role).toLongLong() : 0;
    auto const scroll = list_->verticalScrollBar()->value();
    list_->clear();
    for (auto const& message : messages_)
    {
        if (!matches(message, kind)) { continue; }
        auto* item = new QListWidgetItem(list_);
        item->setData(id_role, message.id);
        item->setData(sender_role, message.username);
        item->setData(from_role, message.from);
        item->setData(time_role, time_text(message.timestamp));
        item->setData(content_role, content_text(message));
        item->setData(Qt::AccessibleTextRole, message.username + QStringLiteral("，") + time_text(message.timestamp) +
                                                 QStringLiteral("：") + content_text(message));
        item->setToolTip(content_text(message));
        if (message.id == selected) { list_->setCurrentItem(item); }
    }
    list_->verticalScrollBar()->setValue(scroll);
    if (loading_ && list_->count() == 0) { status_->setText(QStringLiteral("正在加载…")); }
    else if (list_->count() == 0)
    {
        status_->setText(!query_.isEmpty() ? QStringLiteral("没有找到相关记录")
            : kind == category::images ? QStringLiteral("没有图片")
            : kind == category::files ? QStringLiteral("没有文件")
            : kind == category::links ? QStringLiteral("没有链接") : QStringLiteral("还没有聊天记录"));
    }
    else { status_->clear(); }
    status_->setVisible(!status_->text().isEmpty());
    more_->setVisible(has_more_ && !loading_ && list_->count() > 0);
}

void chat_history_dialog::show_menu(QPoint position)
{
    auto* item = list_->itemAt(position);
    if (!item) { return; }
    list_->setCurrentItem(item);
    auto const id = item->data(id_role).toLongLong();
    auto const found = std::ranges::find(messages_, id, &message_data::id);
    QMenu menu(this);
    auto* locate = menu.addAction(QStringLiteral("定位到聊天位置"));
    auto* copy = found != messages_.end() && !found->text.isEmpty() ? menu.addAction(QStringLiteral("复制")) : nullptr;
    auto* chosen = menu.exec(list_->viewport()->mapToGlobal(position));
    if (chosen == locate) { emit message_activated(id); }
    else if (copy && chosen == copy) { QGuiApplication::clipboard()->setText(found->text); }
}
