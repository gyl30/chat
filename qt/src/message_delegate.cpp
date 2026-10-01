#include "message_delegate.hpp"

#include <algorithm>

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "avatar.hpp"
#include "message_model.hpp"
#include "theme.hpp"

namespace
{

constexpr qint64 kGroupInterval = 5 * 60 * 1000;

bool outgoing_at(QModelIndex const& index)
{
    return index.data(message_model::outgoing_role).toBool();
}

bool read_at(QModelIndex const& index)
{
    return index.data(message_model::read_role).toBool();
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
    if (!lhs.isValid() || !rhs.isValid() || outgoing_at(lhs) != outgoing_at(rhs) ||
        lhs.data(message_model::from_role) != rhs.data(message_model::from_role))
    {
        return false;
    }

    auto const lhs_timestamp = timestamp_at(lhs);
    auto const rhs_timestamp = timestamp_at(rhs);
    return lhs_timestamp > 0 && rhs_timestamp >= lhs_timestamp && local_date(lhs) == local_date(rhs)
        && rhs_timestamp - lhs_timestamp <= kGroupInterval;
}

bool starts_group(QModelIndex const& index)
{
    return index.row() == 0 || !same_group(index.sibling(index.row() - 1, index.column()), index);
}

bool ends_group(QModelIndex const& index)
{
    return !same_group(index, index.sibling(index.row() + 1, index.column()));
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

QString time_text(QModelIndex const& index)
{
    auto const timestamp = timestamp_at(index);
    return timestamp > 0
        ? QDateTime::fromMSecsSinceEpoch(timestamp).toLocalTime().toString(QStringLiteral("HH:mm"))
        : QString{};
}

QFont name_font(QStyleOptionViewItem const& option)
{
    auto font = option.font;
    font.setBold(true);
    font.setPixelSize(13);
    return font;
}

QFont time_font(QStyleOptionViewItem const& option)
{
    auto font = option.font;
    font.setPixelSize(12);
    return font;
}

QFont date_font(QStyleOptionViewItem const& option)
{
    auto font = option.font;
    font.setBold(true);
    font.setPixelSize(13);
    return font;
}

int maximum_bubble_width(QStyleOptionViewItem const& option, bool outgoing)
{
    auto const width = std::max(320, option.rect.width());
    auto const reserved = chat_theme::message_side_margin * 2
        + (outgoing ? 0 : chat_theme::message_avatar_skip);
    return std::max(180, std::min(chat_theme::message_max_width, width - reserved));
}

struct message_layout
{
    QString text;
    QString sender;
    QString time;
    bool outgoing = false;
    bool read = false;
    bool day_start = false;
    bool group_start = false;
    bool group_end = false;
    bool time_on_text_line = false;
    int name_height = 0;
    int text_width = 0;
    int text_height = 0;
    int time_width = 0;
    int time_height = 0;
    int receipt_width = 0;
    int bubble_width = 0;
    int bubble_height = 0;
    int day_height = 0;
    int top_margin = 0;
};

message_layout calculate_layout(QStyleOptionViewItem const& option, QModelIndex const& index)
{
    message_layout result;
    result.text = index.data(message_model::text_role).toString();
    auto const reply = index.data(message_model::reply_text_role).toString();
    if (!reply.isEmpty())
    {
        result.text = reply + QStringLiteral("\n\n") + result.text;
    }
    result.sender = index.data(message_model::sender_name_role).toString();
    result.time = time_text(index);
    if (!index.data(message_model::deleted_role).toBool() && index.data(message_model::edited_at_role).toLongLong() > 0)
    {
        result.time = QStringLiteral("已编辑 · ") + result.time;
    }
    result.outgoing = outgoing_at(index);
    result.read = result.outgoing && read_at(index);
    result.day_start = starts_day(index);
    result.group_start = starts_group(index);
    result.group_end = ends_group(index);
    result.top_margin = result.group_start
        ? chat_theme::message_margin_top
        : chat_theme::message_margin_top_attached;

    auto const bubble_max = maximum_bubble_width(option, result.outgoing);
    auto const inner_max = std::max(80, bubble_max - chat_theme::message_padding_horizontal * 2);

    QFontMetrics body_metrics(option.font);
    auto const one_line_width = body_metrics.horizontalAdvance(result.text);
    auto const body_line_height = body_metrics.height();
    auto const time_metrics = QFontMetrics(time_font(option));
    result.time_width = result.time.isEmpty() ? 0 : time_metrics.horizontalAdvance(result.time);
    result.time_height = result.time.isEmpty() ? 0 : time_metrics.height();
    result.receipt_width = result.outgoing ? 17 : 0;
    auto const metadata_width = result.time_width
        + ((result.time_width > 0 && result.receipt_width > 0) ? 3 : 0)
        + result.receipt_width;

    auto const single_line = !result.text.contains(QLatin1Char('\n')) && one_line_width <= inner_max;
    if (single_line)
    {
        result.text_width = one_line_width;
        result.text_height = body_line_height;
        result.time_on_text_line = metadata_width == 0
            || result.text_width + chat_theme::message_time_gap + metadata_width <= inner_max;
    }
    else
    {
        auto const bounds = body_metrics.boundingRect(
            QRect(0, 0, inner_max, 10000), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, result.text);
        result.text_width = std::min(inner_max, std::max(1, bounds.width()));
        result.text_height = std::max(body_line_height, bounds.height());
    }

    auto content_width = result.text_width;
    auto content_height = result.text_height;
    if (metadata_width > 0)
    {
        if (result.time_on_text_line)
        {
            content_width += chat_theme::message_time_gap + metadata_width;
            content_height = std::max(content_height, result.time_height);
        }
        else
        {
            content_width = std::max(content_width, metadata_width);
            content_height += 2 + result.time_height;
        }
    }

    if (!result.outgoing && result.group_start && !result.sender.isEmpty())
    {
        auto const metrics = QFontMetrics(name_font(option));
        result.name_height = metrics.height() + chat_theme::message_name_gap;
        content_width = std::max(content_width, metrics.horizontalAdvance(result.sender));
    }

    result.bubble_width = std::min(
        bubble_max,
        content_width + chat_theme::message_padding_horizontal * 2);
    result.bubble_height = result.name_height + content_height
        + chat_theme::message_padding_vertical * 2;
    if (result.day_start && !date_text(index).isEmpty())
    {
        result.day_height = chat_theme::message_date_margin_top
            + chat_theme::message_date_height
            + chat_theme::message_date_margin_bottom;
    }
    return result;
}

QPainterPath rounded_path(QRectF const& rect, qreal top_left, qreal top_right, qreal bottom_right, qreal bottom_left)
{
    QPainterPath path;
    path.moveTo(rect.left() + top_left, rect.top());
    path.lineTo(rect.right() - top_right, rect.top());
    path.quadTo(rect.right(), rect.top(), rect.right(), rect.top() + top_right);
    path.lineTo(rect.right(), rect.bottom() - bottom_right);
    path.quadTo(rect.right(), rect.bottom(), rect.right() - bottom_right, rect.bottom());
    path.lineTo(rect.left() + bottom_left, rect.bottom());
    path.quadTo(rect.left(), rect.bottom(), rect.left(), rect.bottom() - bottom_left);
    path.lineTo(rect.left(), rect.top() + top_left);
    path.quadTo(rect.left(), rect.top(), rect.left() + top_left, rect.top());
    path.closeSubpath();
    return path;
}

void paint_receipt(QPainter& painter, QRect const& rect, bool read)
{
    auto pen = QPen(read ? QColor(QStringLiteral("#4C876C")) : QColor(QStringLiteral("#6E8877")));
    pen.setWidthF(1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    auto draw_check = [&](int x) {
        QPainterPath path;
        path.moveTo(x, rect.center().y());
        path.lineTo(x + 3, rect.center().y() + 3);
        path.lineTo(x + 8, rect.center().y() - 3);
        painter.drawPath(path);
    };

    if (read)
    {
        draw_check(rect.left());
        draw_check(rect.left() + 5);
    }
    else
    {
        draw_check(rect.left() + 5);
    }
}

QPainterPath bubble_path(QRect const& bubble, bool outgoing, bool group_start, bool group_end)
{
    auto const large = static_cast<qreal>(chat_theme::message_radius);
    auto const small = static_cast<qreal>(chat_theme::message_attached_radius);
    auto top_left = large;
    auto top_right = large;
    auto bottom_right = large;
    auto bottom_left = large;

    if (outgoing)
    {
        top_right = group_start ? large : small;
        bottom_right = group_end ? large : small;
    }
    else
    {
        top_left = group_start ? large : small;
        bottom_left = group_end ? large : small;
    }

    auto path = rounded_path(QRectF(bubble), top_left, top_right, bottom_right, bottom_left);
    if (!group_end)
    {
        return path;
    }

    QPainterPath tail;
    auto const bottom = static_cast<qreal>(bubble.bottom()) + 0.5;
    if (outgoing)
    {
        auto const right = static_cast<qreal>(bubble.right()) + 0.5;
        tail.moveTo(right - 4.0, bottom - chat_theme::message_tail_height);
        tail.cubicTo(right - 1.5, bottom - 4.5, right + 0.5, bottom - 1.5,
                     right + chat_theme::message_tail_width, bottom);
        tail.cubicTo(right + 1.5, bottom + 0.3, right - 1.0, bottom - 0.3, right - 4.0, bottom - 2.0);
    }
    else
    {
        auto const left = static_cast<qreal>(bubble.left()) - 0.5;
        tail.moveTo(left + 4.0, bottom - chat_theme::message_tail_height);
        tail.cubicTo(left + 1.5, bottom - 4.5, left - 0.5, bottom - 1.5,
                     left - chat_theme::message_tail_width, bottom);
        tail.cubicTo(left - 1.5, bottom + 0.3, left + 1.0, bottom - 0.3, left + 4.0, bottom - 2.0);
    }
    tail.closeSubpath();
    return path.united(tail);
}

}    // namespace

message_delegate::message_delegate(QObject* parent) : QStyledItemDelegate(parent) {}

void message_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const layout = calculate_layout(option, index);
    auto y = option.rect.top();

    if (layout.day_height > 0)
    {
        auto const label = date_text(index);
        auto const font = date_font(option);
        auto const metrics = QFontMetrics(font);
        auto const width = metrics.horizontalAdvance(label) + 20;
        QRect pill(option.rect.center().x() - width / 2,
                   y + chat_theme::message_date_margin_top,
                   width,
                   chat_theme::message_date_height);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(88, 106, 97, 30));
        painter->drawRoundedRect(pill, chat_theme::message_date_height / 2.0,
                                 chat_theme::message_date_height / 2.0);
        painter->setFont(font);
        painter->setPen(QColor(QStringLiteral("#6E7A74")));
        painter->drawText(pill, Qt::AlignCenter, label);
        y += layout.day_height;
    }

