#include "message_card.hpp"
#include <chat/message.hpp>
#include "theme_manager.hpp"
#include <QPainter>
#include <QTextLayout>

namespace
{
QFont title_font(QFont font)
{
    font.setBold(true);
    return font;
}

int card_footer(QPainter* painter, QFont const& font, int y, int width, QString const& state, QString const& action)
{
    int const height = QFontMetrics(font).height();
    if (painter)
    {
        painter->setPen(themed("border"));
        painter->drawLine(0, y, width - 1, y);
        QRect row(0, y + 6, width, height);
        painter->setFont(font);
        painter->setPen(themed("text_secondary"));
        painter->drawText(row, Qt::AlignLeft | Qt::AlignVCenter, state);
        painter->setPen(themed("link"));
        painter->drawText(row, Qt::AlignRight | Qt::AlignVCenter, action);
    }
    return y + height + 10;
}
}    // namespace

int layout_bundle_card(QPainter* painter, QFont const& font, int width, QString const& summary)
{
    // The server supplies a presentation envelope: title, up to three source excerpts, then a count line.
    // Line breaks inside an excerpt are ordinary text and do not identify authors or attachments.
    auto const lines = QStringView(summary).split(u'\n');
    QFontMetrics const metrics(font);
    auto const title = title_font(font);
    int y = QFontMetrics(title).height() + 6;
    if (painter)
    {
        painter->setPen(themed("text"));
        painter->setFont(title);
        painter->drawText(QRect(0, 0, width, y), Qt::AlignTop, QStringLiteral("聊天记录"));
        painter->setFont(font);
        painter->setPen(themed("text_secondary"));
    }
    for (qsizetype i = 1; i + 1 < lines.size() && i <= 3; ++i)
    {
        if (painter)
        {
            auto line = lines[i].toString();
            if (i == 3 && lines.size() > 5)
            {
                line += QChar(0x2026);
            }
            painter->drawText(QRect(0, y, width, metrics.height()), Qt::AlignLeft, metrics.elidedText(line, Qt::ElideRight, width));
        }
        y += metrics.height() + 2;
    }
    return card_footer(painter, font, y + 5, width, lines.size() > 1 ? lines.last().toString() : QString{}, QStringLiteral("查看记录 ›"));
}

int layout_poll_card(QPainter* painter,
                     QFont const& font,
                     int width,
                     QString const& question,
                     chat::poll_summary const& poll,
                     QPoint const* point,
                     int* action,
                     bool pending)
{
    auto const title = title_font(font);
    QFontMetrics const metrics(font);
    auto text_row = [&](int y, int x, int available, QFont const& face, QString text, QColor ink)
    {
        // Native Unicode line breaking keeps words and closing punctuation together where possible.
        QTextLayout paragraph(text.replace(u'\n', QChar::LineSeparator), face);
        QTextOption wrap;
        wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        paragraph.setTextOption(wrap);
        int height = 0;
        paragraph.beginLayout();
        for (auto line = paragraph.createLine(); line.isValid(); line = paragraph.createLine())
        {
            line.setLineWidth(available);
            line.setPosition({0., qreal(height)});
            height += qCeil(line.height());
        }
        paragraph.endLayout();
        if (painter)
        {
            painter->setPen(ink);
            paragraph.draw(painter, {qreal(x), qreal(y)});
        }
        return height;
    };
    int y = text_row(0, 0, width, title, question, themed("text")) + 4;
    qint64 total = 0;
    for (auto const& option : poll.options)
    {
        total += option.votes;
    }
    const bool results = poll.closed || poll.selection != 0;
    y += text_row(y, 0, width, font, poll.closed ? QStringLiteral("实名单选 · 已结束") : QStringLiteral("实名单选"), themed("text_secondary")) + 8;
    for (auto const& option : poll.options)
    {
        const int left = results ? metrics.horizontalAdvance(QStringLiteral("100%")) + 12 : 30;
        const int height = text_row(y + 5, left, width - left, font, QString::fromStdString(option.label), themed("text"));
        const double share = total ? double(option.votes) / total : 0;
        const QRect row(0, y, width, height + (results ? 18 : 12));
        if (painter)
        {
            painter->setFont(font);
            if (results)
            {
                painter->setPen(themed("text_secondary"));
                painter->drawText(QRect(0, y + 5, left - 8, metrics.height()), Qt::AlignRight, QString::number(qRound(share * 100)) + u'%');
                QRectF track(left, y + height + 9, width - left, 4);
                painter->setPen(Qt::NoPen);
                painter->setBrush(themed("border"));
                painter->drawRoundedRect(track, 2, 2);
                painter->setBrush(themed("accent"));
                track.setWidth(track.width() * share);
                painter->drawRoundedRect(track, 2, 2);
                if ((chat::poll_bit(option.position) & poll.selection) != 0)
                {
                    painter->setPen(QPen(themed("accent"), 2));
                    const QPoint p(left - 17, y + height + 10);
                    painter->drawLine(p - QPoint(4, 1), p + QPoint(-1, 2));
                    painter->drawLine(p + QPoint(-1, 2), p + QPoint(5, -4));
                }
            }
            else
            {
                painter->setPen(QPen(themed("accent"), 1.5));
                painter->setBrush(Qt::NoBrush);
                painter->drawEllipse(QPoint(12, y + 5 + metrics.height() / 2), 7, 7);
            }
        }
        if (!results && !pending && point && row.contains(*point))
        {
            *action = static_cast<int>(option.position);
        }
        y += row.height();
    }
    const QString link = results ? QStringLiteral("查看结果 ›") : QString{};
    int bottom = card_footer(painter, font, y, width, pending ? QStringLiteral("正在提交…") : QStringLiteral("%1 人已投票").arg(total), link);
    if (point && results && QRect(width - metrics.horizontalAdvance(link) - 4, y, metrics.horizontalAdvance(link) + 4, bottom - y).contains(*point))
    {
        *action = -1;
    }
    return bottom;
}
