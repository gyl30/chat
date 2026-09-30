#include "user_delegate.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>

#include "avatar.hpp"
#include "theme.hpp"
#include "user_model.hpp"

user_delegate::user_delegate(QObject* parent) : QStyledItemDelegate(parent) {}

void user_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const rect = option.rect;
    if (option.state & QStyle::State_Selected)
    {
        painter->fillRect(rect, QColor(QStringLiteral("#E7EEE9")));
    }
    else if (option.state & QStyle::State_MouseOver)
    {
        painter->fillRect(rect, QColor(QStringLiteral("#F1F3EF")));
    }

    auto const username = index.data(user_model::username_role).toString();
    QRect avatar_rect(rect.left() + chat_theme::dialog_left, rect.top() + chat_theme::dialog_avatar_top,
                      chat_theme::dialog_avatar_size, chat_theme::dialog_avatar_size);
    paint_avatar(*painter, avatar_rect, username, 17);

    QFont username_font = option.font;
    username_font.setBold(true);
    username_font.setPixelSize(14);
    painter->setFont(username_font);
    painter->setPen(QColor(QStringLiteral("#25332D")));

    auto const left = rect.left() + chat_theme::dialog_text_left;
    auto const right = rect.right() - chat_theme::dialog_right + 1;
    QRect username_rect(left, rect.top(), right - left, rect.height());
    painter->drawText(username_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(username_font).elidedText(username, Qt::ElideRight, username_rect.width()));

    painter->restore();
}

QSize user_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    (void)option;
    (void)index;
    return {314, chat_theme::dialog_row_height};
}
