#include "icons.hpp"

#include <QByteArray>
#include <QFile>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QProxyStyle>
#include <QSvgRenderer>

QIcon svg_icon(QStringView name, QColor const& color, QSize size, int inset)
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

    QPixmap pixmap(size * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(QPointF{}, QSizeF(size)).adjusted(inset, inset, -inset, -inset));
    return QIcon(pixmap);
}


namespace
{
class menu_icon_style final : public QProxyStyle
{
   public:
    explicit menu_icon_style(int size) : size_(size) {}

    int pixelMetric(PixelMetric metric, QStyleOption const* option, QWidget const* widget) const override
    {
        // Qt's styled menu renderer requests the metric without a widget.
        return metric == PM_SmallIconSize ? size_ : QProxyStyle::pixelMetric(metric, option, widget);
    }

   private:
    int size_;
};
}    // namespace

void style_menu_icons(QMenu& menu, int size)
{
    auto* style = new menu_icon_style(size);
    style->setParent(&menu);
    menu.setStyle(style);
}
