#include "message_delegate.hpp"
#include "message_card.hpp"
#include "theme_manager.hpp"
#include "message_links.hpp"
#include "message_markdown.hpp"
#include <QAbstractTextDocumentLayout>
#include <QHelpEvent>
#include <QToolTip>
#include <QDesktopServices>
#include <QApplication>
#include <QClipboard>

#include <algorithm>
#include <memory>

#include <QAbstractItemView>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextLayout>
#include <QtMath>

#include "avatar.hpp"
#include "emoji_text.hpp"
#include "message_model.hpp"
#include "theme.hpp"

namespace
{

constexpr qint64 kGroupInterval = 5 * 60 * 1000;

bool outgoing_at(QModelIndex const& index) { return index.data(message_model::outgoing_role).toBool(); }

qint64 timestamp_at(QModelIndex const& index) { return index.data(message_model::timestamp_role).toLongLong(); }

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
    if (!lhs.isValid() || !rhs.isValid() || rhs.data(message_model::unread_boundary_role).toBool() || outgoing_at(lhs) != outgoing_at(rhs) ||
        lhs.data(message_model::from_role) != rhs.data(message_model::from_role))
    {
        return false;
    }

    auto const lhs_timestamp = timestamp_at(lhs);
    auto const rhs_timestamp = timestamp_at(rhs);
    return lhs_timestamp > 0 && rhs_timestamp >= lhs_timestamp && local_date(lhs) == local_date(rhs) &&
           rhs_timestamp - lhs_timestamp <= kGroupInterval;
}

bool starts_group(QModelIndex const& index) { return index.row() == 0 || !same_group(index.sibling(index.row() - 1, index.column()), index); }

bool ends_group(QModelIndex const& index) { return !same_group(index, index.sibling(index.row() + 1, index.column())); }

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
    return timestamp > 0 ? QDateTime::fromMSecsSinceEpoch(timestamp).toLocalTime().toString(QStringLiteral("HH:mm")) : QString{};
}

QFont time_font(QStyleOptionViewItem const& option)
{
    auto font = option.font;
    font.setPixelSize(13);
    return font;
}

QFont date_font(QStyleOptionViewItem const& option)
{
    auto font = option.font;
    font.setBold(true);
    font.setPixelSize(13);
    return font;
}

int maximum_bubble_width(QStyleOptionViewItem const& option)
{
    auto const width = std::max(320, option.rect.width());
    auto const reserved = chat_theme::message_side_margin * 2 + chat_theme::message_avatar_skip;
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
    int text_width = 0;
    int text_height = 0;
    int time_width = 0;
    int time_height = 0;
    int receipt_width = 0;
    int bubble_width = 0;
    int bubble_height = 0;
    int day_height = 0;
    int unread_height = 0;
    int top_margin = 0;
    QList<reaction_data> reactions;
    QList<QRect> reaction_rects;
    int reactions_height = 0;
    QPixmap image;
    QString image_status;
    int image_width = 0;
    int image_height = 0;
    std::unique_ptr<QTextLayout> text_layout;
    std::unique_ptr<message_markdown> markdown;
    int prefix_height = 0;
    bool bundle = false;
    std::optional<chat::poll_summary> poll;

    QRect bubble_rect(QRect const& row) const
    {
        int const margin = chat_theme::message_side_margin + chat_theme::message_avatar_skip;
        int const x = outgoing ? row.right() - margin - bubble_width + 1 : row.left() + margin;
        return {x, row.top() + day_height + unread_height + top_margin, bubble_width, bubble_height};
    }

    QRect avatar_rect(QRect const& row) const
    {
        if (!group_end)
        {
            return {};
        }
        constexpr int size = chat_theme::message_avatar_size;
        auto const x = outgoing ? row.right() - chat_theme::message_side_margin - size + 1 : row.left() + chat_theme::message_side_margin;
        return {x, bubble_rect(row).bottom() - size + 1, size, size};
    }

    QPoint text_origin(QRect const& row) const
    {
        return bubble_rect(row).topLeft() + QPoint(chat_theme::message_padding_horizontal, chat_theme::message_padding_vertical + image_height);
    }

    QUrl link_at(QRect const& row, QPoint point, QModelIndex const& index) const
    {
        if (index.data(message_model::send_state_role).toInt() != message_model::sent)
        {
            return {};
        }
        auto local = point - text_origin(row);
        if (markdown)
        {
            return http_message_url(markdown->documentLayout()->anchorAt(local - QPoint(0, prefix_height)));
        }
        for (int i = 0; i < text_layout->lineCount(); ++i)
        {
            auto line = text_layout->lineAt(i);
            if (!line.naturalTextRect().contains(local))
            {
                continue;
            }
            auto character = line.xToCursor(local.x(), QTextLine::CursorOnCharacter);
            for (auto const& range : text_layout->formats())
            {
                if (range.format.isAnchor() && character >= range.start && character < range.start + range.length)
                {
                    return QUrl(range.format.anchorHref());
                }
            }
            break;
        }
        return {};
    }
};

