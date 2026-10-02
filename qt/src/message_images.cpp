#include "message_images.hpp"

#include <algorithm>
#include <utility>
#include <QBuffer>
#include <QImageReader>
#include <chat/attachment.hpp>

void message_images::observe(qint64 conversation, qint64 message)
{
    if (cache_.contains(message) || active_.value(message).has_value() ||
        std::ranges::any_of(queued_, [message](auto const& item) { return item.second == message; })) { return; }
    queued_.push_back({conversation, message});
    pump();
}

void message_images::pump()
{
    while (active_.size() < 3 && !queued_.isEmpty())
    {
        auto const next = std::ranges::find_if(queued_, [this](auto const& item) { return !active_.contains(item.second); });
        if (next == queued_.end()) { break; }
        auto const [conversation, message] = *next;
        queued_.erase(next);
        active_.insert(message, conversation);
        emit requested(conversation, message);
    }
}

void message_images::receive(qint64 conversation, qint64 message, QByteArray bytes, QString error)
{
    auto const active = active_.constFind(message);
    if (active == active_.cend() || (active.value() && *active.value() != conversation)) { return; }
    auto const wanted = active.value().has_value();
    active_.remove(message);
    if (!wanted) { pump(); return; }
    auto* value = new entry{conversation, std::move(bytes), {}, std::move(error)};
    if (value->error.isEmpty())
    {
        QBuffer buffer(&value->bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        auto const format = reader.format();
        auto const size = reader.size();
        if (value->bytes.size() <= static_cast<qsizetype>(chat::max_attachment_size) &&
            (format == "png" || format == "jpeg" || format == "jpg") && size.width() > 0 && size.height() > 0 &&
            qint64(size.width()) * size.height() <= 16 * 1024 * 1024)
        {
            QImageReader::setAllocationLimit(64);
            reader.setScaledSize(size.scaled(QSize(640, 480), Qt::KeepAspectRatio));
            value->image = QPixmap::fromImage(reader.read());
        }
        if (value->image.isNull()) { value->error = QStringLiteral("图片无法预览；可下载原文件。"); }
    }
    if (!value->error.isEmpty() && value->bytes.isEmpty())
    {
        value->error = QStringLiteral("图片加载失败；可下载原文件。");
    }
    auto const cost = std::max<qsizetype>(1, (value->bytes.size() + qint64(value->image.width()) * value->image.height() * 4 + 1023) / 1024);
    cache_.insert(message, value, static_cast<int>(cost));
    emit changed(message);
    pump();
}

QPixmap message_images::image(qint64 message) const
{
    auto const* value = cache_.object(message);
    return value ? value->image : QPixmap{};
}

QByteArray message_images::bytes(qint64 message) const
{
    auto const* value = cache_.object(message);
    return value ? value->bytes : QByteArray{};
}

QString message_images::status(qint64 message) const
{
    if (auto const* value = cache_.object(message)) { return value->error; }
    return active_.value(message).has_value() ? QStringLiteral("正在加载图片…") : QStringLiteral("图片等待加载…");
}

void message_images::discard_queued()
{
    queued_.clear();
}

void message_images::remove(qint64 conversation, qint64 message)
{
    auto const matches = [conversation, message](qint64 conv, qint64 id) {
        return conv == conversation && (message == 0 || message == id);
    };
    for (auto id : cache_.keys())
    {
        if (matches(cache_.object(id)->conversation, id)) { cache_.remove(id); emit changed(id); }
    }
    for (auto it = active_.begin(); it != active_.end(); ++it)
    {
        if (it.value() && matches(*it.value(), it.key())) { it.value().reset(); }
    }
    queued_.removeIf([&](auto const& item) { return matches(item.first, item.second); });
    pump();
}

void message_images::retry()
{
    active_.clear();
    queued_.clear();
    for (auto id : cache_.keys())
    {
        if (cache_.object(id)->image.isNull()) { cache_.remove(id); emit changed(id); }
    }
}

void message_images::clear()
{
    active_.clear();
    queued_.clear();
    cache_.clear();
}
