#include "chat_background.hpp"
#include "theme_manager.hpp"
#include <QLinearGradient>
#include <QPainter>
#include <QSvgRenderer>
#include <QtMath>

void paint_chat_background(QPainter& painter, const QRect& area, chat_background choice, bool dark)
{
    if (choice == chat_background::solid)
    {
        painter.fillRect(area, themed("canvas"));
        return;
    }
    struct gradient_colors
    {
        QColor light_start;
        QColor light_end;
        QColor dark_start;
        QColor dark_end;
    };
    static const gradient_colors schemes[]{
        {QColor(0xdcebd2), QColor(0xb0d8c1), QColor(0x1d372e), QColor(0x254b43)},
        {QColor(0xe4f0fa), QColor(0xbccff0), QColor(0x1b2c41), QColor(0x2d4560)},
        {QColor(0xf5eadd), QColor(0xe4c9ad), QColor(0x352a24), QColor(0x514033)},
        {QColor(0xd8e1ef), QColor(0xb4c1d9), QColor(0x192534), QColor(0x2d3a52)},
    };
    const auto& colors = schemes[static_cast<int>(choice) - 1];
    QLinearGradient gradient(area.topLeft(), area.bottomRight());
    gradient.setColorAt(0, dark ? colors.dark_start : colors.light_start);
    gradient.setColorAt(1, dark ? colors.dark_end : colors.light_end);
    painter.fillRect(area, gradient);
    static QSvgRenderer pattern(QStringLiteral(":/chat-background/linework.svg"));
    constexpr int tile_size = 192;
    // Rasterize at the device scale. Paint the viewport without scaling the pattern to fit.
    static QPixmap tile;
    const auto scale = painter.device()->devicePixelRatioF();
    if (tile.isNull() || tile.devicePixelRatio() != scale)
    {
        tile = QPixmap(qCeil(tile_size * scale), qCeil(tile_size * scale));
        tile.setDevicePixelRatio(scale);
        tile.fill(Qt::transparent);
        QPainter texture(&tile);
        pattern.render(&texture, QRectF(0, 0, tile_size, tile_size));
    }
    painter.save();
    painter.setOpacity(dark ? 0.22 : 0.13);
    painter.drawTiledPixmap(area, tile);
    painter.restore();
}
