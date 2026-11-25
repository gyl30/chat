#include "message_card.hpp"
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