message_layout calculate_layout(QStyleOptionViewItem const& option, QModelIndex const& index)
{
    message_layout result;
    result.text = index.data(message_model::text_role).toString();
    if (auto poll = index.data(message_model::poll_role); poll.isValid())
    {
        result.poll = poll.value<chat::poll_summary>();
    }
    result.outgoing = outgoing_at(index);
    auto const bubble_max = maximum_bubble_width(option);
    auto const inner_max = std::max(80, bubble_max - chat_theme::message_padding_horizontal * 2);

    if (!index.data(message_model::attachment_name_role).toString().isEmpty())
    {
        // Keep the size on the next line and the extension visible in a narrow bubble.
        auto const end = result.text.lastIndexOf(u'\n');
        result.text.replace(0, end, QFontMetrics(option.font).elidedText(result.text.left(end), Qt::ElideMiddle, inner_max));
    }
    auto download_state = index.data(message_model::download_status_role).toString();
    if (!download_state.isEmpty())
    {
        result.text += u'\n' + download_state;
    }
    auto const reply = index.data(message_model::reply_text_role).toString();
    if (!reply.isEmpty())
    {
        result.text = reply + QStringLiteral("\n\n") + result.text;
    }
    auto const origin = index.data(message_model::forwarded_from_role).toString();
    if (!origin.isEmpty())
    {
        result.text = QStringLiteral("转发自 %1\n").arg(origin) + result.text;
    }
    result.sender = index.data(message_model::sender_name_role).toString();
    result.time = time_text(index);
    if (!index.data(message_model::deleted_role).toBool() && index.data(message_model::edited_at_role).toLongLong() > 0)
    {
        result.time = QStringLiteral("已编辑 · ") + result.time;
    }
    auto const read_count = index.data(message_model::read_count_role);
    if (read_count.isValid())
    {
        result.time = QStringLiteral("已读 %1 人 · ").arg(read_count.toInt()) + result.time;
    }
    if (index.data(message_model::mention_all_role).toBool())
    {
        result.time = QStringLiteral("@所有人 · ") + result.time;
    }
    else if (!result.outgoing && index.data(message_model::mentioned_role).toBool())
    {
        result.time = QStringLiteral("提及你 · ") + result.time;
    }
    auto const receipt = index.data(message_model::read_role);
    result.read = receipt.toBool();
    switch (index.data(message_model::send_state_role).toInt())
    {
        case message_model::waiting:
            result.time = QStringLiteral("等待发送");
            break;
        case message_model::sending:
            result.time = index.data(message_model::send_error_role).toString();
            if (result.time.isEmpty())
            {
                result.time = QStringLiteral("发送中…");
            }
            break;
        case message_model::failed:
            result.time = index.data(message_model::send_error_role).toString();
            if (index.data(message_model::attachment_name_role).toString().isEmpty())
            {
                result.time = QStringLiteral("发送失败：%1").arg(result.time);
            }
            break;
        default:
            break;
    }
    result.day_start = starts_day(index);
    result.group_start = starts_group(index);
    result.group_end = ends_group(index);
    result.top_margin = result.group_start ? chat_theme::message_margin_top : chat_theme::message_margin_top_attached;

    auto const time_metrics = QFontMetrics(time_font(option));
    result.time_width = result.time.isEmpty() ? 0 : time_metrics.horizontalAdvance(result.time);
    result.time_height = result.time.isEmpty() ? 0 : time_metrics.height();
    result.receipt_width = receipt.isValid() ? 17 : 0;
    auto const metadata_width = result.time_width + ((result.time_width > 0 && result.receipt_width > 0) ? 3 : 0) + result.receipt_width;

    auto const mentions = index.data(message_model::mentions_role).value<QList<mention_data>>();
    auto text = result.text;
    result.bundle = index.data(message_model::content_role).toString() == QStringLiteral("bundle");
    if (result.bundle || result.poll)
    {
        text.clear();
    }
    else if (index.data(message_model::markdown_role).toBool())
    {
        auto body = index.data(message_model::text_role).toString();
        result.markdown = std::make_unique<message_markdown>(body, option.font, inner_max, mentions);
        text.chop(body.size());
    }
    text.replace(QLatin1Char('\n'), QChar::LineSeparator);
    result.text_layout = std::make_unique<QTextLayout>(text, option.font);
    QTextOption text_option;
    text_option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    result.text_layout->setTextOption(text_option);
    auto formats = emoji_formats(text, option.font);
    if (index.data(message_model::content_role).toString() == QStringLiteral("message") && !result.markdown &&
        !index.data(message_model::deleted_role).toBool() && index.data(message_model::send_state_role).toInt() == message_model::sent &&
        index.data(message_model::attachment_name_role).toString().isEmpty())
    {
        auto body = index.data(message_model::text_role).toString();
        for (auto range : message_links(body))
        {
            range.start += static_cast<int>(text.size() - body.size());
            formats.push_back(range);
        }
    }
    if (!result.markdown && !mentions.isEmpty())
    {
        auto const body_start = result.text.size() - index.data(message_model::text_role).toString().size();
        for (auto const& mention : mentions)
        {
            QRegularExpression pattern(
                QStringLiteral("(?<![\\p{L}\\p{N}_@])@%1(?![\\p{L}\\p{N}_@])").arg(QRegularExpression::escape(mention.username)));
            auto matches = pattern.globalMatch(result.text, body_start);
            while (matches.hasNext())
            {
                auto const match = matches.next();
                QTextCharFormat format;
                format.setForeground(themed("link"));
                formats.push_back({static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), format});
            }
        }
    }
    result.text_layout->setFormats(formats);
    result.text_layout->beginLayout();
    result.text_width = 1;
    while (true)
    {
        auto line = result.text_layout->createLine();
        if (!line.isValid())
        {
            break;
        }
        line.setLineWidth(inner_max);
        line.setPosition(QPointF(0, result.text_height));
        result.text_height += qCeil(line.height());
        result.text_width = std::max(result.text_width, qCeil(line.naturalTextWidth()));
    }
    result.text_layout->endLayout();
    if (result.markdown)
    {
        result.prefix_height = text.isEmpty() ? 0 : result.text_height;
        result.text_width = std::max(result.text_width, qCeil(result.markdown->size().width()));
        result.text_height = result.prefix_height + qCeil(result.markdown->size().height());
    }
    if (result.bundle || result.poll)
    {
        result.text_width = std::min(result.poll ? 350 : 320, inner_max);
        result.text_height = result.poll ? layout_poll_card(nullptr, option.font, result.text_width, result.text, *result.poll)
                                         : layout_bundle_card(nullptr, option.font, result.text_width, result.text);
    }
    auto const single_line =
        !result.bundle && !result.poll && !result.markdown && !result.text.contains(QLatin1Char('\n')) && result.text_layout->lineCount() <= 1;
    result.time_on_text_line = single_line && (metadata_width == 0 || result.text_width + chat_theme::message_time_gap + metadata_width <= inner_max);

    auto content_width = result.text_width;
    auto content_height = result.text_height;
    auto const image_status = index.data(message_model::image_status_role);
    if (image_status.isValid() && index.data(message_model::attachment_type_role).toString().startsWith(QStringLiteral("image/")))
    {
        result.image = index.data(message_model::image_role).value<QPixmap>();
        result.image_status = image_status.toString();
        result.image_width = std::min(320, inner_max);
        result.image_height = 248;
        content_width = std::max(content_width, result.image_width);
        content_height += result.image_height;
        result.time_on_text_line = false;
    }
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

    result.reactions = index.data(message_model::reactions_role).value<QList<reaction_data>>();
    int chip_x = 0;
    int chip_y = 6;
    auto const chip_height = time_metrics.height() + 8;
    for (auto const& reaction : result.reactions)
    {
        auto const width = time_metrics.horizontalAdvance(reaction.emoji + QStringLiteral(" %1").arg(reaction.users.size())) + 16;
        if (chip_x > 0 && chip_x + width > inner_max)
        {
            chip_x = 0;
            chip_y += chip_height + 4;
        }
        result.reaction_rects.push_back(QRect(chip_x, chip_y, width, chip_height));
        content_width = std::max(content_width, chip_x + width);
        chip_x += width + 4;
        result.reactions_height = chip_y + chip_height;
    }
    content_height += result.reactions_height;

    result.bubble_width = std::min(bubble_max, content_width + chat_theme::message_padding_horizontal * 2);
    result.bubble_height = content_height + chat_theme::message_padding_vertical * 2;
    result.unread_height = index.data(message_model::unread_boundary_role).toBool() ? QFontMetrics(date_font(option)).height() + 12 : 0;
    if (result.day_start && !date_text(index).isEmpty())
    {
        result.day_height = chat_theme::message_date_margin_top + chat_theme::message_date_height + chat_theme::message_date_margin_bottom;
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
    auto pen = QPen(read ? themed("read") : themed("message_muted"));
    pen.setWidthF(1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    auto draw_check = [&](int x)
    {
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
        tail.cubicTo(right - 1.5, bottom - 4.5, right + 0.5, bottom - 1.5, right + chat_theme::message_tail_width, bottom);
        tail.cubicTo(right + 1.5, bottom + 0.3, right - 1.0, bottom - 0.3, right - 4.0, bottom - 2.0);
    }
    else
    {
        auto const left = static_cast<qreal>(bubble.left()) - 0.5;
        tail.moveTo(left + 4.0, bottom - chat_theme::message_tail_height);
        tail.cubicTo(left + 1.5, bottom - 4.5, left - 0.5, bottom - 1.5, left - chat_theme::message_tail_width, bottom);
        tail.cubicTo(left - 1.5, bottom + 0.3, left + 1.0, bottom - 0.3, left + 4.0, bottom - 2.0);
    }
    tail.closeSubpath();
    return path.united(tail);
}

}    // namespace

