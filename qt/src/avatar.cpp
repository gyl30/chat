#include "avatar.hpp"

#include <array>

#include <QFont>
#include <QPainter>
#include <QPixmap>

QString avatar_initial(QString const& username)
{
    auto const value = username.trimmed();
    return value.isEmpty() ? QStringLiteral("?") : value.left(1).toUpper();
}

QColor avatar_background(QString const& username)
{
    static constexpr std::array<char const*, 6> palette = {
        "#DCE9E1",
        "#E9E1D8",
        "#DDE6EE",
        "#E7E0EB",
        "#E9E6D7",
        "#DCE8E8",
    };
    return QColor(QString::fromLatin1(palette[qHash(username.trimmed()) % palette.size()]));
}

void paint_avatar(QPainter& painter, QRect const& rect, QString const& username, int font_size)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(avatar_background(username));
    painter.drawEllipse(rect);

    auto font = painter.font();
    font.setBold(true);
    font.setPixelSize(font_size > 0 ? font_size : rect.height() * 9 / 22);
    painter.setFont(font);
    painter.setPen(QColor(QStringLiteral("#315A4B")));
    painter.drawText(rect, Qt::AlignCenter, avatar_initial(username));
    painter.restore();
}

QIcon avatar_icon(QString const& username, int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paint_avatar(painter, pixmap.rect(), username);
    return QIcon(pixmap);
}
