#include "message_delegate.hpp"

#include <algorithm>

#include <QFontMetrics>
#include <QPainter>

#include "message_model.hpp"

namespace
{

QRect text_rect(QStyleOptionViewItem const& option, QString const& text)
{
    auto const available = std::max(220, option.rect.width());
    auto const maximum_width = std::max(180, static_cast<int>(available * 0.62));
    auto bounds = QFontMetrics(option.font).boundingRect(QRect(0, 0, maximum_width, 10000), Qt::TextWordWrap, text);
    bounds.setWidth(std::min(maximum_width, std::max(42, bounds.width())));
    return bounds;
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

    constexpr int horizontal_padding = 14;
    constexpr int vertical_padding = 10;
    constexpr int side_margin = 18;
    auto const bubble_width = bounds.width() + horizontal_padding * 2;
    auto const bubble_height = bounds.height() + vertical_padding * 2;
    auto const x = outgoing ? option.rect.right() - side_margin - bubble_width + 1 : option.rect.left() + side_margin;
    QRect bubble(x, option.rect.top() + 6, bubble_width, bubble_height);

    painter->setPen(Qt::NoPen);
    painter->setBrush(outgoing ? QColor(QStringLiteral("#DCE9E1")) : QColor(QStringLiteral("#FFFFFF")));
    painter->drawRoundedRect(bubble, 12, 12);

    painter->setPen(QColor(QStringLiteral("#2B3832")));
    QRect content = bubble.adjusted(horizontal_padding, vertical_padding, -horizontal_padding, -vertical_padding);
    painter->drawText(content, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
    painter->restore();
}

QSize message_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    auto const text = index.data(message_model::text_role).toString();
    auto bounds = text_rect(option, text);
    return {option.rect.width(), bounds.height() + 32};
}