    y += layout.top_margin;
    auto const incoming_left = option.rect.left() + chat_theme::message_side_margin + chat_theme::message_avatar_skip;
    auto const outgoing_right = option.rect.right() - chat_theme::message_side_margin;
    auto const bubble_x = layout.outgoing
        ? outgoing_right - layout.bubble_width + 1
        : incoming_left;
    QRect bubble(bubble_x, y, layout.bubble_width, layout.bubble_height);

    if (!layout.outgoing && layout.group_end)
    {
        auto const avatar_x = option.rect.left() + chat_theme::message_side_margin;
        auto const avatar_y = bubble.bottom() - chat_theme::message_avatar_size + 1;
        paint_avatar(*painter,
                     QRect(avatar_x, avatar_y, chat_theme::message_avatar_size, chat_theme::message_avatar_size),
                     layout.sender,
                     13);
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(layout.outgoing
                          ? QColor(QStringLiteral("#D6EAD9"))
                          : QColor(QStringLiteral("#FFFFFF")));
    painter->drawPath(bubble_path(bubble, layout.outgoing, layout.group_start, layout.group_end));

    auto content_top = bubble.top() + chat_theme::message_padding_vertical;
    auto const content_left = bubble.left() + chat_theme::message_padding_horizontal;
    auto const content_right = bubble.right() - chat_theme::message_padding_horizontal + 1;

    if (!layout.outgoing && layout.group_start && !layout.sender.isEmpty())
    {
        auto const font = name_font(option);
        painter->setFont(font);
        painter->setPen(QColor(QStringLiteral("#315A4B")));
        QRect name_rect(content_left, content_top, content_right - content_left, QFontMetrics(font).height());
        painter->drawText(name_rect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(layout.sender, Qt::ElideRight, name_rect.width()));
        content_top += layout.name_height;
    }

    painter->setFont(option.font);
    painter->setPen(QColor(QStringLiteral("#26342E")));
    auto const metadata_width = layout.time_width
        + ((layout.time_width > 0 && layout.receipt_width > 0) ? 3 : 0)
        + layout.receipt_width;
    auto const time_reserved = layout.time_on_text_line && metadata_width > 0
        ? chat_theme::message_time_gap + metadata_width
        : 0;
    QRect text_rect(content_left, content_top,
                    std::max(1, content_right - content_left - time_reserved),
                    layout.text_height);
    painter->drawText(text_rect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, layout.text);

    if (metadata_width > 0)
    {
        auto const metadata_y = layout.time_on_text_line
            ? content_top + std::max(0, (layout.text_height - layout.time_height) / 2)
            : content_top + layout.text_height + 2;
        auto metadata_x = content_right - metadata_width;
        if (layout.time_width > 0)
        {
            auto const font = time_font(option);
            painter->setFont(font);
            painter->setPen(layout.outgoing
                                ? QColor(QStringLiteral("#6E8877"))
                                : QColor(QStringLiteral("#89918D")));
            QRect time_rect(metadata_x, metadata_y, layout.time_width, layout.time_height);
            painter->drawText(time_rect, Qt::AlignRight | Qt::AlignVCenter, layout.time);
            metadata_x += layout.time_width;
            if (layout.receipt_width > 0)
            {
                metadata_x += 3;
            }
        }
        if (layout.receipt_width > 0)
        {
            QRect receipt_rect(metadata_x, metadata_y, layout.receipt_width, layout.time_height);
            paint_receipt(*painter, receipt_rect, layout.read);
        }
    }

    painter->restore();
}

QSize message_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    auto const layout = calculate_layout(option, index);
    return {
        option.rect.width(),
        layout.day_height + layout.top_margin + layout.bubble_height + chat_theme::message_margin_bottom,
    };
}

bool message_delegate::editorEvent(QEvent* event, QAbstractItemModel* model,
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

    auto const layout = calculate_layout(option, index);
    if (layout.outgoing || !layout.group_end)
    {
        return false;
    }

    auto y = option.rect.top() + layout.day_height + layout.top_margin;
    auto const bubble_bottom = y + layout.bubble_height - 1;
    QRect avatar_rect(option.rect.left() + chat_theme::message_side_margin,
                      bubble_bottom - chat_theme::message_avatar_size + 1,
                      chat_theme::message_avatar_size, chat_theme::message_avatar_size);
    if (!avatar_rect.contains(mouse->position().toPoint()))
    {
        return false;
    }

    emit avatar_clicked(index);
    return true;
}
