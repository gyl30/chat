#include "icons.hpp"
#include "theme_manager.hpp"

#include <QByteArray>
#include <QFile>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

QIcon svg_icon(QStringView name, QColor const& color, QSize size)
{
    QFile file(QStringLiteral(":/icons/%1.svg").arg(name.toString()));
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }

    auto data = file.readAll();
    // White marks an icon drawn on an accent or navigation ground.
    auto const ink = color == QColor(Qt::white) ? theme_manager::instance().on_accent() : themed(color);
    data.replace("currentColor", ink.name().toUtf8());
    QSvgRenderer renderer(data);
    if (!renderer.isValid())
    {
        return {};
    }

    QPixmap pixmap(size * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
    return QIcon(pixmap);
}
