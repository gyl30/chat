#include "message_card.hpp"
#include "avatar.hpp"

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

bool has_poll_results(chat::poll_summary const& p) { return !p.anonymous && p.participants && (p.closed || p.selection); }

QList<int> poll_percentages(chat::poll_summary const& p)
{
    QList<int> result(p.options.size(), 0);
    if (!p.participants)
    {
        return result;
    }
    QList<int> order;
    int left = 100;
    auto remainder = [&](int i) { return p.options[i].votes * 100 % p.participants; };
    for (int i = 0; i < result.size(); ++i)
    {
        result[i] = int(100 * p.options[i].votes / p.participants);
        left -= result[i];
        order.append(i);
    }
    if (left <= 0 || left > result.size())
    {
        return result;
    }
    std::ranges::sort(order,
                      [&](int a, int b)
                      {
                          if (remainder(a) != remainder(b))
                          {
                              return remainder(a) > remainder(b);
                          }
                          return result[a] < result[b];
                      });
    for (int i = 0; i < order.size() && left > 0;)
    {
        int j = i + 1;
        auto first = order[i];
        if (!remainder(first))
        {
            break;
        }
        while (j < order.size() && remainder(order[j]) == remainder(first) && result[order[j]] == result[first])
        {
            ++j;
        }
        if (j - i <= left)
        {
            left -= j - i;
            for (int k = i; k < j; ++k)
            {
                ++result[order[k]];
            }
        }
        i = j;
    }
    return result;
}
int layout_poll_card(QPainter* painter,
                     QFont const& font,
                     int width,
                     QString const& question,
                     chat::poll_summary const& poll,
                     QPoint const* point,
                     int* action,
                     bool pending,
                     qint64 draft,
                     QString* tooltip,
                     avatar_cache const* avatars)
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
    const bool results = poll.selection || poll.closed;
    QString type = poll.anonymous ? QStringLiteral("匿名") : QStringLiteral("实名");
    type += poll.quiz ? QStringLiteral("问答") : (poll.multiple ? QStringLiteral("多选投票") : QStringLiteral("投票"));
    if (poll.closed)
    {
        type += QStringLiteral(" · 已结束");
    }
    const int solution_width = poll.explanation.empty() ? 0 : 26;
    const int avatar_width = int(poll.recent.size()) * 15 + 5;
    const int type_height = text_row(y, 0, width - avatar_width - solution_width, font, type, themed("text_secondary"));
    if (painter)
    {
        int x = std::min(width - avatar_width - solution_width, metrics.horizontalAdvance(type) + 8);
        for (const auto& person : poll.recent)
        {
            paint_avatar(*painter,
                         {x, y, 18, 18},
                         QString::fromStdString(person.nickname.empty() ? person.username : person.nickname),
                         10,
                         avatars ? avatars->image(person.id) : QPixmap{});
            x += 15;
        }
    }
    if (solution_width)
    {
        QRect hit(width - solution_width, y - 2, solution_width, 24);
        if (painter)
        {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(themed("link"), 1.5));
            painter->drawEllipse(hit.center(), 8, 8);
            painter->setFont(title);
            painter->drawText(hit, Qt::AlignCenter, QStringLiteral("?"));
        }
        if (point && hit.contains(*point))
        {
            if (action)
            {
                *action = poll_solution;
            }
            if (tooltip)
            {
                *tooltip = QStringLiteral("查看答案解析");
            }
        }
    }
    y += std::max(18, type_height) + 8;
    auto percents = poll_percentages(poll);
    qint64 maximum = 0;
    for (const auto& o : poll.options)
    {
        maximum = std::max<qint64>(maximum, o.votes);
    }
    for (const auto& option : poll.options)
    {
        auto bit = chat::poll_bit(option.position);
        bool chosen = (poll.selection & bit) != 0;
        bool correct = poll.correct == option.position;
        int left = results ? metrics.horizontalAdvance(QStringLiteral("100%")) + 12 : 30;
        int h = text_row(y + 5, left, width - left, font, QString::fromStdString(option.label), themed("text"));
        QRect row(0, y, width, h + (results ? 18 : 12));
        auto ink = themed(correct ? "online" : poll.quiz && chosen ? "danger" : "accent");
        if (painter)
        {
            painter->setFont(font);
            if (results)
            {
                painter->setPen(themed("text_secondary"));
                painter->drawText(QRect(0, y + 5, left - 8, metrics.height()), Qt::AlignRight, QString::number(percents[option.position - 1]) + u'%');
                if (option.votes)
                {
                    QRectF bar(left, y + h + 9, (width - left) * double(option.votes) / maximum, 4);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(ink);
                    painter->drawRoundedRect(bar, 2, 2);
                }
                if (chosen || correct)
                {
                    painter->setPen(QPen(ink, 2));
                    QPoint p(left - 16, y + h + 10);
                    if (poll.quiz && !correct)
                    {
                        painter->drawLine(p - QPoint(3, 3), p + QPoint(3, 3));
                        painter->drawLine(p - QPoint(3, -3), p + QPoint(3, -3));
                    }
                    else
                    {
                        painter->drawLine(p - QPoint(4, 1), p + QPoint(-1, 2));
                        painter->drawLine(p + QPoint(-1, 2), p + QPoint(5, -4));
                    }
                }
            }
            else
            {
                QPoint center(12, y + 5 + metrics.height() / 2);
                painter->setPen(QPen(themed("accent"), 1.6));
                painter->setBrush(Qt::NoBrush);
                if (pending && (draft & bit))
                {
                    painter->drawArc(QRect(center - QPoint(8, 8), QSize(16, 16)), 40 * 16, 260 * 16);
                }
                else
                {
                    if (draft & bit)
                    {
                        painter->setBrush(themed("accent"));
                    }
                    painter->drawEllipse(center, 8, 8);
                    if (draft & bit)
                    {
                        painter->setPen(QPen(themed("on_accent"), 1.6));
                        painter->drawLine(center - QPoint(4, 0), center + QPoint(-1, 3));
                        painter->drawLine(center + QPoint(-1, 3), center + QPoint(4, -3));
                    }
                }
            }
        }
        if (point && row.contains(*point))
        {
            if (results && tooltip)
            {
                *tooltip = QStringLiteral("%1 票").arg(option.votes);
            }
            if (!results && !pending && action)
            {
                *action = int(option.position);
            }
        }
        y += row.height();
    }
    auto footer = [&](QString label, int command, bool enabled)
    {
        QRect rect(0, y, width, metrics.height() + 14);
        if (painter)
        {
            painter->setPen(themed("border"));
            painter->drawLine(0, y, width - 1, y);
            painter->setPen(themed(command && enabled ? "link" : "text_secondary"));
            painter->setFont(font);
            painter->drawText(rect, Qt::AlignCenter, label);
        }
        if (enabled && action && point && rect.contains(*point))
        {
            *action = command;
        }
        y += rect.height();
    };
    if (has_poll_results(poll))
    {
        footer(QStringLiteral("查看结果"), poll_results, true);
    }
    else if (poll.multiple && !results)
    {
        footer(QStringLiteral("提交投票"), poll_submit, draft && !pending);
    }
    else
    {
        auto count = QStringLiteral("%1 人参与").arg(poll.participants);
        if (poll.anonymous && !poll.multiple)
        {
            y += text_row(y + 3, 0, width, font, count, themed("text_secondary")) + 4;
        }
        else
        {
            y += text_row(y + 6, 0, width, font, count, themed("text_secondary")) + 10;
        }
    }
    return y;
}