message_delegate::message_delegate(QObject* parent, qint64 const* context_anchor) : QStyledItemDelegate(parent), context_anchor_(context_anchor) {}

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
        QRect pill(option.rect.center().x() - width / 2, y + chat_theme::message_date_margin_top, width, chat_theme::message_date_height);
        painter->setPen(Qt::NoPen);
        painter->setBrush(themed("day_pill"));
        painter->drawRoundedRect(pill, chat_theme::message_date_height / 2.0, chat_theme::message_date_height / 2.0);
        painter->setFont(font);
        painter->setPen(themed("text_secondary"));
        painter->drawText(pill, Qt::AlignCenter, label);
    }

    if (layout.unread_height)
    {
        QRect band(option.rect.left() + 16, y + layout.day_height, option.rect.width() - 32, layout.unread_height);
        auto const label = QStringLiteral("未读消息");
        painter->setFont(date_font(option));
        int const gap = painter->fontMetrics().horizontalAdvance(label) / 2 + 10;
        auto const center = band.center();
        painter->setPen(themed("input_border"));
        painter->drawLine(band.left(), center.y(), center.x() - gap, center.y());
        painter->drawLine(center.x() + gap, center.y(), band.right(), center.y());
        painter->setPen(themed("text_secondary"));
        painter->drawText(band, Qt::AlignCenter, label);
    }
    auto const bubble = layout.bubble_rect(option.rect);
    if (auto rect = layout.avatar_rect(option.rect); !rect.isEmpty())
    {
        paint_avatar(*painter, rect, layout.sender, 13, index.data(Qt::DecorationRole).value<QPixmap>());
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(layout.outgoing ? themed("bubble_out") : themed("bubble_in"));
    auto const path = bubble_path(bubble, layout.outgoing, layout.group_start, layout.group_end);
    if (!context_anchor_ && (layout.bundle || layout.poll) && (option.state & (QStyle::State_MouseOver | QStyle::State_HasFocus)))
    {
        painter->setPen(themed("focus"));
    }
    painter->drawPath(path);

    auto content_top = bubble.top() + chat_theme::message_padding_vertical;
    auto const content_left = bubble.left() + chat_theme::message_padding_horizontal;
    auto const content_right = bubble.right() - chat_theme::message_padding_horizontal + 1;

    painter->setFont(option.font);
    if (layout.image_height > 0)
    {
        QRect frame(content_left, content_top, layout.image_width, layout.image_height - 8);
        painter->setPen(Qt::NoPen);
        painter->setBrush(themed("subtle"));
        painter->drawRoundedRect(frame, 8, 8);
        if (!layout.image.isNull())
        {
            auto const size = layout.image.size().scaled(frame.size(), Qt::KeepAspectRatio);
            QRect target(QPoint(0, 0), size);
            target.moveCenter(frame.center());
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawPixmap(target, layout.image);
        }
        else
        {
            painter->setPen(themed("text_secondary"));
            painter->drawText(frame.adjusted(12, 8, -12, -8), Qt::AlignCenter | Qt::TextWordWrap, layout.image_status);
        }
        content_top += layout.image_height;
    }
    painter->setPen(themed("text"));
    auto const metadata_width = layout.time_width + ((layout.time_width > 0 && layout.receipt_width > 0) ? 3 : 0) + layout.receipt_width;
    auto origin = layout.text_origin(option.rect);
    layout.text_layout->draw(painter, origin);
    if (layout.bundle || layout.poll)
    {
        painter->save();
        painter->translate(origin);
        if (layout.poll)
        {
            layout_poll_card(painter, option.font, layout.text_width, layout.text, *layout.poll);
        }
        else
        {
            layout_bundle_card(painter, option.font, layout.text_width, layout.text);
        }
        painter->restore();
    }
    else if (layout.markdown)
    {
        painter->save();
        painter->translate(origin + QPoint(0, layout.prefix_height));
        QAbstractTextDocumentLayout::PaintContext context;
        context.palette.setColor(QPalette::Text, themed("text"));
        context.palette.setColor(QPalette::Link, themed("link"));
        layout.markdown->documentLayout()->draw(painter, context);
        layout.markdown->draw_quote_guides(*painter);
        painter->restore();
    }

    if (metadata_width > 0)
    {
        auto const metadata_y = layout.time_on_text_line ? content_top + std::max(0, (layout.text_height - layout.time_height) / 2)
                                                         : content_top + layout.text_height + 2;
        auto metadata_x = content_right - metadata_width;
        if (layout.time_width > 0)
        {
            auto const font = time_font(option);
            painter->setFont(font);
            painter->setPen(themed("message_muted"));
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

    auto const own_reaction = index.data(message_model::own_reaction_role).toString();
    painter->setFont(time_font(option));
    for (int i = 0; i < layout.reactions.size(); ++i)
    {
        auto const& reaction = layout.reactions[i];
        auto const rect =
            layout.reaction_rects[i].translated(content_left, bubble.bottom() + 1 - chat_theme::message_padding_vertical - layout.reactions_height);
        auto const mine = reaction.emoji == own_reaction;
        painter->setPen(Qt::NoPen);
        painter->setBrush(themed(mine ? "reaction_own" : "selected"));
        painter->drawRoundedRect(rect, 10, 10);
        painter->setPen(themed("reaction_text"));
        painter->drawText(rect, Qt::AlignCenter, reaction.emoji + QStringLiteral(" %1").arg(reaction.users.size()));
    }

    auto const* view = qobject_cast<QAbstractItemView const*>(option.widget);
    bool const focused =
        context_anchor_ ? index.data(message_model::id_role).toLongLong() == *context_anchor_ : option.state.testFlag(QStyle::State_HasFocus);
    if (focused ||
        (!context_anchor_ && (!view || view->selectionMode() != QAbstractItemView::NoSelection) && (option.state & QStyle::State_Selected)))
    {
        painter->setPen(QPen(themed("message_focus"), focused ? 1.5 : 1.0));
        painter->setBrush(Qt::NoBrush);
        // Attached bubbles meet the row's top edge; keep the centered stroke in its own item.
        painter->setClipRect(option.rect, Qt::IntersectClip);
        painter->drawPath(path);
    }

    painter->restore();
}

QSize message_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    auto const layout = calculate_layout(option, index);
    return {
        option.rect.width(),
        layout.day_height + layout.unread_height + layout.top_margin + layout.bubble_height + chat_theme::message_margin_bottom,
    };
}

bool message_delegate::on_unread_divider(QStyleOptionViewItem const& option, QModelIndex const& index, QPoint position)
{
    auto const layout = calculate_layout(option, index);
    auto const y = position.y() - option.rect.top() - layout.day_height;
    return y >= 0 && y < layout.unread_height;
}

QUrl message_delegate::link_at(QStyleOptionViewItem const& option, QModelIndex const& index, QPoint position)
{
    return calculate_layout(option, index).link_at(option.rect, position, index);
}

QString message_delegate::code_at(QStyleOptionViewItem const& option, QModelIndex const& index, QPoint point)
{
    if (!index.data(message_model::markdown_role).toBool())
    {
        return {};
    }
    auto layout = calculate_layout(option, index);
    return layout.markdown->code_at(point - layout.text_origin(option.rect) - QPoint(0, layout.prefix_height));
}

bool message_delegate::helpEvent(QHelpEvent* event, QAbstractItemView* view, QStyleOptionViewItem const& option, QModelIndex const& index)
{
    if (event->type() == QEvent::ToolTip && view->selectionMode() != QAbstractItemView::ExtendedSelection)
    {
        auto const layout = calculate_layout(option, index);
        if (layout.avatar_rect(option.rect).contains(event->pos()))
        {
            QToolTip::showText(event->globalPos(), layout.sender.toHtmlEscaped(), view);
            return true;
        }
        auto url = layout.link_at(option.rect, event->pos(), index);
        if (!url.isEmpty())
        {
            QToolTip::showText(event->globalPos(), url.toString(QUrl::FullyEncoded).toHtmlEscaped(), view);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

bool message_delegate::editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index)
{
    (void)model;
    if (event->type() != QEvent::MouseButtonRelease)
    {
        return false;
    }

    auto const* mouse = static_cast<QMouseEvent*>(event);
    auto const* view = qobject_cast<QAbstractItemView const*>(option.widget);
    if (view && view->selectionMode() == QAbstractItemView::ExtendedSelection)
    {
        return false;
    }
    if (mouse->button() != Qt::LeftButton)
    {
        return false;
    }

    auto const layout = calculate_layout(option, index);
    if (auto kind = index.data(message_model::content_role).toString();
        !kind.isEmpty() && kind != QStringLiteral("message") &&
        QRect(layout.text_origin(option.rect), QSize(layout.text_width, layout.text_height)).contains(mouse->position().toPoint()))
    {
        emit content_clicked(index);
        return true;
    }
    if (layout.markdown)
    {
        auto const point = mouse->position() - layout.text_origin(option.rect) - QPoint(0, layout.prefix_height);
        auto const code = layout.markdown->code_for_link(QUrl(layout.markdown->documentLayout()->anchorAt(point)));
        if (!code.isNull())
        {
            QApplication::clipboard()->setText(code);
            return true;
        }
    }
    if (auto url = layout.link_at(option.rect, mouse->position().toPoint(), index); !url.isEmpty())
    {
        QDesktopServices::openUrl(url);
        return true;
    }
    auto const bubble = layout.bubble_rect(option.rect);
    auto const y = bubble.top();
    auto const bubble_x = bubble.left();
    auto const content_right = bubble_x + layout.bubble_width - chat_theme::message_padding_horizontal;
    auto const metadata_width = layout.time_width + layout.receipt_width + ((layout.time_width > 0 && layout.receipt_width > 0) ? 3 : 0);
    auto const content_top = y + chat_theme::message_padding_vertical + layout.image_height;
    if (layout.image_height > 0 &&
        QRect(bubble_x + chat_theme::message_padding_horizontal, content_top - layout.image_height, layout.image_width, layout.image_height - 8)
            .contains(mouse->position().toPoint()))
    {
        emit image_clicked(index);
        return true;
    }
    for (int i = 0; i < layout.reactions.size(); ++i)
    {
        auto const rect =
            layout.reaction_rects[i].translated(bubble_x + chat_theme::message_padding_horizontal,
                                                y + layout.bubble_height - chat_theme::message_padding_vertical - layout.reactions_height);
        if (rect.contains(mouse->position().toPoint()))
        {
            emit reaction_clicked(index, layout.reactions[i].emoji);
            return true;
        }
    }
    auto const metadata_y =
        layout.time_on_text_line ? content_top + std::max(0, (layout.text_height - layout.time_height) / 2) : content_top + layout.text_height + 2;
    QRect read_rect(
        content_right - metadata_width,
        metadata_y,
        QFontMetrics(time_font(option)).horizontalAdvance(QStringLiteral("已读 %1 人").arg(index.data(message_model::read_count_role).toInt())),
        layout.time_height);
    if (index.data(message_model::read_count_role).isValid() && read_rect.contains(mouse->position().toPoint()))
    {
        emit read_details_clicked(index);
        return true;
    }
    if (!layout.avatar_rect(option.rect).contains(mouse->position().toPoint()))
    {
        return false;
    }

    emit avatar_clicked(index);
    return true;
}
