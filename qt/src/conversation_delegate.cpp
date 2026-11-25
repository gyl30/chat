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
#include "icons.hpp"
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

conversation_delegate::conversation_delegate(QObject* parent) : QStyledItemDelegate(parent)
{
    auto update_icons = [this]
    {
        pin_ = svg_icon(u"pin", themed("text_secondary"), {16, 16});
        expanded_ = svg_icon(u"chevron-down", themed("text_secondary"), {16, 16});
        collapsed_ = svg_icon(u"chevron-right", themed("text_secondary"), {16, 16});
        mute_ = svg_icon(u"bell-off", themed("text_secondary"), {16, 16});
    };
    update_icons();
    connect(&theme_manager::instance(), &theme_manager::changed, this, update_icons);
}

void conversation_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    if (index.data(conversation_model::folder_header_role).toBool())
    {
        painter->save();
        auto const row = option.rect.adjusted(6, 2, -6, -2);
        if (option.state & (QStyle::State_MouseOver | QStyle::State_HasFocus))
        {
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setBrush(themed("subtle"));
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(row, 8, 8);
        }
        QRect arrow(option.rect.left() + chat_theme::dialog_left + (chat_theme::dialog_avatar_size - 16) / 2, row.center().y() - 7, 16, 16);
        (index.data(conversation_model::collapsed_role).toBool() ? collapsed_ : expanded_).paint(painter, arrow);
        auto const label = row.adjusted(arrow.right() + 7 - row.left(), 0, -8, 0);
        painter->setFont(option.font);
        painter->setPen(themed("text_secondary"));
        painter->drawText(
            label, Qt::AlignLeft | Qt::AlignVCenter, option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, label.width()));
        painter->restore();
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const rect = option.rect;
    // Selection is an inset rounded card rather than a full-width band.
    auto const card = QRectF(rect).adjusted(6, 2, -6, -2);
    if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))
    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(option.state & QStyle::State_Selected ? themed("selected") : themed("subtle"));
        painter->drawRoundedRect(card, 12, 12);
    }
    if (option.state & QStyle::State_Selected)
    {
        painter->setPen(Qt::NoPen);
        painter->setBrush(themed("accent"));
        painter->drawRoundedRect(QRectF(card.left() + 1, card.top() + 12, 3, card.height() - 24), 1.5, 1.5);
    }

    auto const username = index.data(Qt::DisplayRole).toString();
    auto const last_text = index.data(conversation_model::last_text_role).toString();
    auto const last_timestamp = index.data(conversation_model::last_timestamp_role).toLongLong();
    auto const unread = index.data(conversation_model::unread_role).toULongLong();
    auto const group = index.data(conversation_model::group_role).toBool();
    auto const online = index.data(conversation_model::online_role).toBool();
    auto const muted = index.data(conversation_model::muted_role).toBool();
    auto const pinned = index.data(conversation_model::pinned_role).toBool();

    QRect avatar_rect(rect.left() + chat_theme::dialog_left,
                      rect.top() + chat_theme::dialog_avatar_top,
                      chat_theme::dialog_avatar_size,
                      chat_theme::dialog_avatar_size);
    paint_avatar(*painter, avatar_rect, group ? QStringLiteral("群") : username, 17, index.data(Qt::DecorationRole).value<QPixmap>());
    if (online)
    {
        painter->setPen(QPen(themed("panel"), 2));
        painter->setBrush(themed("online"));
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
        painter->setPen(themed("text_muted"));
        QRect date_rect(content_right - date_width, rect.top() + chat_theme::dialog_name_top, date_width, QFontMetrics(date_font).height());
        painter->drawText(date_rect, Qt::AlignRight | Qt::AlignVCenter, date);
    }

    QFont username_font = option.font;
    username_font.setBold(true);
    username_font.setPixelSize(14);
    painter->setFont(username_font);
    painter->setPen(themed("text"));
    auto name_right = content_right - (date_width > 0 ? date_width + 8 : 0);
    auto paint_status = [&](QIcon const& icon)
    {
        constexpr int side = 14;
        QRect area(name_right - side, rect.top() + chat_theme::dialog_name_top + (QFontMetrics(username_font).height() - side) / 2, side, side);
        icon.paint(painter, area);
        name_right -= side + 3;
    };
    if (muted)
    {
        paint_status(mute_);
    }
    if (pinned)
    {
        paint_status(pin_);
    }
    QRect username_rect(
        content_left, rect.top() + chat_theme::dialog_name_top, std::max(0, name_right - content_left), QFontMetrics(username_font).height());
    auto const name_padding = std::max(0, (chat_theme::dialog_preview_top - chat_theme::dialog_name_top - username_rect.height()) / 2);
    username_rect.adjust(0, -name_padding, 0, name_padding);
    paint_emoji_line(*painter, username_rect, username, username_font, themed("text"));

    QFont preview_font = option.font;
    preview_font.setPixelSize(13);
    painter->setFont(preview_font);
    painter->setPen(themed("text_muted"));

    auto badge_width = 0;
    QString badge_text;
    if (unread > 0 || index.data(conversation_model::marked_role).toBool())
    {
        badge_text = unread == 0 ? QStringLiteral("•") : unread > 99 ? QStringLiteral("99+") : QString::number(unread);
        QFont badge_font = option.font;
        badge_font.setBold(true);
        badge_font.setPixelSize(11);
        if (index.data(conversation_model::unread_mention_role).toBool())
        {
            badge_text.prepend(u'@');
        }
        badge_width = std::max(chat_theme::dialog_unread_height, QFontMetrics(badge_font).horizontalAdvance(badge_text) + 10);
    }

    auto const preview_right = content_right - (badge_width > 0 ? badge_width + 8 : 0);
    QRect preview_rect(
        content_left, rect.top() + chat_theme::dialog_preview_top, std::max(0, preview_right - content_left), QFontMetrics(preview_font).height());
    auto const preview_padding =
        std::max(0, (chat_theme::dialog_row_height - chat_theme::dialog_preview_top - chat_theme::dialog_avatar_top - preview_rect.height()) / 2);
    preview_rect.adjust(0, -preview_padding, 0, preview_padding);
    paint_emoji_line(*painter, preview_rect, last_text, preview_font, themed("text_muted"));

    if (unread > 0 || index.data(conversation_model::marked_role).toBool())
    {
        QFont badge_font = option.font;
        badge_font.setBold(true);
        badge_font.setPixelSize(11);
        painter->setFont(badge_font);
        QRect badge_rect(content_right - badge_width, rect.top() + chat_theme::dialog_preview_top, badge_width, chat_theme::dialog_unread_height);
        painter->setPen(Qt::NoPen);
        // A muted conversation still counts, but quietly, as in WeChat.
        painter->setBrush(muted ? themed("accent_disabled") : theme_manager::instance().highlight());
        painter->drawRoundedRect(badge_rect, chat_theme::dialog_unread_height / 2.0, chat_theme::dialog_unread_height / 2.0);
        painter->setPen(muted ? themed("text") : themed("badge_text"));
        painter->drawText(badge_rect, Qt::AlignCenter, badge_text);
    }

    if (option.state & QStyle::State_HasFocus)
    {
        painter->setPen(QPen(themed("focus"), 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(card, 12, 12);
    }
    painter->restore();
}

QSize conversation_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    (void)option;
    if (index.data(conversation_model::folder_header_role).toBool())
    {
        return {314, 30};
    }
    return {314, chat_theme::dialog_row_height};
}

bool conversation_delegate::editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index)
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
                      chat_theme::dialog_avatar_size,
                      chat_theme::dialog_avatar_size);
    if (!index.data(conversation_model::folder_header_role).toBool() && avatar_rect.contains(mouse->position().toPoint()))
    {
        emit avatar_clicked(index);
    }
    else
    {
        emit body_clicked(index);
    }
    return true;
}
