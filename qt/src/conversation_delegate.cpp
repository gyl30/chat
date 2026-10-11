#include "conversation_delegate.hpp"
#include "theme_manager.hpp"

#include <algorithm>

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QLocale>
#include <QPainter>

#include "avatar.hpp"
#include "conversation_model.hpp"
#include "emoji_text.hpp"
#include "theme.hpp"

namespace
{

QString timestamp_text(qint64 timestamp)
{
    if (timestamp <= 0)
    {
        return {};
    }

    auto const value = QDateTime::fromMSecsSinceEpoch(timestamp).toLocalTime();
    auto const today = QDate::currentDate();
    auto const date = value.date();
    if (date == today)
    {
        return value.toString(QStringLiteral("HH:mm"));
    }
    if (date.daysTo(today) >= 0 && date.daysTo(today) < 7)
    {
        return QLocale().toString(date, QStringLiteral("ddd"));
    }
    if (date.year() == today.year())
    {
        return QStringLiteral("%1/%2").arg(date.month()).arg(date.day());
    }
    return QStringLiteral("%1/%2/%3").arg(date.year()).arg(date.month()).arg(date.day());
}

}    // namespace

conversation_delegate::conversation_delegate(QObject* parent) : QStyledItemDelegate(parent) {}

void conversation_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const rect = option.rect;
    // Selection is an inset rounded card rather than a full-width band.
    auto const card = QRectF(rect).adjusted(6, 2, -6, -2);
    if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))
    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(option.state & QStyle::State_Selected ? themed("#E7EEE9") : themed("#F1F3EF"));
        painter->drawRoundedRect(card, 12, 12);
    }

    auto const username = index.data(conversation_model::username_role).toString();
    auto const last_text = index.data(conversation_model::last_text_role).toString();
    auto const last_timestamp = index.data(conversation_model::last_timestamp_role).toLongLong();
    auto const unread = index.data(conversation_model::unread_role).toULongLong();
    auto const group = index.data(conversation_model::group_role).toBool();
    auto const online = index.data(conversation_model::online_role).toBool();
    auto const muted = index.data(conversation_model::muted_role).toBool();
    auto const pinned = index.data(conversation_model::pinned_role).toBool();

    QRect avatar_rect(rect.left() + chat_theme::dialog_left, rect.top() + chat_theme::dialog_avatar_top,
                      chat_theme::dialog_avatar_size, chat_theme::dialog_avatar_size);
    paint_avatar(*painter, avatar_rect, group ? QStringLiteral("群") : username, 17, index.data(Qt::DecorationRole).value<QPixmap>());
    if (online)
    {
        painter->setPen(QPen(themed("#FCFBF7"), 2));
        painter->setBrush(themed("#4F8A70"));
        painter->drawEllipse(QPointF(avatar_rect.right() - 3, avatar_rect.bottom() - 3), 5, 5);
    }

    auto const content_left = rect.left() + chat_theme::dialog_text_left;
    auto const content_right = rect.right() - chat_theme::dialog_right + 1;

    QFont date_font = option.font;
    date_font.setPixelSize(13);
    painter->setFont(date_font);
    auto const date = timestamp_text(last_timestamp);
    auto const date_width = date.isEmpty() ? 0 : QFontMetrics(date_font).horizontalAdvance(date);
    if (!date.isEmpty())
    {
        painter->setPen(themed("#8C948F"));
        QRect date_rect(content_right - date_width, rect.top() + chat_theme::dialog_name_top, date_width,
                        QFontMetrics(date_font).height());
        painter->drawText(date_rect, Qt::AlignRight | Qt::AlignVCenter, date);
    }

    QFont username_font = option.font;
    username_font.setBold(true);
    username_font.setPixelSize(14);
    painter->setFont(username_font);
    painter->setPen(themed("#25332D"));
    auto name_right = content_right - (date_width > 0 ? date_width + 8 : 0);
    if (muted || pinned)
    {
        auto const label = muted && pinned ? QStringLiteral("置顶 静音") :
                           pinned ? QStringLiteral("置顶") : QStringLiteral("静音");
        painter->setFont(date_font);
        painter->setPen(themed("#8C948F"));
        auto const label_width = QFontMetrics(date_font).horizontalAdvance(label);
        painter->drawText(QRect(name_right - label_width, rect.top() + chat_theme::dialog_name_top,
                                label_width, QFontMetrics(date_font).height()), Qt::AlignVCenter, label);
        name_right -= label_width + 8;
        painter->setFont(username_font);
        painter->setPen(themed("#25332D"));
    }
    QRect username_rect(content_left, rect.top() + chat_theme::dialog_name_top,
                        std::max(0, name_right - content_left), QFontMetrics(username_font).height());
    auto const name_padding = std::max(0, (chat_theme::dialog_preview_top - chat_theme::dialog_name_top - username_rect.height()) / 2);
    username_rect.adjust(0, -name_padding, 0, name_padding);
    paint_emoji_line(*painter, username_rect, username, username_font, themed("#25332D"));

    QFont preview_font = option.font;
    preview_font.setPixelSize(13);
    painter->setFont(preview_font);
    painter->setPen(themed("#858D88"));

    auto badge_width = 0;
    QString badge_text;
    if (unread > 0)
    {
        badge_text = unread > 99 ? QStringLiteral("99+") : QString::number(unread);
        QFont badge_font = option.font;
        badge_font.setBold(true);
        badge_font.setPixelSize(11);
        badge_width = std::max(chat_theme::dialog_unread_height,
                               QFontMetrics(badge_font).horizontalAdvance(badge_text) + 10);
    }

    auto const preview_right = content_right - (badge_width > 0 ? badge_width + 8 : 0);
    QRect preview_rect(content_left, rect.top() + chat_theme::dialog_preview_top,
                       std::max(0, preview_right - content_left), QFontMetrics(preview_font).height());
    auto const preview_padding = std::max(0, (chat_theme::dialog_row_height - chat_theme::dialog_preview_top -
                                             chat_theme::dialog_avatar_top - preview_rect.height()) / 2);
    preview_rect.adjust(0, -preview_padding, 0, preview_padding);
    paint_emoji_line(*painter, preview_rect, last_text, preview_font, themed("#858D88"));

    if (unread > 0)
    {
        QFont badge_font = option.font;
        badge_font.setBold(true);
        badge_font.setPixelSize(11);
        painter->setFont(badge_font);
        QRect badge_rect(content_right - badge_width,
                         rect.top() + chat_theme::dialog_preview_top,
                         badge_width,
                         chat_theme::dialog_unread_height);
        painter->setPen(Qt::NoPen);
        // A muted conversation still counts, but quietly, as in WeChat.
        painter->setBrush(muted ? themed("#AEBDB6") : theme_manager::instance().highlight());
        painter->drawRoundedRect(badge_rect, chat_theme::dialog_unread_height / 2.0,
                                 chat_theme::dialog_unread_height / 2.0);
        painter->setPen(muted && theme_manager::instance().dark()
                            ? themed("#27332E") : theme_manager::instance().on_accent());
        painter->drawText(badge_rect, Qt::AlignCenter, badge_text);
    }

    painter->restore();
}

QSize conversation_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    (void)option;
    (void)index;
    return {314, chat_theme::dialog_row_height};
}

bool conversation_delegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                         QStyleOptionViewItem const& option, QModelIndex const& index)
{
    (void)model;
    if (event->type() != QEvent::MouseButtonRelease)
    {
        return false;
    }

    auto const* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton)
    {
        return false;
    }

    QRect avatar_rect(option.rect.left() + chat_theme::dialog_left,
                      option.rect.top() + chat_theme::dialog_avatar_top,
                      chat_theme::dialog_avatar_size, chat_theme::dialog_avatar_size);
    if (avatar_rect.contains(mouse->position().toPoint())) { emit avatar_clicked(index); }
    else { emit body_clicked(index); }
    return true;
}
