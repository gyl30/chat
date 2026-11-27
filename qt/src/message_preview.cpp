#include "message_preview.hpp"

#include "avatar.hpp"
#include "theme_manager.hpp"

#include <QDateTime>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <chat/message.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <utility>

namespace
{
QString case_lower(QString const& text)
{
    auto points = text.toStdU32String();
    for (auto& ch : points)
    {
        ch = utf8proc_tolower(ch);
    }
    return QString::fromUcs4(points.data(), points.size());
}

QString preview_time(qint64 timestamp)
{
    auto const time = QDateTime::fromMSecsSinceEpoch(timestamp);
    auto const today = QDate::currentDate();
    if (time.date() == today)
    {
        return time.toString(QStringLiteral("HH:mm"));
    }
    if (time.date() == today.addDays(-1))
    {
        return QStringLiteral("昨天 ") + time.toString(QStringLiteral("HH:mm"));
    }
    return time.toString(time.date().year() == today.year() ? QStringLiteral("M月d日 HH:mm") : QStringLiteral("yyyy/M/d HH:mm"));
}

QString near_match(QString text, QString const& needle, QFontMetrics const& metrics, int width)
{
    auto const match = needle.isEmpty() ? -1 : case_lower(text).indexOf(needle);
    if (match <= 0 || metrics.horizontalAdvance(text.left(match)) <= width / 3)
    {
        return text;
    }
    // Retain a little context, without splitting a surrogate pair or Emoji sequence.
    QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, text);
    boundary.setPosition(match);
    for (int i = 0; i < 8 && boundary.position() > 0; ++i)
    {
        boundary.toPreviousBoundary();
    }
    return QStringLiteral("…") + text.mid(std::max<qsizetype>(0, boundary.position()));
}

void draw_highlighted(QPainter& painter, QPoint top, QString const& text, QString const& needle, QFont const& font)
{
    QTextLayout layout(text, font);
    QList<QTextLayout::FormatRange> matches;
    if (!needle.isEmpty())
    {
        auto const lowered = case_lower(text);
        QTextCharFormat accent;
        accent.setForeground(themed("link"));
        accent.setFontUnderline(true);
        for (qsizetype pos = 0; (pos = lowered.indexOf(needle, pos)) >= 0; pos += needle.size())
        {
            matches.append({static_cast<int>(pos), static_cast<int>(needle.size()), accent});
        }
    }
    layout.setFormats(matches);
    layout.beginLayout();
    auto line = layout.createLine();
    if (line.isValid())
    {
        line.setNumColumns(text.size());
    }
    layout.endLayout();
    layout.draw(&painter, top);
}

void draw_excerpt(QPainter& painter, QRect area, QString const& snippet, QString const& needle, QFont const& font, bool two_lines)
{
    if (area.width() <= 0)
    {
        return;
    }
    QFontMetrics const metrics(font);
    auto text = near_match(snippet.simplified(), needle, metrics, area.width());
    if (two_lines && metrics.horizontalAdvance(text) > area.width())
    {
        QTextLayout wrap(text, font);
        QTextOption flow;
        flow.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        wrap.setTextOption(flow);
        wrap.beginLayout();
        auto first = wrap.createLine();
        if (first.isValid())
        {
            first.setLineWidth(area.width());
            auto const count = first.textLength();
            draw_highlighted(painter, area.topLeft(), text.left(count).trimmed(), needle, font);
            area.translate(0, metrics.height());
            text = text.mid(count).trimmed();
        }
        wrap.endLayout();
    }
    draw_highlighted(painter, area.topLeft(), metrics.elidedText(text, Qt::ElideRight, area.width()), needle, font);
}

