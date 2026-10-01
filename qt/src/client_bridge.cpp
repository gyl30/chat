#include "client_bridge.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QMetaType>

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

message_data to_message_data(chat::message const& value)
{
    message_data message;
    message.id = value.id;
    message.from = value.from;
    message.timestamp = value.timestamp;
    message.text = from_utf8(value.text);
    return message;
}

presence_data to_presence_data(chat::presence const& value)
{
    presence_data presence;
    presence.user = value.user;
    presence.online = value.online;
    presence.last_seen = value.last_seen;
    return presence;
}

}    // namespace

client_bridge::client_bridge(QObject* parent) : QObject(parent), client_(std::make_unique<chat::client>())
{
    qRegisterMetaType<QList<conversation_data>>();
    qRegisterMetaType<message_data>();
    qRegisterMetaType<QList<message_data>>();
    qRegisterMetaType<user_data>();
    qRegisterMetaType<QList<user_data>>();
    qRegisterMetaType<presence_data>();
    qRegisterMetaType<QList<presence_data>>();
    client_->set_connected_handler([this] { emit connected(); });
    client_->set_disconnected_handler([this] { emit disconnected(); });
    client_->set_error_handler([this](chat::error const& value) { emit error(from_utf8(value.message)); });
    client_->set_message_handler([this](chat::message message) { emit message_received(to_message_data(message)); });
    client_->set_read_handler([this](std::int64_t user, std::int64_t message) { emit messages_read(user, message); });
    client_->set_presence_handler([this](chat::presence value) { emit presence_changed(to_presence_data(value)); });
}

client_bridge::~client_bridge() { client_.reset(); }

void client_bridge::connect_to_server(QString const& url) { client_->connect(to_utf8(url)); }

void client_bridge::close() { client_->close(); }

void client_bridge::authenticate(QString const& username, QString const& password)
{
    client_->authenticate(to_utf8(username), to_utf8(password), [this](std::expected<bool, chat::error> result) {
        if (!result)
        {
            emit authentication_finished(false, from_utf8(result.error().message), result.error().kind == chat::error_kind::transport);
            return;
        }
        emit authentication_finished(*result, {}, false);
    });
}

void client_bridge::register_user(QString const& username, QString const& password)
{
    client_->register_user(to_utf8(username), to_utf8(password), [this](std::expected<std::int64_t, chat::error> result) {
        if (!result)
        {
            emit registration_finished(0, from_utf8(result.error().message));
            return;
        }
        emit registration_finished(*result, {});
    });
}

void client_bridge::get_conversations()
{
    client_->get_conversations({}, [this](std::expected<std::vector<chat::conversation>, chat::error> result) {
        if (!result)
        {
            emit conversations_received({}, from_utf8(result.error().message));
            return;
        }

        QList<conversation_data> conversations;
        conversations.reserve(static_cast<qsizetype>(result->size()));
        for (auto const& item : *result)
        {
            conversation_data value;
            value.user = item.user;
            value.username = from_utf8(item.username);
            value.last_id = item.last.id;
            value.last_from = item.last.from;
            value.last_timestamp = item.last.timestamp;
            value.last_text = from_utf8(item.last.text);
            value.unread = item.unread;
            conversations.push_back(std::move(value));
        }
        emit conversations_received(std::move(conversations), {});
    });
}

void client_bridge::get_contacts()
{
    client_->get_contacts([this](std::expected<std::vector<chat::user>, chat::error> result) {
        if (!result)
        {
            emit contacts_received({}, from_utf8(result.error().message));
            return;
        }

        QList<user_data> contacts;
        contacts.reserve(static_cast<qsizetype>(result->size()));
        for (auto const& item : *result)
        {
            user_data value;
            value.id = item.id;
            value.username = from_utf8(item.username);
            contacts.push_back(std::move(value));
        }
        emit contacts_received(std::move(contacts), {});
    });
}

void client_bridge::get_presence()
{
    client_->get_presence([this](std::expected<std::vector<chat::presence>, chat::error> result) {
        if (!result)
        {
            emit presences_received({}, from_utf8(result.error().message));
            return;
        }

        QList<presence_data> users;
        users.reserve(static_cast<qsizetype>(result->size()));
        for (auto const& item : *result)
        {
            users.push_back(to_presence_data(item));
        }
        emit presences_received(std::move(users), {});
    });
}

void client_bridge::get_messages(qint64 user, std::optional<qint64> before)
{
    auto request_before = before ? std::optional<std::int64_t>(*before) : std::nullopt;
    client_->get_messages(user, request_before, [this, user, older = before.has_value()](std::expected<chat::messages_result, chat::error> result) {
        if (!result)
        {
            emit messages_received(user, {}, 0, older, from_utf8(result.error().message));
            return;
        }

        QList<message_data> messages;
        messages.reserve(static_cast<qsizetype>(result->messages.size()));
        for (auto const& item : result->messages)
        {
            messages.push_back(to_message_data(item));
        }
        emit messages_received(user, std::move(messages), result->read_message, older, {});
    });
}

void client_bridge::send_message(qint64 user, QString text)
{
    auto request_text = to_utf8(text);
    client_->send_message(user, std::move(request_text), [this, user, text = std::move(text)](std::expected<chat::send_message_result, chat::error> result) mutable {
        if (!result)
        {
            emit message_sent(user, std::move(text), 0, 0, false, from_utf8(result.error().message));
            return;
        }
        emit message_sent(user, std::move(text), result->message_id, result->timestamp, result->realtime, {});
    });
}

void client_bridge::search_users(QString query)
{
    client_->search_users(to_utf8(query), [this](std::expected<std::vector<chat::user>, chat::error> result) {
        if (!result)
        {
            emit users_received({}, from_utf8(result.error().message));
            return;
        }

        QList<user_data> users;
        users.reserve(static_cast<qsizetype>(result->size()));
        for (auto const& item : *result)
        {
            user_data value;
            value.id = item.id;
            value.username = from_utf8(item.username);
            users.push_back(std::move(value));
        }
        emit users_received(std::move(users), {});
    });
}

void client_bridge::add_contact(qint64 user)
{
    client_->add_contact(user, [this](std::expected<chat::user, chat::error> result) {
        if (!result)
        {
            emit contact_added({}, from_utf8(result.error().message));
            return;
        }

        user_data value;
        value.id = result->id;
        value.username = from_utf8(result->username);
        emit contact_added(std::move(value), {});
    });
}

void client_bridge::mark_read(qint64 user, qint64 message)
{
    client_->mark_read(user, message, [this, user](std::expected<std::int64_t, chat::error> result) {
        if (!result)
        {
            emit read_marked(user, 0, from_utf8(result.error().message));
            return;
        }
        emit read_marked(user, *result, {});
    });
}
