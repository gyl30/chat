#include "message_card.hpp"
#include <chat/message.hpp>
#include "theme_manager.hpp"
#include <QPainter>

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

int layout_poll_card(QPainter* painter, QFont const& font, int width, QString const& question, chat::poll_summary const& poll)
{
    auto const title = title_font(font);
    QFontMetrics const metrics(font);
    // Use the very same wrapping flags and width when measuring and painting.
    int const flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWrapAnywhere;
    auto text_row = [&](int y, int available, QFont const& face, QString const& text, QColor ink)
    {
        QRect const rect = QFontMetrics(face).boundingRect(QRect(0, y, available, 0), flags, text);
        if (painter)
        {
            painter->setFont(face);
            painter->setPen(ink);
            painter->drawText(QRect(0, y, available, rect.height()), flags, text);
        }
        return rect.height();
    };
    int y = text_row(0, width, title, question, themed("text")) + 4;
    qint64 total = 0;
    for (auto const& option : poll.options)
    {
        total += option.votes;
    }
    y += text_row(y, width, font, QStringLiteral("实名单选 · 共 %1 票").arg(total), themed("text_secondary")) + 10;
    for (auto const& option : poll.options)
    {
        double share = total ? double(option.votes) / total : 0;
        auto const tally = QStringLiteral("%1 票 · %2%").arg(option.votes).arg(qRound(share * 100));
        int const available = width - metrics.horizontalAdvance(tally) - 12;
        int const height = text_row(y, available, font, QString::fromStdString(option.label), themed("text"));
        if (painter)
        {
            painter->setFont(font);
            painter->setPen(themed("text_secondary"));
            painter->drawText(QRect(0, y, width, metrics.height()), Qt::AlignRight | Qt::AlignTop, tally);
            painter->setPen(Qt::NoPen);
            QRectF track(0, y + height + 3, width, 4);
            painter->setBrush(themed("border"));
            painter->drawRoundedRect(track, 2, 2);
            if (option.votes)
            {
                track.setWidth(width * share);
                painter->setBrush(themed("accent"));
                painter->drawRoundedRect(track, 2, 2);
            }
        }
        y += height + 15;
    }
    return card_footer(painter,
                       font,
                       y,
                       width,
                       poll.closed ? QStringLiteral("投票已结束") : QStringLiteral("投票进行中"),
                       poll.closed ? QStringLiteral("查看结果 ›") : QStringLiteral("参与投票 ›"));
}
