#include "message_delegate.hpp"

#include <algorithm>

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include "message_model.hpp"

namespace
{

QRect text_rect(QStyleOptionViewItem const& option, QString const& text)
{
    auto const available = std::max(240, option.rect.width());
    auto const maximum_width = std::max(200, static_cast<int>(available * 0.64));
    auto bounds = QFontMetrics(option.font).boundingRect(QRect(0, 0, maximum_width, 10000), Qt::TextWordWrap, text);
    bounds.setWidth(std::min(maximum_width, std::max(36, bounds.width())));
    return bounds;
}

bool outgoing_at(QModelIndex const& index)
{
    return index.data(message_model::outgoing_role).toBool();
}

bool starts_group(QModelIndex const& index)
{
    if (index.row() == 0)
    {
        return true;
    }
    auto const previous = index.sibling(index.row() - 1, index.column());
    return !previous.isValid() || outgoing_at(previous) != outgoing_at(index);
}

bool ends_group(QModelIndex const& index)
{
    auto const next = index.sibling(index.row() + 1, index.column());
    return !next.isValid() || outgoing_at(next) != outgoing_at(index);
}

QPainterPath bubble_path(QRect const& bubble, bool outgoing, bool tail)
{
    constexpr qreal radius = 15.0;
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
    auto const outgoing = outgoing_at(index);
    auto const group_start = starts_group(index);
    auto const group_end = ends_group(index);
    auto bounds = text_rect(option, text);

    constexpr int horizontal_padding = 14;
    constexpr int vertical_padding = 9;
    constexpr int side_margin = 22;
    constexpr int tail_space = 6;
    auto const top_spacing = group_start ? 8 : 2;
    auto const bubble_width = bounds.width() + horizontal_padding * 2;
    auto const bubble_height = bounds.height() + vertical_padding * 2;
    auto const x = outgoing ? option.rect.right() - side_margin - tail_space - bubble_width + 1
                            : option.rect.left() + side_margin + tail_space;
    QRect bubble(x, option.rect.top() + top_spacing, bubble_width, bubble_height);

    painter->setPen(Qt::NoPen);
    painter->setBrush(outgoing ? QColor(QStringLiteral("#DCEADF")) : QColor(QStringLiteral("#FFFEFA")));
    painter->drawPath(bubble_path(bubble, outgoing, group_end));

    painter->setPen(QColor(QStringLiteral("#2A3731")));
    QRect content = bubble.adjusted(horizontal_padding, vertical_padding, -horizontal_padding, -vertical_padding);
    painter->drawText(content, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
    painter->restore();
}

QSize message_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    auto const text = index.data(message_model::text_role).toString();
    auto bounds = text_rect(option, text);
    auto const top_spacing = starts_group(index) ? 8 : 2;
    auto const bottom_spacing = ends_group(index) ? 8 : 2;
    return {option.rect.width(), bounds.height() + 18 + top_spacing + bottom_spacing};
}
