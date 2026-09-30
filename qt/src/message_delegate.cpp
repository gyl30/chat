#include "message_delegate.hpp"

#include <algorithm>

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include "avatar.hpp"
#include "message_model.hpp"

namespace
{

constexpr int kAvatarSize = 38;
constexpr int kAvatarGap = 10;
constexpr int kSideMargin = 16;
constexpr int kTailSpace = 6;
constexpr int kHorizontalPadding = 14;
constexpr int kVerticalPadding = 9;
constexpr int kGroupGap = 8;
constexpr int kMessageGap = 2;
constexpr int kHeaderHeight = 18;
constexpr int kHeaderGap = 3;
constexpr int kFooterHeight = 15;
constexpr int kFooterGap = 2;
constexpr int kGroupEndGap = 5;
constexpr int kMessageEndGap = 1;
constexpr int kDateHeaderTopGap = 4;
constexpr int kDateHeaderHeight = 20;
constexpr int kDateHeaderBottomGap = 4;
constexpr qint64 kGroupInterval = 5 * 60 * 1000;

bool outgoing_at(QModelIndex const& index)
{
    return index.data(message_model::outgoing_role).toBool();
}

qint64 timestamp_at(QModelIndex const& index)
{
    return index.data(message_model::timestamp_role).toLongLong();
}

QDate local_date(QModelIndex const& index)
{
    auto const timestamp = timestamp_at(index);
    if (timestamp <= 0)
    {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(timestamp).toLocalTime().date();
}

bool same_group(QModelIndex const& lhs, QModelIndex const& rhs)
{
    if (!lhs.isValid() || !rhs.isValid() || outgoing_at(lhs) != outgoing_at(rhs))
    {
        return false;
    }

    auto const lhs_timestamp = timestamp_at(lhs);
    auto const rhs_timestamp = timestamp_at(rhs);
    return lhs_timestamp > 0 && rhs_timestamp >= lhs_timestamp && local_date(lhs) == local_date(rhs)
        && rhs_timestamp - lhs_timestamp <= kGroupInterval;
}

bool starts_day(QModelIndex const& index)
{
    if (index.row() == 0)
    {
        return true;
    }

    auto const date = local_date(index);
    auto const previous = local_date(index.sibling(index.row() - 1, index.column()));
    return date.isValid() && date != previous;
}

QString date_text(QModelIndex const& index)
{
    auto const date = local_date(index);
    if (!date.isValid())
    {
        return {};
    }

    auto const today = QDate::currentDate();
    if (date == today)
    {
        return QStringLiteral("今天");
    }
    if (date == today.addDays(-1))
    {
        return QStringLiteral("昨天");
    }
    if (date.year() == today.year())
    {
        return QStringLiteral("%1月%2日").arg(date.month()).arg(date.day());
    }
    return QStringLiteral("%1年%2月%3日").arg(date.year()).arg(date.month()).arg(date.day());
}

bool starts_group(QModelIndex const& index)
{
    if (index.row() == 0)
    {
        return true;
    }
    return !same_group(index.sibling(index.row() - 1, index.column()), index);
}

bool ends_group(QModelIndex const& index)
{
    return !same_group(index, index.sibling(index.row() + 1, index.column()));
}

QString time_text(QModelIndex const& index)
{
    auto const timestamp = timestamp_at(index);
    if (timestamp <= 0)
    {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(timestamp).toLocalTime().toString(QStringLiteral("HH:mm"));
}

int maximum_text_width(QStyleOptionViewItem const& option)
{
    auto const width = std::max(320, option.rect.width());
    auto const available = width - kSideMargin * 2 - kAvatarSize - kAvatarGap - kTailSpace;
    return std::max(180, std::min(static_cast<int>(width * 0.62), available));
}

QRect text_rect(QStyleOptionViewItem const& option, QString const& text)
{
    auto const maximum_width = maximum_text_width(option);
    auto bounds = QFontMetrics(option.font).boundingRect(QRect(0, 0, maximum_width, 10000), Qt::TextWordWrap, text);
    bounds.setWidth(std::min(maximum_width, std::max(36, bounds.width())));
    return bounds;
}

QPainterPath bubble_path(QRect const& bubble, bool outgoing, bool tail)
{
    constexpr qreal radius = 14.0;
    constexpr qreal tail_width = 6.0;
    constexpr qreal tail_height = 9.0;

    QPainterPath path;
    path.addRoundedRect(QRectF(bubble), radius, radius);
    if (!tail)
    {
        return path;
    }

    QPainterPath tip;
    if (outgoing)
    {
        auto const right = static_cast<qreal>(bubble.right()) + 0.5;
        auto const bottom = static_cast<qreal>(bubble.bottom()) + 0.5;
        tip.moveTo(right - 3.0, bottom - tail_height);
        tip.cubicTo(right - 1.5, bottom - 4.5, right + 0.5, bottom - 1.5, right + tail_width, bottom);
        tip.cubicTo(right + 1.5, bottom + 0.3, right - 1.0, bottom - 0.3, right - 4.0, bottom - 2.0);
        tip.closeSubpath();
    }
    else
    {
        auto const left = static_cast<qreal>(bubble.left()) - 0.5;
        auto const bottom = static_cast<qreal>(bubble.bottom()) + 0.5;
        tip.moveTo(left + 3.0, bottom - tail_height);
        tip.cubicTo(left + 1.5, bottom - 4.5, left - 0.5, bottom - 1.5, left - tail_width, bottom);
        tip.cubicTo(left - 1.5, bottom + 0.3, left + 1.0, bottom - 0.3, left + 4.0, bottom - 2.0);
        tip.closeSubpath();
    }
    return path.united(tip);
}

}    // namespace

message_delegate::message_delegate(QObject* parent) : QStyledItemDelegate(parent) {}

void message_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const text = index.data(message_model::text_role).toString();
    auto const sender = index.data(message_model::sender_name_role).toString();
    auto const outgoing = outgoing_at(index);
    auto const day_start = starts_day(index);
    auto const group_start = starts_group(index);
    auto const group_end = ends_group(index);
    auto bounds = text_rect(option, text);

    auto const bubble_width = bounds.width() + kHorizontalPadding * 2;
    auto const bubble_height = bounds.height() + kVerticalPadding * 2;
    auto y = option.rect.top();

    if (day_start)
    {
        auto const label = date_text(index);
        if (!label.isEmpty())
        {
            QFont date_font = option.font;
            date_font.setPointSize(std::max(8, option.font.pointSize() - 2));
            painter->setFont(date_font);
            painter->setPen(QColor(QStringLiteral("#8E9591")));
            QRect date_rect(option.rect.left(), y + kDateHeaderTopGap, option.rect.width(), kDateHeaderHeight);
            painter->drawText(date_rect, Qt::AlignCenter, label);
        }
        y += kDateHeaderTopGap + kDateHeaderHeight + kDateHeaderBottomGap;
    }

    y += group_start ? kGroupGap : kMessageGap;
    auto const group_top = y;

    auto const incoming_content_left = option.rect.left() + kSideMargin + kAvatarSize + kAvatarGap + kTailSpace;
    auto const outgoing_content_right = option.rect.right() - kSideMargin - kAvatarSize - kAvatarGap - kTailSpace;

    if (!outgoing && group_start)
    {
        QFont name_font = option.font;
        name_font.setBold(true);
        name_font.setPointSize(std::max(9, option.font.pointSize() - 1));
        painter->setFont(name_font);
        painter->setPen(QColor(QStringLiteral("#315A4B")));

        auto const name_width = QFontMetrics(name_font).horizontalAdvance(sender);
        QRect name_rect(incoming_content_left, y, name_width, kHeaderHeight);
        painter->drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter, sender);

        auto const timestamp = time_text(index);
        if (!timestamp.isEmpty())
        {
            QFont time_font = option.font;
            time_font.setPointSize(std::max(8, option.font.pointSize() - 2));
            painter->setFont(time_font);
            painter->setPen(QColor(QStringLiteral("#929894")));
            QRect time_rect(name_rect.right() + 8, y, 52, kHeaderHeight);
            painter->drawText(time_rect, Qt::AlignLeft | Qt::AlignVCenter, timestamp);
        }
        y += kHeaderHeight + kHeaderGap;
    }

