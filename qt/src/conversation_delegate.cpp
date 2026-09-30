#include "conversation_delegate.hpp"

#include <algorithm>

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include "avatar.hpp"
#include "conversation_model.hpp"

conversation_delegate::conversation_delegate(QObject* parent) : QStyledItemDelegate(parent) {}

void conversation_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const rect = option.rect.adjusted(6, 3, -6, -3);
    if (option.state & QStyle::State_Selected)
    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(QStringLiteral("#E7EEE9")));
        painter->drawRoundedRect(rect, 14, 14);
    }
    else if (option.state & QStyle::State_MouseOver)
    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(QStringLiteral("#F1F2ED")));
        painter->drawRoundedRect(rect, 14, 14);
    }

    auto const username = index.data(conversation_model::username_role).toString();
    auto const last_text = index.data(conversation_model::last_text_role).toString();
    auto const unread = index.data(conversation_model::unread_role).toULongLong();

    QRect avatar_rect(rect.left() + 12, rect.top() + 11, 46, 46);
    paint_avatar(*painter, avatar_rect, username, 17);

    auto const content_left = avatar_rect.right() + 13;
    auto const right_padding = 13;
    auto const badge_space = unread > 0 ? 42 : 0;

    QFont username_font = option.font;
    username_font.setBold(true);
    username_font.setPointSize(std::max(10, username_font.pointSize()));
    painter->setFont(username_font);
    painter->setPen(QColor(QStringLiteral("#25332D")));
    QRect username_rect(content_left, rect.top() + 11,
                        rect.right() - content_left - right_padding - badge_space + 1, 22);
    painter->drawText(username_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(username_font).elidedText(username, Qt::ElideRight, username_rect.width()));

    QFont preview_font = option.font;
    preview_font.setPointSize(std::max(9, preview_font.pointSize() - 1));
    painter->setFont(preview_font);
    painter->setPen(QColor(QStringLiteral("#858C88")));
    QRect preview_rect(content_left, rect.top() + 35, rect.right() - content_left - right_padding + 1, 21);
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
        QRect badge_rect(rect.right() - right_padding - badge_width, rect.top() + 13, badge_width, 22);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(QStringLiteral("#315A4B")));
        painter->drawRoundedRect(badge_rect, 11, 11);
        painter->setPen(QColor(QStringLiteral("#FFFFFF")));
        painter->drawText(badge_rect, Qt::AlignCenter, badge_text);
    }

    painter->restore();
}

QSize conversation_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    (void)option;
    (void)index;
    return {314, 74};
}
