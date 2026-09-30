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
    auto const maximum_width = std::max(200, static_cast<int>(available * 0.66));
    auto bounds = QFontMetrics(option.font).boundingRect(QRect(0, 0, maximum_width, 10000), Qt::TextWordWrap, text);
    bounds.setWidth(std::min(maximum_width, std::max(38, bounds.width())));
    return bounds;
}

QPainterPath bubble_path(QRect const& bubble, bool outgoing)
{
    constexpr qreal radius = 15.0;
    constexpr qreal tail_width = 7.0;
    constexpr qreal tail_height = 11.0;

    QPainterPath path;
    path.addRoundedRect(QRectF(bubble), radius, radius);

    QPainterPath tail;
    if (outgoing)
    {
        auto const right = static_cast<qreal>(bubble.right()) + 0.5;
        auto const bottom = static_cast<qreal>(bubble.bottom()) + 0.5;
        tail.moveTo(right - 3.0, bottom - tail_height);
        tail.cubicTo(right - 1.0, bottom - 5.0, right + 1.0, bottom - 2.0, right + tail_width, bottom);
        tail.cubicTo(right + 2.0, bottom + 0.5, right - 1.0, bottom - 0.5, right - 5.0, bottom - 2.0);
        tail.closeSubpath();
    }
    else
    {
        auto const left = static_cast<qreal>(bubble.left()) - 0.5;
        auto const bottom = static_cast<qreal>(bubble.bottom()) + 0.5;
        tail.moveTo(left + 3.0, bottom - tail_height);
        tail.cubicTo(left + 1.0, bottom - 5.0, left - 1.0, bottom - 2.0, left - tail_width, bottom);
        tail.cubicTo(left - 2.0, bottom + 0.5, left + 1.0, bottom - 0.5, left + 5.0, bottom - 2.0);
        tail.closeSubpath();
    }
    return path.united(tail);
}

}    // namespace

message_delegate::message_delegate(QObject* parent) : QStyledItemDelegate(parent) {}

void message_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    auto const text = index.data(message_model::text_role).toString();
    auto const outgoing = index.data(message_model::outgoing_role).toBool();
    auto bounds = text_rect(option, text);

    constexpr int horizontal_padding = 13;
    constexpr int vertical_padding = 8;
    constexpr int side_margin = 20;
    constexpr int tail_width = 7;
    auto const bubble_width = bounds.width() + horizontal_padding * 2;
    auto const bubble_height = bounds.height() + vertical_padding * 2;
    auto const x = outgoing ? option.rect.right() - side_margin - tail_width - bubble_width + 1
                            : option.rect.left() + side_margin + tail_width;
    QRect bubble(x, option.rect.top() + 4, bubble_width, bubble_height);

    painter->setPen(Qt::NoPen);
    painter->setBrush(outgoing ? QColor(QStringLiteral("#DDEBE3")) : QColor(QStringLiteral("#FFFEFA")));
    painter->drawPath(bubble_path(bubble, outgoing));

    painter->setPen(QColor(QStringLiteral("#2B3832")));
    QRect content = bubble.adjusted(horizontal_padding, vertical_padding, -horizontal_padding, -vertical_padding);
    painter->drawText(content, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
    painter->restore();
}

QSize message_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    auto const text = index.data(message_model::text_role).toString();
    auto bounds = text_rect(option, text);
    return {option.rect.width(), bounds.height() + 24};
}
