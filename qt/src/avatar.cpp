#include "avatar.hpp"
#include "emoji_segments.hpp"
#include "emoji_text.hpp"
#include "theme_manager.hpp"

#include <algorithm>

#include <QFont>
#include <QFileDialog>
#include <QFile>
#include <QPainter>
#include <QPixmap>
#include <QBuffer>
#include <QImageReader>
#include <QPainterPath>

QString avatar_initial(QString const& username)
{
    auto const value = username.trimmed();
    return value.isEmpty() ? QStringLiteral("?") : value.left(grapheme_ends(value).front()).toUpper();
}

void paint_avatar(QPainter& painter, QRect const& rect, QString const& username, int font_size, QPixmap const& image, quint64 color_key)
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
    painter.setBrush(theme_manager::instance().avatar_background(color_key ? color_key : qHash(username.trimmed())));
    painter.drawEllipse(rect);

    auto font = painter.font();
    font.setBold(true);
    font.setPixelSize(font_size > 0 ? font_size : rect.height() * 9 / 22);
    painter.setFont(font);
    paint_emoji_line(painter, rect, avatar_initial(username), font, theme_manager::instance().avatar_text(), Qt::AlignHCenter);
    painter.restore();
}

QIcon avatar_icon(QString const& username, int size, QPixmap const& image, quint64 color_key)
{
    QPixmap pixmap(size * 2, size * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paint_avatar(painter, QRect(0, 0, size, size), username, 0, image, color_key);
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

std::expected<QByteArray, QString> choose_avatar(QWidget* parent)
{
    auto path = QFileDialog::getOpenFileName(parent, QStringLiteral("更换头像"), {}, QStringLiteral("图片 (*.png *.jpg *.jpeg)"));
    if (path.isEmpty())
    {
        return QByteArray{};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > static_cast<qint64>(chat::max_avatar_size))
    {
        return std::unexpected(QStringLiteral("无法读取图片，头像文件不得超过 1 MiB。"));
    }
    auto bytes = file.read(chat::max_avatar_size + 1);
    auto const original = decode_avatar(bytes);
    if (original.isNull())
    {
        return std::unexpected(QStringLiteral("请选择完整的 PNG 或 JPEG 图片，最多 1600 万像素。"));
    }
    // Draw into a fresh image so source metadata does not reach the upload.
    auto const extent = chat::avatar_extent;
    QImage square(extent, extent, original.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
    auto const side = std::min(original.width(), original.height());
    QPainter painter(&square);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(square.rect(), original, QRect((original.width() - side) / 2, (original.height() - side) / 2, side, side));
    painter.end();

    QByteArray png;
    QBuffer output(&png);
    if (!output.open(QIODevice::WriteOnly) || !square.save(&output, "PNG") || png.size() > static_cast<qsizetype>(chat::max_avatar_size))
    {
        return std::unexpected(QStringLiteral("无法生成 PNG 头像，请尝试其他图片。"));
    }
    return png;
}

void avatar_cache::observe(qint64 user, chat::avatar_state state)
{
    if (user == 0)
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
        // The largest avatar is 104 logical pixels at 200% scaling.
        auto const side = std::min(image.width(), image.height());
        entries_[user].image = QPixmap::fromImage(image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
                                                      .scaled(208, 208, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
