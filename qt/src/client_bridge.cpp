#include "client_bridge.hpp"

#include <cstddef>
#include <expected>
#include <string>

#include <QByteArray>

#include <chat/client.hpp>

namespace
{

std::string to_utf8(QString const& value)
{
    auto const bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

QString from_utf8(std::string const& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

}    // namespace

client_bridge::client_bridge(QObject* parent) : QObject(parent), client_(std::make_unique<chat::client>())
{
    client_->set_connected_handler([this] { emit connected(); });
    client_->set_disconnected_handler([this] { emit disconnected(); });
    client_->set_error_handler([this](chat::error const& value) { emit error(from_utf8(value.message)); });
}

client_bridge::~client_bridge() { client_.reset(); }

void client_bridge::connect_to_server(QString const& url) { client_->connect(to_utf8(url)); }

void client_bridge::close() { client_->close(); }

void client_bridge::authenticate(QString const& username, QString const& password)
{
    client_->authenticate(to_utf8(username), to_utf8(password), [this](std::expected<bool, chat::error> result) {
        if (!result)
        {
            emit authentication_finished(false, from_utf8(result.error().message));
            return;
        }
        emit authentication_finished(*result, {});
    });
}
