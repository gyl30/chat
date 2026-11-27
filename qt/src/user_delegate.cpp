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
QRect user_avatar_rect(QRect const& row, user_delegate::layout mode)
{
    auto const avatar_size = mode == user_delegate::layout::compact ? 40 : 48;
    return {row.left() + chat_theme::dialog_left, row.top() + (row.height() - avatar_size) / 2, avatar_size, avatar_size};
}
}    // namespace

user_delegate::user_delegate(QObject* parent, std::function<QString(QModelIndex const&)> status, layout mode)
    : QStyledItemDelegate(parent), status_(std::move(status)), mode_(mode)
{
}

bool user_delegate::editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index)
{
    if (mode_ == layout::picker || event->type() != QEvent::MouseButtonRelease)
    {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton)
    {
        return false;
    }
    if (user_avatar_rect(option.rect, mode_).contains(mouse->position().toPoint()))
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
    const bool selected = mode_ != layout::picker && option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const auto surface = themed(selected ? "selected" : hovered ? "canvas_hover" : "panel");
    if (selected || hovered)
    {
        painter->fillRect(rect, surface);
    }

    auto const username = index.data(Qt::DisplayRole).toString();
    auto const online = index.data(user_model::online_role).toBool();
    auto const last_seen = index.data(user_model::last_seen_role).toLongLong();
    const auto avatar_rect = user_avatar_rect(rect, mode_);
    paint_avatar(*painter, avatar_rect, username, mode_ == layout::compact ? 17 : 19, index.data(Qt::DecorationRole).value<QPixmap>());
    if (online)
    {
        painter->setPen(QPen(surface, 2));
        painter->setBrush(themed("online"));
        painter->drawEllipse(QPointF(avatar_rect.right() - 2, avatar_rect.bottom() - 2), 4, 4);
    }

    auto const left = avatar_rect.right() + 12;
    auto right = rect.right() - chat_theme::dialog_right + 1;
    auto const badge = index.data(badge_role).toString();
    const auto request_status = index.data(Qt::StatusTipRole);
    const auto status = status_ ? status_(index) : request_status.isValid() ? request_status.toString() : presence_text(online, last_seen);

    QFont username_font = option.font;
    username_font.setWeight(mode_ == layout::compact ? QFont::Bold : QFont::Medium);
    username_font.setPixelSize(mode_ == layout::compact ? 14 : 15);
    QFont status_font = option.font;
    status_font.setPixelSize(13);
    if (!badge.isEmpty())
    {
        auto const width = QFontMetrics(status_font).horizontalAdvance(badge);
        paint_emoji_line(*painter, {right - width, rect.top(), width + 1, rect.height()}, badge, status_font, themed("text_secondary"));
        right -= width + 12;
    }
    if (mode_ == layout::picker)
    {
        auto const center = QPointF(right - 11, rect.center().y());
        bool const checked = index.data(chosen_role).toBool();
        painter->setPen(QPen(themed(checked ? "accent" : "text_muted"), 1.4));
        painter->setBrush(checked ? QBrush(themed("accent")) : Qt::NoBrush);
        painter->drawEllipse(center, 9.5, 9.5);
        if (checked)
        {
            painter->setPen(QPen(themed("on_accent"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPointF const path[]{center + QPointF(-4, 0), center + QPointF(-1, 3), center + QPointF(4.5, -3)};
            painter->drawPolyline(path, 3);
        }
        right -= 35;
    }
    const auto name_height = QFontMetrics(username_font).height();
    const auto status_height = status.isEmpty() ? 0 : QFontMetrics(status_font).height();
    const auto top = rect.center().y() - (name_height + status_height) / 2;
    paint_emoji_line(*painter, {left, top, right - left, name_height}, username, username_font, themed("text"));
    if (!status.isEmpty())
    {
        paint_emoji_line(
            *painter, {left, top + name_height, right - left, status_height}, status, status_font, themed(online ? "online" : "text_secondary"));
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
    return {314, mode_ == layout::compact ? chat_theme::user_row_height : chat_theme::member_row_height};
}
