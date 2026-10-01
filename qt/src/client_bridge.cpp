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
    message.conversation = value.conversation;
    message.from = value.from;
    message.username = from_utf8(value.username);
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
    qRegisterMetaType<conversation_data>();
    qRegisterMetaType<read_positions>();
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
    client_->set_read_handler([this](std::int64_t conversation, std::int64_t user, std::int64_t message)
                              { emit messages_read(conversation, user, message); });
    client_->set_conversation_handler([this](std::int64_t) { emit conversation_changed(); });
    client_->set_presence_handler([this](chat::presence value) { emit presence_changed(to_presence_data(value)); });
}

client_bridge::~client_bridge() { client_.reset(); }

void client_bridge::connect_to_server(QString const& url)
{
    ++messages_generation_;
    ++conversations_generation_;
    client_->connect(to_utf8(url));
}

void client_bridge::close()
{
    ++messages_generation_;
    ++conversations_generation_;
    client_->close();
}

void client_bridge::authenticate(QString const& username, QString const& password)
{
    client_->authenticate(to_utf8(username), to_utf8(password),
                          [this](std::expected<chat::authentication_result, chat::error> result)
                          {
        if (!result)
        {
                                  emit authentication_finished(false, 0, from_utf8(result.error().message),
                                                               result.error().kind == chat::error_kind::transport);
            return;
        }
                              emit authentication_finished(result->authenticated, result->user, {}, false);
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
    get_conversations_page({}, {}, ++conversations_generation_);
}

void client_bridge::get_conversations_page(std::optional<chat::conversation_cursor> before,
                                           QList<conversation_data> conversations, std::uint64_t generation)
{
    client_->get_conversations(
        before,
        [this, generation, conversations = std::move(conversations)](
            std::expected<chat::conversations_result, chat::error> result) mutable
        {
            QMetaObject::invokeMethod(
                this,
                [this, generation, conversations = std::move(conversations), result = std::move(result)]() mutable
                {
                    if (generation != conversations_generation_)
                    {
                        return;
                    }
        if (!result)
        {
            emit conversations_received({}, from_utf8(result.error().message));
            return;
        }
                    for (auto const& item : result->conversations)
        {
            conversation_data value;
                        value.id = item.id;
                        value.group = item.kind == chat::conversation_kind::group;
            value.user = item.user;
            value.username = from_utf8(item.username);
                        value.member_count = item.member_count;
            value.last_id = item.last.id;
            value.last_from = item.last.from;
            value.last_timestamp = item.last.timestamp;
            value.last_text = from_utf8(item.last.text);
            value.unread = item.unread;
                        bool found = false;
                        for (auto const& existing : conversations)
                        {
                            if (existing.id == value.id)
                            {
                                found = true;
                                break;
                            }
                        }
                        if (!found)
                        {
            conversations.push_back(std::move(value));
        }
                    }
                    if (result->next)
                    {
                        get_conversations_page(result->next, std::move(conversations), generation);
                    }
                    else
                    {
        emit conversations_received(std::move(conversations), {});
                    }
                },
                Qt::QueuedConnection);
        });
}

void client_bridge::open_direct_conversation(qint64 user, QString username)
{
    client_->open_direct_conversation(user,
                                      [this, user, username = std::move(username)](auto result)
                                      {
                                          conversation_data value;
                                          if (result)
                                          {
                                              value.id = *result;
                                              value.user = user;
                                              value.username = username;
                                          }
                                          emit conversation_opened(value, result ? QString{}
                                                                                 : from_utf8(result.error().message));
                                      });
}

void client_bridge::create_group(QString title, QList<qint64> members)
{
    std::vector<std::int64_t> values(members.begin(), members.end());
    auto request_title = to_utf8(title);
    client_->create_group(std::move(request_title), std::move(values),
                          [this, title = std::move(title), count = members.size() + 1](auto result)
                          {
                              conversation_data value;
                              if (result)
                              {
                                  value.id = *result;
                                  value.group = true;
                                  value.username = title;
                                  value.member_count = count;
                              }
                              emit conversation_opened(value, result ? QString{} : from_utf8(result.error().message));
                          });
}

void client_bridge::get_members(qint64 conversation)
{
    client_->get_members(conversation,
                         [this, conversation](auto result)
                         {
                             QList<user_data> values;
                             if (result)
                             {
                                 for (auto const& user : *result)
                                 {
                                     values.push_back(user_data{user.id, from_utf8(user.username)});
                                 }
                             }
                             emit members_received(conversation, std::move(values),
                                                   result ? QString{} : from_utf8(result.error().message));
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

void client_bridge::get_messages(qint64 conversation, std::optional<qint64> before, std::optional<qint64> after)
{
    auto const generation = ++messages_generation_;
    client_->get_messages(
        conversation, before,
        [this, conversation, before, after,
         generation](std::expected<chat::messages_result, chat::error> result) mutable
        {
            QMetaObject::invokeMethod(
                this,
                [this, conversation, before, after, generation, result = std::move(result)]() mutable
                {
                    if (generation != messages_generation_)
                    {
                        return;
                    }
        if (!result)
        {
                        emit messages_received(conversation, {}, {}, before.has_value(), after.has_value(), false,
                                               from_utf8(result.error().message));
            return;
        }
        QList<message_data> messages;
                    for (auto const& value : result->messages)
        {
                        messages.push_back(to_message_data(value));
        }
                    read_positions positions;
                    for (auto const& position : result->read_positions)
                    {
                        positions.insert(position.user, position.message);
                    }
                    auto const next = result->messages.empty() ? 0 : result->messages.back().id;
                    emit messages_received(conversation, std::move(messages), std::move(positions), before.has_value(),
                                           after.has_value(), result->has_more, {});
                    if (after && result->has_more && next > *after)
                    {
                        get_messages(conversation, {}, next);
                    }
                },
                Qt::QueuedConnection);
        },
        after);
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
