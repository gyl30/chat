#include "icons.hpp"

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
    data.replace("currentColor", color.name().toUtf8());
    QSvgRenderer renderer(data);
    if (!renderer.isValid())
    {
        return {};
    }

    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
    return QIcon(pixmap);
}
