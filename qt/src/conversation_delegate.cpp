#include "conversation_delegate.hpp"

#include <algorithm>

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>

#include "conversation_model.hpp"

conversation_delegate::conversation_delegate(QObject* parent) : QStyledItemDelegate(parent) {}

void conversation_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const rect = option.rect;
    if (option.state & QStyle::State_Selected)
    {
        painter->fillRect(rect, QColor(QStringLiteral("#E6EEE9")));
    }
    else if (option.state & QStyle::State_MouseOver)
    {
        painter->fillRect(rect, QColor(QStringLiteral("#F0F2EC")));
    }

    auto const username = index.data(conversation_model::username_role).toString();
    auto const last_text = index.data(conversation_model::last_text_role).toString();
    auto const unread = index.data(conversation_model::unread_role).toULongLong();

    QRect avatar_rect(rect.left() + 18, rect.top() + 14, 42, 42);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(QStringLiteral("#D7E5DC")));
    painter->drawEllipse(avatar_rect);

    QFont avatar_font = option.font;
    avatar_font.setBold(true);
    avatar_font.setPointSize(13);
    painter->setFont(avatar_font);
    painter->setPen(QColor(QStringLiteral("#315A4B")));
    auto avatar_text = username.isEmpty() ? QStringLiteral("?") : username.left(1).toUpper();
    painter->drawText(avatar_rect, Qt::AlignCenter, avatar_text);

    auto const content_left = avatar_rect.right() + 14;
    auto const right_padding = 18;
    auto const badge_space = unread > 0 ? 40 : 0;

    QFont username_font = option.font;
    username_font.setBold(true);
    painter->setFont(username_font);
    painter->setPen(QColor(QStringLiteral("#27362F")));
    QRect username_rect(content_left, rect.top() + 13, rect.width() - content_left + rect.left() - right_padding - badge_space, 22);
    painter->drawText(username_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(username_font).elidedText(username, Qt::ElideRight, username_rect.width()));

    QFont preview_font = option.font;
    preview_font.setPointSize(std::max(8, preview_font.pointSize() - 1));
    painter->setFont(preview_font);
    painter->setPen(QColor(QStringLiteral("#7B847F")));
    QRect preview_rect(content_left, rect.top() + 38, rect.width() - content_left + rect.left() - right_padding, 20);
    painter->drawText(preview_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(preview_font).elidedText(last_text, Qt::ElideRight, preview_rect.width()));

    if (unread > 0)
    {
        auto const badge_text = unread > 99 ? QStringLiteral("99+") : QString::number(unread);
        QFont badge_font = option.font;
        badge_font.setBold(true);
        badge_font.setPointSize(std::max(8, badge_font.pointSize() - 2));
        painter->setFont(badge_font);
        auto const badge_width = std::max(22, QFontMetrics(badge_font).horizontalAdvance(badge_text) + 12);
        QRect badge_rect(rect.right() - right_padding - badge_width, rect.top() + 15, badge_width, 22);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(QStringLiteral("#315A4B")));
        painter->drawRoundedRect(badge_rect, 11, 11);
        painter->setPen(QColor(QStringLiteral("#FFFFFF")));
        painter->drawText(badge_rect, Qt::AlignCenter, badge_text);
    }

    painter->setPen(QColor(QStringLiteral("#E7E5DE")));
    painter->drawLine(content_left, rect.bottom(), rect.right() - right_padding, rect.bottom());
    painter->restore();
}

QSize conversation_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    (void)option;
    (void)index;
    return {300, 70};
}
