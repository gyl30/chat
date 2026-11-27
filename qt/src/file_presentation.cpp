#include "file_presentation.hpp"

#include "icons.hpp"
#include "theme_manager.hpp"

#include <QLocale>
#include <QPainter>

QString file_size(qint64 bytes) { return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeIecFormat); }

void paint_file_row(
    QPainter& painter, QRect const& rect, QFont font, QString const& name, QString const& details, file_transfer const& transfer, QStringView glyph)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    auto const icon = QRect(rect.left(), rect.top() + 6, 42, 42);
    auto const accent = themed("accent");
    auto const foreground = themed("on_accent");
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawEllipse(icon);
    if (transfer.progress >= 0)
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(themed("on_accent"), 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(icon.adjusted(4, 4, -4, -4), 90 * 16, -qRound(360 * 16 * transfer.progress));
    }
    auto const center = icon.center() + QPoint(1, 1);
    painter.setPen(QPen(foreground, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    switch (transfer.action)
    {
        case file_action::pause:
            for (auto const x : {-3, 3})
            {
                painter.drawLine(center + QPoint(x, -6), center + QPoint(x, 6));
            }
            break;
        case file_action::resume:
            painter.setPen(Qt::NoPen);
            painter.setBrush(foreground);
            painter.drawPolygon(QPolygon{center + QPoint(-4, -7), center + QPoint(7, 0), center + QPoint(-4, 7)});
            break;
        case file_action::download:
            painter.drawLine(center + QPoint(0, -7), center + QPoint(0, 5));
            painter.drawPolyline(QPolygon{center + QPoint(-5, 0), center + QPoint(0, 5), center + QPoint(5, 0)});
            break;
        default:
        {
            auto const symbol = transfer.action == file_action::cancel ? u"close"
                                : transfer.action == file_action::open ? QStringView(u"check")
                                                                       : glyph;
            svg_icon(symbol, foreground).paint(&painter, QRect(center - QPoint(10, 10), QSize(20, 20)));
            break;
        }
    }
    auto const text = rect.adjusted(52, 0, 0, 0);
    if (text.width() > 0)
    {
        font.setPixelSize(16);
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.setPen(themed("text"));
        painter.drawText(QRect(text.left(), text.top() + 5, text.width(), 24),
                         Qt::AlignVCenter,
                         QFontMetrics(font).elidedText(name, Qt::ElideMiddle, text.width()));
        font.setPixelSize(13);
        font.setWeight(QFont::Normal);
        painter.setFont(font);
        painter.setPen(themed("text_secondary"));
        painter.drawText(QRect(text.left(), text.top() + 29, text.width(), 19),
                         Qt::AlignVCenter,
                         QFontMetrics(font).elidedText(details, Qt::ElideRight, text.width()));
    }
    painter.restore();
}
