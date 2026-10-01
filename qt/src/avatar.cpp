#include "avatar.hpp"

#include <array>
#include <algorithm>

#include <QFont>
#include <QPainter>
#include <QPixmap>
#include <QBuffer>
#include <QImageReader>
#include <QPainterPath>

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

void paint_avatar(QPainter& painter, QRect const& rect, QString const& username, int font_size, QPixmap const& image)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (!image.isNull())
    {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.addEllipse(rect);
        painter.setClipPath(clip);
        auto const side = std::min(image.width(), image.height());
        painter.drawPixmap(rect, image, QRect((image.width() - side) / 2, (image.height() - side) / 2, side, side));
        painter.restore();
        return;
    }
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

QIcon avatar_icon(QString const& username, int size, QPixmap const& image)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paint_avatar(painter, pixmap.rect(), username, 0, image);
    return QIcon(pixmap);
}

QImage decode_avatar(QByteArray const& bytes)
{
    if (bytes.isEmpty() || bytes.size() > static_cast<qsizetype>(chat::max_avatar_size))
    {
        return {};
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    auto const format = reader.format();
    auto const size = reader.size();
    if ((format != "png" && format != "jpeg" && format != "jpg") || !size.isValid() ||
        qint64(size.width()) * size.height() > static_cast<qint64>(chat::max_avatar_pixels))
    {
        return {};
    }
    return reader.read();
}

void avatar_cache::observe(qint64 user, chat::avatar_state state)
{
    if (user <= 0)
    {
        return;
    }
    auto& value = entries_[user];
    if (state.revision < value.state.revision)
    {
        return;
    }
    if (state != value.state)
    {
        value = {state, {}, false};
        emit changed(user);
    }
    if (value.state.present && !value.attempted)
    {
        value.attempted = true;
        emit requested(user, state.revision);
    }
}

void avatar_cache::receive(qint64 user, chat::avatar_state state, QByteArray const& bytes)
{
    if (!entries_.contains(user) || state.revision < entries_[user].state.revision)
    {
        return;
    }
    if (state != entries_[user].state)
    {
        observe(user, state);
        return;
    }
    if (!state.present || bytes.isEmpty())
    {
        return;
    }
    auto const image = decode_avatar(bytes);
    if (!image.isNull())
    {
        entries_[user].image = QPixmap::fromImage(image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        emit changed(user);
    }
}

QPixmap avatar_cache::image(qint64 user) const { return entries_.value(user).image; }
chat::avatar_state avatar_cache::state(qint64 user) const { return entries_.value(user).state; }

void avatar_cache::retry()
{
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
    {
        if (it->state.present && it->image.isNull())
        {
            it->attempted = true;
            emit requested(it.key(), it->state.revision);
        }
    }
}

void avatar_cache::clear() { entries_.clear(); }
