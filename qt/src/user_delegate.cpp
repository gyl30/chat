#include "user_delegate.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QMouseEvent>

#include "avatar.hpp"
#include "presence.hpp"
#include "theme.hpp"
#include "user_model.hpp"

user_delegate::user_delegate(QObject* parent) : QStyledItemDelegate(parent) {}

bool user_delegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                QStyleOptionViewItem const& option, QModelIndex const& index)
{
    if (event->type() != QEvent::MouseButtonRelease)
    {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    auto* mouse = static_cast<QMouseEvent*>(event);
    QRect avatar_rect(option.rect.left() + chat_theme::dialog_left, option.rect.top() + chat_theme::dialog_avatar_top,
                      chat_theme::dialog_avatar_size, chat_theme::dialog_avatar_size);
    if (mouse->button() != Qt::LeftButton || !avatar_rect.contains(mouse->pos())) { return false; }
    emit avatar_clicked(index);
    return true;
}

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
    auto const online = index.data(user_model::online_role).toBool();
    auto const last_seen = index.data(user_model::last_seen_role).toLongLong();
    QRect avatar_rect(rect.left() + chat_theme::dialog_left, rect.top() + chat_theme::dialog_avatar_top,
                      chat_theme::dialog_avatar_size, chat_theme::dialog_avatar_size);
    paint_avatar(*painter, avatar_rect, username, 17, index.data(Qt::DecorationRole).value<QPixmap>());

    auto const left = rect.left() + chat_theme::dialog_text_left;
    auto const right = rect.right() - chat_theme::dialog_right + 1;

    QFont username_font = option.font;
    username_font.setBold(true);
    username_font.setPixelSize(14);
    painter->setFont(username_font);
    painter->setPen(QColor(QStringLiteral("#25332D")));
    QRect username_rect(left, rect.top() + chat_theme::dialog_name_top, right - left,
                        QFontMetrics(username_font).height());
    painter->drawText(username_rect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(username_font).elidedText(username, Qt::ElideRight, username_rect.width()));

    auto const status = presence_text(online, last_seen);
    if (!status.isEmpty())
    {
        QFont status_font = option.font;
        status_font.setPixelSize(13);
        painter->setFont(status_font);
        painter->setPen(QColor(online ? QStringLiteral("#4F8A70") : QStringLiteral("#858D88")));
        QRect status_rect(left, rect.top() + chat_theme::dialog_preview_top, right - left,
                          QFontMetrics(status_font).height());
        painter->drawText(status_rect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(status_font).elidedText(status, Qt::ElideRight, status_rect.width()));
    }

    painter->restore();
}

QSize user_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    (void)option;
    (void)index;
    return {314, chat_theme::dialog_row_height};
}
