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
#include <chat/error_text.hpp>

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

// Server and protocol errors are English identifiers; users see them in Chinese.
QString display_error(chat::error const& value) { return from_utf8(chat::error_text(value)); }

quoted_message_data to_reply_data(std::optional<chat::quoted_message> const& reply)
{
    return reply ? quoted_message_data{reply->id, from_utf8(reply->username), from_utf8(reply->text),
                                       reply->edited_at.value_or(0), reply->deleted}
                 : quoted_message_data{};
}

QList<reaction_data> to_reactions(std::vector<chat::reaction> const& values)
{
    QList<reaction_data> result;
    for (auto const& value : values)
    {
        reaction_data item{from_utf8(value.emoji), {}};
        for (auto user : value.users) { item.users.push_back(user); }
        result.push_back(std::move(item));
    }
    return result;
}

QList<mention_data> to_mentions(std::vector<chat::mention> const& values)
{
    QList<mention_data> result;
    for (auto const& value : values) { result.push_back({value.user, from_utf8(value.username)}); }
    return result;
}

message_data to_message_data(chat::message const& value)
{
    message_data message;
    message.id = value.id;
    message.conversation = value.conversation;
    message.from = value.from;
    message.username = from_utf8(value.username);
    message.avatar = value.avatar;
    message.reaction_revision = value.reaction_revision;
    message.reactions = to_reactions(value.reactions);
    message.mentions = to_mentions(value.mentions);
    message.timestamp = value.timestamp;
    message.text = from_utf8(value.text);
    message.edited_at = value.edited_at.value_or(0);
    message.deleted = value.deleted;
    message.reply = to_reply_data(value.reply);
    if (value.attachment)
    {
        message.attachment = attachment_data{from_utf8(value.attachment->filename), from_utf8(value.attachment->media_type),
                                             value.attachment->size};
    }
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
    qRegisterMetaType<chat::avatar_state>();
    qRegisterMetaType<chat::group_join_request_state>();
    qRegisterMetaType<QList<conversation_data>>();
    qRegisterMetaType<conversation_data>();
    qRegisterMetaType<read_positions>();
    qRegisterMetaType<message_data>();
    qRegisterMetaType<QList<message_data>>();
    qRegisterMetaType<QList<reaction_data>>();
    qRegisterMetaType<QList<mention_data>>();
    qRegisterMetaType<QList<member_data>>();
    qRegisterMetaType<user_data>();
    qRegisterMetaType<QList<user_data>>();
    qRegisterMetaType<presence_data>();
    qRegisterMetaType<QList<presence_data>>();
    client_->set_avatar_handler([this](std::int64_t user, chat::avatar_state state) {
        auto const generation = connection_generation_.load();
        QMetaObject::invokeMethod(this, [this, generation, user, state] {
            if (generation == connection_generation_) { emit avatar_changed(user, state); }
        }, Qt::QueuedConnection);
    });
    client_->set_connected_handler([this] { emit connected(); });
    client_->set_disconnected_handler([this] { ++connection_generation_; emit disconnected(); });
    client_->set_error_handler([this](chat::error const& value) { emit error(display_error(value)); });
    client_->set_message_handler([this](chat::message message) {
        auto const generation = connection_generation_.load();
        QMetaObject::invokeMethod(this, [this, generation, message = std::move(message)] {
            if (generation == connection_generation_) { emit message_received(to_message_data(message)); }
        }, Qt::QueuedConnection);
    });
    client_->set_message_updated_handler([this](chat::message value) {
        auto const generation = connection_generation_.load();
        QMetaObject::invokeMethod(this, [this, generation, value = std::move(value)] {
            if (generation == connection_generation_) { emit message_updated(value.conversation, to_message_data(value), {}); }
        }, Qt::QueuedConnection);
    });
    client_->set_reaction_handler([this](chat::reaction_update value) {
        auto const generation = connection_generation_.load();
        QMetaObject::invokeMethod(this, [this, generation, value = std::move(value)] {
            if (generation == connection_generation_)
            {
                emit reaction_changed(value.conversation, value.message, value.revision, to_reactions(value.reactions), {});
            }
        }, Qt::QueuedConnection);
    });
    client_->set_read_handler([this](std::int64_t conversation, std::int64_t user, std::int64_t message)
    {
        post_result(connection_generation_.load(), [this, conversation, user, message] {
            emit messages_read(conversation, user, message);
        });
    });
    client_->set_typing_handler([this](chat::typing_event value) {
        post_result(connection_generation_.load(), [this, value = std::move(value)] {
            emit typing_changed(value.conversation, value.user, from_utf8(value.username), value.typing);
        });
    });
    client_->set_conversation_handler([this](std::int64_t id, bool removed) {
        post_result(connection_generation_.load(), [this, id, removed] { emit conversation_changed(id, removed); });
    });
    client_->set_friendship_handler([this](std::int64_t user) {
        post_result(connection_generation_.load(), [this, user] { emit friendship_changed(user); });
    });
    client_->set_presence_handler([this](chat::presence value) {
        post_result(connection_generation_.load(), [this, value] { emit presence_changed(to_presence_data(value)); });
    });
    client_->set_group_join_request_handler([this](chat::group_join_request_event value) {
        auto const generation = connection_generation_.load();
        QMetaObject::invokeMethod(this, [this, generation, value] {
            if (generation == connection_generation_) { emit group_join_request_changed(value.conversation, value.user, value.state); }
        }, Qt::QueuedConnection);
    });
}