    auto const bubble_x = outgoing ? outgoing_content_right - bubble_width + 1 : incoming_content_left;
    QRect bubble(bubble_x, y, bubble_width, bubble_height);

    if (group_start)
    {
        auto const avatar_x = outgoing ? option.rect.right() - kSideMargin - kAvatarSize + 1 : option.rect.left() + kSideMargin;
        auto const avatar_y = outgoing ? bubble.top() : group_top;
        QRect avatar_rect(avatar_x, avatar_y, kAvatarSize, kAvatarSize);
        paint_avatar(*painter, avatar_rect, sender, 14);
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(outgoing ? QColor(QStringLiteral("#D6EAD9")) : QColor(QStringLiteral("#FFFFFF")));
    painter->drawPath(bubble_path(bubble, outgoing, group_end));

    painter->setFont(option.font);
    painter->setPen(QColor(QStringLiteral("#26342E")));
    QRect content = bubble.adjusted(kHorizontalPadding, kVerticalPadding, -kHorizontalPadding, -kVerticalPadding);
    painter->drawText(content, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);

    if (outgoing && group_end)
    {
        auto const timestamp = time_text(index);
        if (!timestamp.isEmpty())
        {
            QFont time_font = option.font;
            time_font.setPointSize(std::max(8, option.font.pointSize() - 2));
            painter->setFont(time_font);
            painter->setPen(QColor(QStringLiteral("#8A928E")));
            QRect time_rect(bubble.left(), bubble.bottom() + 1 + kFooterGap, bubble.width(), kFooterHeight);
            painter->drawText(time_rect, Qt::AlignRight | Qt::AlignVCenter, timestamp);
        }
    }

    painter->restore();
}

QSize message_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    auto const text = index.data(message_model::text_role).toString();
    auto const outgoing = outgoing_at(index);
    auto const day_start = starts_day(index);
    auto const group_start = starts_group(index);
    auto const group_end = ends_group(index);
    auto bounds = text_rect(option, text);

    auto height = bounds.height() + kVerticalPadding * 2;
    if (day_start)
    {
        height += kDateHeaderTopGap + kDateHeaderHeight + kDateHeaderBottomGap;
    }
    height += group_start ? kGroupGap : kMessageGap;
    if (!outgoing && group_start)
    {
        height += kHeaderHeight + kHeaderGap;
    }
    if (outgoing && group_end)
    {
        height += kFooterGap + kFooterHeight;
    }
    height += group_end ? kGroupEndGap : kMessageEndGap;
    return {option.rect.width(), height};
}
