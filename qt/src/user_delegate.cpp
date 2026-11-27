#include "user_delegate.hpp"
#include "theme_manager.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QMouseEvent>

#include "avatar.hpp"
#include "emoji_text.hpp"
#include "presence.hpp"
#include "theme.hpp"
#include "user_model.hpp"

namespace
{
constexpr int avatar_size = 40;

QRect user_avatar_rect(QRect const& row)
{
    return {row.left() + chat_theme::dialog_left, row.top() + (row.height() - avatar_size) / 2, avatar_size, avatar_size};
}
}

user_delegate::user_delegate(QObject* parent, std::function<QString(QModelIndex const&)> status)
    : QStyledItemDelegate(parent), status_(std::move(status))
{
}

bool user_delegate::editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index)
{
    if (event->type() != QEvent::MouseButtonRelease)
    {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton)
    {
        return false;
    }
    if (user_avatar_rect(option.rect).contains(mouse->position().toPoint()))
    {
        emit avatar_clicked(index);
    }
    else
    {
        emit body_clicked(index);
    }
    return true;
}

void user_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const rect = option.rect;
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const auto surface = themed(selected ? "selected" : hovered ? "canvas_hover" : "panel");
    if (selected || hovered)
    {
        painter->fillRect(rect, surface);
    }

    auto const username = index.data(Qt::DisplayRole).toString();
    auto const online = index.data(user_model::online_role).toBool();
    auto const last_seen = index.data(user_model::last_seen_role).toLongLong();
    const auto avatar_rect = user_avatar_rect(rect);
    paint_avatar(*painter, avatar_rect, username, 17, index.data(Qt::DecorationRole).value<QPixmap>());
    if (online)
    {
        painter->setPen(QPen(surface, 2));
        painter->setBrush(themed("online"));
        painter->drawEllipse(QPointF(avatar_rect.right() - 2, avatar_rect.bottom() - 2), 4, 4);
    }

    auto const left = avatar_rect.right() + 12;
    auto const right = rect.right() - chat_theme::dialog_right + 1;
    const auto request_status = index.data(Qt::StatusTipRole);
    const auto status = status_ ? status_(index) : request_status.isValid() ? request_status.toString() : presence_text(online, last_seen);

    QFont username_font = option.font;
    username_font.setBold(true);
    username_font.setPixelSize(14);
    QFont status_font = option.font;
    status_font.setPixelSize(13);
    const auto name_height = QFontMetrics(username_font).height();
    const auto status_height = status.isEmpty() ? 0 : QFontMetrics(status_font).height();
    const auto top = rect.center().y() - (name_height + status_height) / 2;
    paint_emoji_line(*painter, {left, top, right - left, name_height}, username, username_font, themed("text"));
    if (!status.isEmpty())
    {
        paint_emoji_line(*painter, {left, top + name_height, right - left, status_height}, status, status_font, themed("text_secondary"));
    }

    if (option.state.testFlag(QStyle::State_HasFocus) && !selected)
    {
        painter->setPen(QPen(themed("focus"), 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(rect.adjusted(0, 0, -1, -1));
    }
    painter->restore();
}

QSize user_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    (void)option;
    (void)index;
    return {314, chat_theme::user_row_height};
}