client_bridge::~client_bridge() { client_.reset(); }

void client_bridge::connect_to_server(QString const& url)
{
    ++messages_generation_;
    ++conversations_generation_;
    ++search_generation_;
    ++upload_generation_;
    ++download_generation_;
    ++connection_generation_;
    client_->connect(to_utf8(url));
}

void client_bridge::close()
{
    ++messages_generation_;
    ++conversations_generation_;
    ++search_generation_;
    ++upload_generation_;
    ++download_generation_;
    ++connection_generation_;
    client_->close();
}

void client_bridge::authenticate(QString const& username, QString const& password)
{
    auto const generation = connection_generation_.load();
    client_->authenticate(to_utf8(username), to_utf8(password),
        [this, generation](auto result) {
        post_result(generation, [this, result = std::move(result)] {
            if (!result)
            {
                emit authentication_finished(false, 0, display_error(result.error()),
                    result.error().kind == chat::error_kind::transport, {});
                return;
            }
            emit authentication_finished(result->authenticated, result->user, {}, false, result->avatar);
        });
    });
}

void client_bridge::register_user(QString const& username, QString const& password)
{
    auto const generation = connection_generation_.load();
    client_->register_user(to_utf8(username), to_utf8(password), [this, generation](auto result) {
        post_result(generation, [this, result = std::move(result)] {
            emit registration_finished(result ? *result : 0, result ? QString{} : display_error(result.error()));
        });
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
            emit conversations_received({}, display_error(result.error()));
            return;
        }
                    for (auto const& item : result->conversations)
        {
            conversation_data value;
                        value.id = item.id;
                        value.group = item.kind == chat::conversation_kind::group;
            value.user = item.user;
            value.username = from_utf8(item.username);
            value.avatar = item.avatar;
                        value.member_count = item.member_count;
            value.last_id = item.last.id;
            value.last_from = item.last.from;
            value.last_timestamp = item.last.timestamp;
            value.last_text = item.last.deleted ? QStringLiteral("消息已删除") : from_utf8(item.last.text);
            value.unread = item.unread;
            value.muted = item.muted;
            value.pinned = item.pinned;
            value.pinned_message = to_reply_data(item.pinned_message);
            value.announcement = from_utf8(item.announcement);
            value.join_approval = item.join_approval;
            value.can_send = item.can_send;
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

void client_bridge::set_conversation_muted(qint64 conversation, bool muted)
{
    auto const generation = connection_generation_.load();
    client_->set_conversation_muted(conversation, muted, [this, conversation, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            if (result) { ++conversations_generation_; }
            emit mute_finished(conversation, result ? *result : false, result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::set_conversation_pinned(qint64 conversation, bool pinned)
{
    auto const generation = connection_generation_.load();
    client_->set_conversation_pinned(conversation, pinned, [this, conversation, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            if (result) { ++conversations_generation_; }
            emit pin_finished(conversation, result ? *result : false, result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::set_group_pinned_message(qint64 conversation, std::optional<qint64> message)
{
    auto const generation = connection_generation_.load();
    auto finished = [this, conversation, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            if (result) { ++conversations_generation_; }
            emit group_pin_finished(conversation, result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    };
    if (message) { client_->pin_group_message(conversation, *message, std::move(finished)); }
    else { client_->unpin_group_message(conversation, std::move(finished)); }
}

void client_bridge::open_direct_conversation(qint64 user, QString username)
{
    auto const generation = connection_generation_.load();
    client_->open_direct_conversation(user, [this, user, username = std::move(username), generation](auto result) {
        post_result(generation, [this, user, username, result = std::move(result)] {
            conversation_data value;
            if (result)
            {
                value.id = result->conversation;
                value.can_send = result->can_send;
                value.user = user;
                value.username = username;
            }
            emit conversation_opened(value, result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::create_group(QString title, QList<qint64> members)
{
    std::vector<std::int64_t> values(members.begin(), members.end());
    auto request_title = to_utf8(title);
    auto const generation = connection_generation_.load();
    client_->create_group(std::move(request_title), std::move(values),
        [this, title = std::move(title), count = members.size() + 1, generation](auto result) {
        post_result(generation, [this, title, count, result = std::move(result)] {
            conversation_data value;
            if (result)
            {
                value.id = *result;
                value.group = true;
                value.can_send = true;
                value.username = title;
                value.member_count = count;
            }
            emit conversation_opened(value, result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::join_group(QString token)
{
    auto const generation = connection_generation_.load();
    client_->join_group(to_utf8(token), [this, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            conversation_data value;
            if (result && result->state == chat::group_join_state::pending)
            {
                emit group_join_pending(from_utf8(result->title));
                return;
            }
            if (result)
            {
                ++conversations_generation_;
                value.id = result->conversation;
                value.group = true;
                value.username = from_utf8(result->title);
                value.can_send = true;
                value.member_count = result->member_count;
            }
            emit conversation_opened(value, result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::group_invite(qint64 conversation, std::optional<bool> create)
{
    auto const generation = connection_generation_.load();
    auto finished = [this, conversation, create, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, conversation, create, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            auto const error = result ? QString{} : display_error(result.error());
            if (create.has_value())
            {
                if (result) { ++conversations_generation_; }
                emit group_action_finished(conversation, false, error);
            }
            emit group_invite_received(conversation, result && *result ? from_utf8(**result) : QString{}, error);
        }, Qt::QueuedConnection);
    };
    if (!create) { client_->get_group_invite(conversation, std::move(finished)); }
    else if (*create) { client_->create_group_invite(conversation, std::move(finished)); }
    else { client_->revoke_group_invite(conversation, std::move(finished)); }
}

void client_bridge::set_group_join_approval(qint64 conversation, bool required)
{
    auto const generation = connection_generation_.load();
    client_->set_group_join_approval(conversation, required, [this, conversation, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            if (result) { ++conversations_generation_; }
            emit group_action_finished(conversation, false, result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::get_group_join_requests(qint64 conversation, qint64 before)
{
    auto const generation = connection_generation_.load();
    client_->get_group_join_requests(conversation, before > 0 ? std::optional<std::int64_t>(before) : std::nullopt,
        [this, conversation, before, generation](auto result) {
            QMetaObject::invokeMethod(this, [this, conversation, before, generation, result = std::move(result)] {
                if (generation != connection_generation_) { return; }
                QList<user_data> values;
                if (result)
                {
                    for (auto const& request : result->requests)
                    {
                        auto const& user = request.applicant;
                        values.push_back({user.id, from_utf8(user.username), false, 0, user.avatar});
                    }
                }
                emit group_join_requests_received(conversation, std::move(values), result ? result->next.value_or(0) : 0,
                    before > 0, result ? QString{} : display_error(result.error()));
            }, Qt::QueuedConnection);
        });
}

void client_bridge::respond_group_join_request(qint64 conversation, qint64 user, bool accept)
{
    auto const generation = connection_generation_.load();
    client_->respond_group_join_request(conversation, user, accept, [this, conversation, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            if (result) { ++conversations_generation_; }
            emit group_action_finished(conversation, false, result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::get_members(qint64 conversation)
{
    auto const generation = connection_generation_.load();
    client_->get_members(conversation,
        [this, conversation, generation](std::expected<std::vector<chat::conversation_member>, chat::error> result) {
        QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            if (!result)
            {
                emit members_received(conversation, {}, display_error(result.error()));
                return;
            }
            QList<member_data> values;
            for (auto const& user : *result)
            {
                values.push_back(member_data{user.id, from_utf8(user.username), user.role, user.avatar});
            }
            emit members_received(conversation, std::move(values), {});
        }, Qt::QueuedConnection);
        });
}

void client_bridge::set_group_admin(qint64 conversation, qint64 user, bool admin)
{
    auto const generation = connection_generation_.load();
    client_->set_group_admin(conversation, user, admin, [this, conversation, generation](auto result) {
        post_result(generation, [this, conversation, result = std::move(result)] {
            if (result) { ++conversations_generation_; }
            emit group_action_finished(conversation, false, result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::rename_group(qint64 conversation, QString title)
{
    auto const generation = connection_generation_.load();
    client_->rename_group(conversation, to_utf8(title), [this, conversation, generation](auto result) {
        post_result(generation, [this, conversation, result = std::move(result)] {
            if (result) { ++conversations_generation_; }
            emit group_action_finished(conversation, false, result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::set_group_announcement(qint64 conversation, QString text)
{
    auto const generation = connection_generation_.load();
    client_->set_group_announcement(conversation, to_utf8(text), [this, conversation, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            if (result) { ++conversations_generation_; }
            emit group_action_finished(conversation, false, result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::transfer_group_owner(qint64 conversation, qint64 user)
{
    auto const generation = connection_generation_.load();
    client_->transfer_group_owner(conversation, user, [this, conversation, generation](auto result) {
        post_result(generation, [this, conversation, result = std::move(result)] {
            if (result) { ++conversations_generation_; }
            emit group_action_finished(conversation, false, result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::remove_group_member(qint64 conversation, qint64 user)
{
    auto const generation = connection_generation_.load();
    client_->remove_group_member(conversation, user, [this, conversation, generation](auto result) {
        post_result(generation, [this, conversation, result = std::move(result)] {
            if (result) { ++conversations_generation_; }
            emit group_action_finished(conversation, false, result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::invite_group_members(qint64 conversation, QList<qint64> members)
{
    auto const generation = connection_generation_.load();
    client_->invite_group_members(conversation, std::vector<std::int64_t>(members.begin(), members.end()),
        [this, conversation, generation](auto result) {
            post_result(generation, [this, conversation, result = std::move(result)] {
                if (result) { ++conversations_generation_; }
                emit group_action_finished(conversation, false, result ? QString{} : display_error(result.error()));
            });
        });
}

void client_bridge::leave_group(qint64 conversation)
{
    auto const generation = connection_generation_.load();
    client_->leave_group(conversation, [this, conversation, generation](auto result) {
        post_result(generation, [this, conversation, result = std::move(result)] {
            if (result) { ++conversations_generation_; }
            emit group_action_finished(conversation, result.has_value(), result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::get_contacts()
{
    auto const generation = connection_generation_.load();
    auto const request = ++contacts_generation_;
    client_->get_contacts([this, generation, request](auto result) {
        post_result(generation, [this, request, result = std::move(result)] {
            if (request != contacts_generation_) { return; }
            if (!result)
            {
                emit contacts_received({}, display_error(result.error()));
                return;
            }

            QList<user_data> contacts;
            contacts.reserve(static_cast<qsizetype>(result->size()));
            for (auto const& item : *result)
            {
                user_data value;
                value.id = item.id;
                value.username = from_utf8(item.username);
                value.avatar = item.avatar;
                contacts.push_back(std::move(value));
            }
            emit contacts_received(std::move(contacts), {});
        });
    });
}

void client_bridge::get_presence()
{
    auto const generation = connection_generation_.load();
    client_->get_presence([this, generation](auto result) {
        post_result(generation, [this, result = std::move(result)] {
            if (!result)
            {
                emit presences_received({}, display_error(result.error()));
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
                                               display_error(result.error()));
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

void client_bridge::set_typing(qint64 conversation, bool typing)
{
    client_->set_typing(conversation, typing, [](auto) {});
}

void client_bridge::send_message(qint64 user, QString text, qint64 reply_to)
{
    auto const generation = connection_generation_.load();
    auto request_text = to_utf8(text);
    client_->send_message(user, std::move(request_text),
        [this, user, generation, text = std::move(text)](auto result) mutable {
            QMetaObject::invokeMethod(this, [this, user, generation, text = std::move(text), result = std::move(result)]() mutable {
                if (generation != connection_generation_) { return; }
                emit message_sent(user, std::move(text), result ? result->message_id : 0, result ? result->timestamp : 0,
                    result && result->realtime, result ? to_reply_data(result->reply) : quoted_message_data{},
                    result ? to_mentions(result->mentions) : QList<mention_data>{},
                    result ? QString{} : display_error(result.error()));
            }, Qt::QueuedConnection);
        }, reply_to > 0 ? std::optional<std::int64_t>(reply_to) : std::nullopt);
}

void client_bridge::send_attachment(qint64 conversation, QString filename, QByteArray data, qint64 reply_to)
{
    auto const generation = ++upload_generation_;
    client_->send_attachment(conversation, to_utf8(filename), {data.constData(), static_cast<std::size_t>(data.size())},
        [this, conversation, generation](auto result) mutable {
            QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)]() mutable {
                if (generation != upload_generation_)
                {
                    return;
                }
                emit attachment_sent(conversation, result ? to_message_data(*result) : message_data{},
                                     result ? QString{} : display_error(result.error()));
            }, Qt::QueuedConnection);
        }, reply_to > 0 ? std::optional<std::int64_t>{reply_to} : std::nullopt);
}

void client_bridge::get_attachment(qint64 conversation, qint64 message)
{
    auto const generation = ++download_generation_;
    client_->get_attachment(conversation, message, [this, conversation, message, generation](auto result) mutable {
        QMetaObject::invokeMethod(this, [this, conversation, message, generation, result = std::move(result)]() mutable {
            if (generation != download_generation_)
            {
                return;
            }
            emit attachment_received(conversation, message,
                result ? QByteArray(result->data(), static_cast<qsizetype>(result->size())) : QByteArray{},
                result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::delete_message(qint64 conversation, qint64 message)
{
    auto const generation = connection_generation_.load();
    client_->delete_message(conversation, message, [this, conversation, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            emit message_updated(conversation, result ? to_message_data(*result) : message_data{},
                                 result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::get_message_image(qint64 conversation, qint64 message)
{
    auto const generation = connection_generation_.load();
    client_->get_attachment(conversation, message, [this, conversation, message, generation](auto result) mutable {
        QMetaObject::invokeMethod(this, [this, conversation, message, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            emit message_image_received(conversation, message,
                result ? QByteArray(result->data(), static_cast<qsizetype>(result->size())) : QByteArray{},
                result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::edit_message(qint64 conversation, qint64 message, QString text)
{
    auto const generation = connection_generation_.load();
    client_->edit_message(conversation, message, to_utf8(text), [this, conversation, generation](auto result) {
        QMetaObject::invokeMethod(this, [this, conversation, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            emit message_updated(conversation, result ? to_message_data(*result) : message_data{},
                                 result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::set_message_reaction(qint64 conversation, qint64 message, QString emoji)
{
    auto const generation = connection_generation_.load();
    client_->set_message_reaction(conversation, message, to_utf8(emoji),
        [this, conversation, message, generation](auto result) mutable {
            QMetaObject::invokeMethod(this, [this, conversation, message, generation, result = std::move(result)]() mutable {
                if (generation != connection_generation_) { return; }
                emit reaction_changed(conversation, message, result ? result->revision : 0,
                    result ? to_reactions(result->reactions) : QList<reaction_data>{},
                    result ? QString{} : display_error(result.error()));
            }, Qt::QueuedConnection);
        });
}

void client_bridge::search_users(QString query)
{
    auto const generation = connection_generation_.load();
    client_->search_users(to_utf8(query), [this, generation](auto result) {
        post_result(generation, [this, result = std::move(result)] {
            if (!result)
            {
                emit users_received({}, display_error(result.error()));
                return;
            }

            QList<user_data> users;
            users.reserve(static_cast<qsizetype>(result->size()));
            for (auto const& item : *result)
            {
                user_data value;
                value.id = item.id;
                value.username = from_utf8(item.username);
                value.avatar = item.avatar;
                users.push_back(std::move(value));
            }
            emit users_received(std::move(users), {});
        });
    });
}

void client_bridge::search_messages(qint64 conversation, QString query, qint64 before)
{
    auto request_query = to_utf8(query);
    auto const generation = ++search_generation_;
    client_->search_messages(conversation, std::move(request_query),
        before > 0 ? std::optional<std::int64_t>{before} : std::nullopt,
        [this, conversation, query = std::move(query), before, generation](auto result) mutable {
            QMetaObject::invokeMethod(this, [this, conversation, query = std::move(query), before, generation,
                                            result = std::move(result)]() mutable {
                if (generation != search_generation_)
                {
                    return;
                }
                if (!result)
                {
                    emit message_search_received(conversation, std::move(query), before, {}, {}, false,
                                                 display_error(result.error()));
                    return;
                }
                QList<message_data> messages;
                for (auto const& value : result->messages)
                {
                    messages.push_back(to_message_data(value));
                }
                read_positions positions;
                for (auto const& value : result->read_positions)
                {
                    positions.insert(value.user, value.message);
                }
                emit message_search_received(conversation, std::move(query), before, std::move(messages),
                                             std::move(positions), result->has_more, {});
            }, Qt::QueuedConnection);
        });
}

void client_bridge::get_history(qint64 conversation, qint64 before)
{
    auto const generation = ++history_generation_;
    client_->get_messages(conversation, before > 0 ? std::optional<std::int64_t>{before} : std::nullopt,
        [this, conversation, before, generation](std::expected<chat::messages_result, chat::error> result) mutable {
            QMetaObject::invokeMethod(this, [this, conversation, before, generation, result = std::move(result)]() mutable {
                if (generation != history_generation_) { return; }
                if (!result)
                {
                    emit history_received(conversation, before, {}, false, display_error(result.error()));
                    return;
                }
                QList<message_data> messages;
                for (auto const& value : result->messages) { messages.push_back(to_message_data(value)); }
                emit history_received(conversation, before, std::move(messages), result->has_more, {});
            }, Qt::QueuedConnection);
        });
}

void client_bridge::add_contact(qint64 user)
{
    auto const generation = connection_generation_.load();
    client_->send_friend_request(user, [this, generation, user](auto result) {
        post_result(generation, [this, user, result = std::move(result)] {
            if (!result)
            {
                user_data value;
                value.id = user;
                emit contact_added(value, display_error(result.error()));
                return;
            }

            user_data value;
            value.id = result->user.id;
            value.username = from_utf8(result->user.username);
            value.avatar = result->user.avatar;
            emit contact_added(std::move(value), {});
        });
    });
}

void client_bridge::get_friend_requests()
{
    auto const generation = connection_generation_.load();
    auto const request = ++friend_requests_generation_;
    client_->get_friend_requests([this, generation, request](auto result) {
        post_result(generation, [this, request, result = std::move(result)] {
            if (request != friend_requests_generation_) { return; }
            QList<user_data> incoming, outgoing;
            if (!result) { emit friend_requests_received({}, {}, display_error(result.error())); return; }
            for (auto const& request : result->incoming)
            {
                user_data user; user.id = request.user.id; user.username = from_utf8(request.user.username);
                user.avatar = request.user.avatar; incoming.push_back(std::move(user));
            }
            for (auto const& request : result->outgoing)
            {
                user_data user; user.id = request.user.id; user.username = from_utf8(request.user.username);
                user.avatar = request.user.avatar; outgoing.push_back(std::move(user));
            }
            emit friend_requests_received(std::move(incoming), std::move(outgoing), {});
        });
    });
}

void client_bridge::respond_friend_request(qint64 user, bool accept)
{
    auto const generation = connection_generation_.load();
    client_->respond_friend_request(user, accept, [this, generation, user](auto result) {
        post_result(generation, [this, user, result = std::move(result)] {
            user_data value; value.id = user;
            emit contact_added(value, result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::cancel_friend_request(qint64 user)
{
    auto const generation = connection_generation_.load();
    client_->cancel_friend_request(user, [this, generation, user](auto result) {
        post_result(generation, [this, user, result = std::move(result)] {
            user_data value; value.id = user;
            emit contact_added(value, result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::remove_contact(qint64 user)
{
    auto const generation = connection_generation_.load();
    client_->remove_contact(user, [this, generation](auto result) {
        post_result(generation, [this, result = std::move(result)] {
            emit contact_removed(result ? QString{} : display_error(result.error()));
        });
    });
}

void client_bridge::mark_read(qint64 user, qint64 message)
{
    auto const generation = connection_generation_.load();
    client_->mark_read(user, message, [this, user, generation](std::expected<std::int64_t, chat::error> result) {
        QMetaObject::invokeMethod(this, [this, user, generation, result = std::move(result)] {
            if (generation != connection_generation_) { return; }
            emit read_marked(user, result ? *result : 0, result ? QString{} : display_error(result.error()));
        }, Qt::QueuedConnection);
    });
}

void client_bridge::get_avatar(qint64 user, qint64 revision)
{
    auto const generation = connection_generation_.load();
    client_->get_avatar(user,
                        revision,
                        [this, generation, user, revision](auto result)
                        {
                            QMetaObject::invokeMethod(
                                this,
                                [this, generation, user, revision, result = std::move(result)]
                                {
                                    if (generation != connection_generation_)
                                    {
                                        return;
                                    }
                                    if (!result)
                                    {
                                        emit avatar_received(user, {revision, true}, {});
                                        return;
                                    }
                                    emit avatar_received(user, result->state, QByteArray(result->data.data(), result->data.size()));
                                },
                                Qt::QueuedConnection);
                        });
}

void client_bridge::set_avatar(QByteArray data)
{
    auto const generation = connection_generation_.load();
    client_->set_avatar(std::string(data.constData(), data.size()),
                        [this, generation](auto result)
                        {
                            QMetaObject::invokeMethod(
                                this,
                                [this, generation, result = std::move(result)]
                                {
                                    if (generation != connection_generation_)
                                    {
                                        return;
                                    }
                                    emit avatar_update_finished(result ? QString{} : display_error(result.error()));
                                },
                                Qt::QueuedConnection);
                        });
}

void client_bridge::clear_avatar()
{
    auto const generation = connection_generation_.load();
    client_->clear_avatar(
        [this, generation](auto result)
        {
            QMetaObject::invokeMethod(
                this,
                [this, generation, result = std::move(result)]
                {
                    if (generation != connection_generation_)
                    {
                        return;
                    }
                    emit avatar_update_finished(result ? QString{} : display_error(result.error()));
                },
                Qt::QueuedConnection);
        });
}