class preview_delegate final : public QStyledItemDelegate
{
   public:
    preview_delegate(QLineEdit& input, avatar_cache* avatars, bool compact, QListWidget& list)
        : QStyledItemDelegate(&list), input_(input), avatars_(avatars), compact_(compact)
    {
    }

    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        if (index.data(preview_role::message).isValid())
        {
            return {0, index.data(preview_role::note).toString().isEmpty() ? (compact_ ? 55 : 70) : 87};
        }
        if (index.data(preview_role::directory).isValid())
        {
            return {0, 44};
        }
        return {0, std::max(30, option.fontMetrics.height() + 10)};
    }

    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        painter->save();
        painter->setClipRect(option.rect);
        auto const row = option.rect;
        bool const selected = option.state.testFlag(QStyle::State_Selected);
        if (selected || ((index.flags() & Qt::ItemIsSelectable) && option.state.testFlag(QStyle::State_MouseOver)))
        {
            painter->fillRect(row, themed(selected ? "selected" : "canvas_hover"));
        }

        auto font = option.font;
        font.setPixelSize(14);
        auto const needle = case_lower(input_.text().simplified());
        auto const text = index.data(Qt::DisplayRole).toString();
        if (index.data(preview_role::message).isValid())
        {
            paint_message(*painter, row, index, text, needle, font);
        }
        else if (index.data(preview_role::directory).isValid())
        {
            QRect const avatar(row.left() + 10, row.center().y() - 15, 32, 32);
            auto const room_or_user = index.data(preview_role::directory).toLongLong();
            painter->setRenderHint(QPainter::Antialiasing);
            paint_avatar(*painter, avatar, text, 14, avatars_->image(room_or_user), static_cast<quint64>(qAbs(room_or_user)));
            painter->setPen(themed("text"));
            draw_excerpt(*painter,
                         QRect(avatar.right() + 10, row.center().y() - QFontMetrics(font).height() / 2, row.right() - avatar.right() - 20, 20),
                         text, needle, font, false);
        }
        else
        {
            auto const more = index.data(preview_role::more).toBool();
            font.setPixelSize(13);
            font.setBold(more || index.data(preview_role::heading).toBool());
            painter->setFont(font);
            painter->setPen(themed(more ? "link" : "text_secondary"));
            painter->drawText(row.adjusted(12, 0, -10, 0), Qt::AlignVCenter | (more ? Qt::AlignHCenter : Qt::AlignLeft),
                              QFontMetrics(font).elidedText(text, Qt::ElideRight, row.width() - 22));
        }
        painter->restore();
    }

   private:
    void paint_message(QPainter& painter, QRect row, QModelIndex const& index, QString const& text, QString const& needle, QFont font) const
    {
        auto const sender = index.data(preview_role::sender).toString();
        auto const title = index.data(preview_role::title).toString();
        auto const note = index.data(preview_role::note).toString();
        auto const from = index.data(preview_role::avatar).toLongLong();
        auto const time = index.data(preview_role::time).toString();
        auto const diameter = compact_ ? 32 : 40;
        QRect const avatar(row.left() + 10, row.top() + 9, diameter, diameter);
        auto const left = avatar.right() + 11;
        auto const right = row.right() - 11;
        auto const y = row.top() + (compact_ ? 8 : 7);
        painter.setRenderHint(QPainter::Antialiasing);
        if (from > 0)
        {
            paint_avatar(painter, avatar, sender, compact_ ? 14 : 17, avatars_ ? avatars_->image(from) : QPixmap{}, from);
        }

        QFont small = font;
        small.setPixelSize(12);
        QFontMetrics const small_metrics(small);
        auto const time_width = std::min(small_metrics.horizontalAdvance(time), std::max(0, (right - left) / 2));
        painter.setFont(small);
        painter.setPen(themed("text_muted"));
        painter.drawText(QRect(right - time_width, y, time_width, 19), Qt::AlignRight | Qt::AlignVCenter,
                         small_metrics.elidedText(time, Qt::ElideLeft, time_width));

        auto const name = compact_ && !title.isEmpty() ? title : sender;
        auto const source = !compact_ && !title.isEmpty() && title != name ? QStringLiteral(" · ") + title : QString{};
        auto const available = std::max(0, right - time_width - left - (time_width ? 10 : 0));
        QFont bold = font;
        bold.setBold(true);
        QFontMetrics const name_metrics(bold);
        auto name_text = name_metrics.elidedText(name, Qt::ElideRight, source.isEmpty() ? available : available / 2);
        auto const name_width = name_metrics.horizontalAdvance(name_text);
        painter.setFont(bold);
        painter.setPen(themed("text"));
        painter.drawText(QRect(left, y, name_width, 19), Qt::AlignVCenter, name_text);
        painter.setFont(small);
        painter.setPen(themed("text_secondary"));
        painter.drawText(QRect(left + name_width, y, available - name_width, 19), Qt::AlignVCenter,
                         small_metrics.elidedText(source, Qt::ElideRight, available - name_width));
        painter.setPen(themed("text"));
        draw_excerpt(painter, QRect(left, y + 21, right - left, 39), text, needle, font, !compact_);
        if (!note.isEmpty())
        {
            painter.setFont(small);
            painter.setPen(themed("text_secondary"));
            painter.drawText(QRect(left, row.bottom() - 20, right - left, 17), Qt::AlignVCenter,
                             small_metrics.elidedText(note, Qt::ElideRight, right - left));
        }
    }

    QLineEdit& input_;
    avatar_cache* avatars_;
    bool compact_;
};
}    // namespace

QString matched_message_excerpt(QString const& body, QString const& query)
{
    auto const match = query.isEmpty() ? 0 : case_lower(body).indexOf(case_lower(query));
    if (match < 0)
    {
        return {};
    }
    QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, body);
    boundary.setPosition(std::max<qsizetype>(0, match - 32));
    auto const start = boundary.isAtBoundary() ? boundary.position() : boundary.toPreviousBoundary();
    boundary.setPosition(std::min(start + 160, body.size()));
    auto const end = boundary.isAtBoundary() ? boundary.position() : boundary.toNextBoundary();
    auto text = body.mid(start, end - start);
    if (end < body.size())
    {
        text += u'…';
    }
    return text.isNull() ? QStringLiteral("") : text;
}

void set_message_preview(QListWidgetItem& item, chat::message_search_hit const& hit, QString snippet, QString note)
{
    auto const name = QString::fromStdString(hit.username);
    auto const title = QString::fromStdString(hit.title);
    auto const time = preview_time(hit.timestamp);
    item.setData(preview_role::message, qint64(hit.id));
    item.setData(preview_role::conversation, qint64(hit.conversation));
    item.setData(preview_role::avatar, qint64(hit.from));
    item.setData(preview_role::sender, name);
    item.setData(preview_role::title, title);
    item.setData(preview_role::time, time);
    item.setData(preview_role::note, std::move(note));
    set_preview_snippet(item, std::move(snippet));
}

void set_preview_snippet(QListWidgetItem& item, QString snippet)
{
    item.setText(snippet.simplified());
    item.setData(Qt::AccessibleTextRole,
                 QStringLiteral("%1 %2 %3 %4 %5").arg(item.data(preview_role::sender).toString(), item.data(preview_role::title).toString(),
                                                       item.data(preview_role::time).toString(), item.text(), item.data(preview_role::note).toString()).simplified());
}

void style_message_previews(QListWidget& list, QLineEdit& input, avatar_cache* avatars, bool compact)
{
    list.setItemDelegate(new preview_delegate(input, avatars, compact, list));
    list.setMouseTracking(true);
    list.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list.setResizeMode(QListView::Adjust);
    if (avatars)
    {
        QObject::connect(avatars, &avatar_cache::changed, &list, [view = list.viewport()] { view->update(); });
    }
}
