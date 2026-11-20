#include "client_bridge.hpp"
#include "clipboard_image.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QMetaType>
#include <QTemporaryDir>
#include <QFile>
#include <QStandardPaths>

#include <chat/client.hpp>
#include <chat/error_text.hpp>

namespace
{

std::string to_utf8(QString const& value)
{
    auto const bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

QString from_utf8(std::string const& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

// Server and protocol errors are English identifiers; users see them in Chinese.
QString display_error(chat::error const& value) { return from_utf8(chat::error_text(value)); }

quoted_message_data to_reply_data(std::optional<chat::quoted_message> const& reply)
{
    return reply ? quoted_message_data{reply->id, from_utf8(reply->username), from_utf8(reply->text), reply->edited_at.value_or(0), reply->deleted}
                 : quoted_message_data{};
}

QList<reaction_data> to_reactions(std::vector<chat::reaction> const& values)
{
    QList<reaction_data> result;
    for (auto const& value : values)
    {
        reaction_data item{from_utf8(value.emoji), {}};
        for (auto user : value.users)
        {
            item.users.push_back(user);
        }
        result.push_back(std::move(item));
    }
    return result;
}

QList<mention_data> to_mentions(std::vector<chat::mention> const& values)
{
    QList<mention_data> result;
    for (auto const& value : values)
    {
        result.push_back({value.user, from_utf8(value.username)});
    }
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
    message.client_id = value.client_id.value_or(0);
    if (value.attachment)
    {
        message.attachment = attachment_data{from_utf8(value.attachment->filename),
                                             from_utf8(value.attachment->media_type),
                                             value.attachment->size,
                                             from_utf8(value.attachment->object_key)};
    }
    return message;
}

user_data to_user_data(chat::user const& user) { return {user.id, from_utf8(user.username), false, 0, user.avatar}; }

presence_data to_presence_data(chat::presence const& value)
{
    presence_data presence;
    presence.user = value.user;
    presence.online = value.online;
    presence.last_seen = value.last_seen;
    return presence;
}

}    // namespace

client_bridge::client_bridge(QObject* parent)
    : QObject(parent), client_(std::make_unique<chat::client>(to_utf8(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))))
{
    qRegisterMetaType<chat::avatar_state>();
    qRegisterMetaType<chat::group_join_request_state>();
    qRegisterMetaType<QList<conversation_data>>();
    qRegisterMetaType<conversation_data>();
    qRegisterMetaType<read_positions>();
    qRegisterMetaType<message_data>();
    qRegisterMetaType<send_status>();
    qRegisterMetaType<QList<message_data>>();
    qRegisterMetaType<QList<reaction_data>>();
    qRegisterMetaType<QList<mention_data>>();
    qRegisterMetaType<QList<member_data>>();
    qRegisterMetaType<QList<user_data>>();
    qRegisterMetaType<presence_data>();
    qRegisterMetaType<QList<presence_data>>();
    client_->set_avatar_handler([this](std::int64_t user, chat::avatar_state state)
                                { post_result(connection_generation_.load(), [this, user, state] { emit avatar_changed(user, state); }); });
    client_->set_connected_handler([this] { emit connected(); });
    client_->set_disconnected_handler(
        [this]
        {
            ++connection_generation_;
            emit disconnected();
        });
    client_->set_error_handler([this](chat::error const& value) { emit error(display_error(value)); });
    client_->set_outbox_handler([this](chat::pending_text item)
                                { post_result(connection_generation_.load(), [this, item = std::move(item)] { publish_pending_text(item); }); });
    client_->set_message_handler(
        [this](chat::message message)
        { post_result(connection_generation_.load(), [this, message = std::move(message)] { emit message_received(to_message_data(message)); }); });
    client_->set_message_updated_handler(
        [this](chat::message value)
        {
            post_result(connection_generation_.load(),
                        [this, value = std::move(value)] { emit message_updated(value.conversation, to_message_data(value), {}); });
        });
    client_->set_reaction_handler(
        [this](chat::reaction_update value)
        {
            post_result(connection_generation_.load(),
                        [this, value = std::move(value)]
                        { emit reaction_changed(value.conversation, value.message, value.revision, to_reactions(value.reactions), {}); });
        });
    client_->set_read_handler(
        [this](std::int64_t conversation, std::int64_t user, std::int64_t message)
        { post_result(connection_generation_.load(), [this, conversation, user, message] { emit messages_read(conversation, user, message); }); });
    client_->set_typing_handler(
        [this](chat::typing_event value)
        {
            post_result(connection_generation_.load(),
                        [this, value = std::move(value)]
                        { emit typing_changed(value.conversation, value.user, from_utf8(value.username), value.typing); });
        });
    client_->set_conversation_handler(
        [this](std::int64_t id, bool removed)
        { post_result(connection_generation_.load(), [this, id, removed] { emit conversation_changed(id, removed); }); });
    client_->set_friendship_handler([this](std::int64_t) { post_result(connection_generation_.load(), [this] { emit friendship_changed(); }); });
    client_->set_presence_handler([this](chat::presence value)
                                  { post_result(connection_generation_.load(), [this, value] { emit presence_changed(to_presence_data(value)); }); });
    client_->set_group_join_request_handler(
        [this](chat::group_join_request_event value)
        {
            post_result(connection_generation_.load(),
                        [this, value] { emit group_join_request_changed(value.conversation, value.user, value.state); });
        });
}

client_bridge::~client_bridge() { client_.reset(); }

void client_bridge::connect_to_server(QString const& url)
{
    ++connection_generation_;
    client_->connect(to_utf8(url));
}

void client_bridge::close()
{
    ++connection_generation_;
    client_->close();
}

void client_bridge::authenticate(QString const& username, QString const& password)
{
    auto const generation = connection_generation_.load();
    client_->authenticate(to_utf8(username),
                          to_utf8(password),
                          [this, generation](auto result)
                          {
                              post_result(
                                  generation,
                                  [this, result = std::move(result)]
                                  {
                                      if (!result)
                                      {
                                          emit authentication_finished(
                                              false, 0, display_error(result.error()), result.error().kind == chat::error_kind::transport, {});
                                          return;
                                      }
                                      emit authentication_finished(result->authenticated, result->user, {}, false, result->avatar);
                                  });
                          });
}

void client_bridge::register_user(QString const& username, QString const& password)
{
    auto const generation = connection_generation_.load();
    client_->register_user(to_utf8(username),
                           to_utf8(password),
                           [this, generation](auto result)
                           {
                               post_result(generation,
                                           [this, result = std::move(result)]
                                           { emit registration_finished(result ? QString{} : display_error(result.error())); });
                           });
}

void client_bridge::get_conversations() { get_conversations_page({}, {}, ++conversations_generation_, connection_generation_.load()); }

void client_bridge::get_conversations_page(std::optional<chat::conversation_cursor> before,
                                           QList<conversation_data> conversations,
                                           std::uint64_t generation,
                                           std::uint64_t connection)
{
    client_->get_conversations(before,
                               [this, generation, connection, conversations = std::move(conversations)](auto result) mutable
                               {
                                   post_result(
                                       connection,
                                       [this, generation, connection, conversations = std::move(conversations), result = std::move(result)]() mutable
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
                                               value.last_timestamp = item.last.timestamp;
                                               value.last_text = item.last.deleted ? QStringLiteral("消息已删除") : from_utf8(item.last.text);
                                               value.unread = item.unread;
                                               value.muted = item.muted;
                                               value.pinned = item.pinned;
                                               value.pinned_message = to_reply_data(item.pinned_message);
                                               value.announcement = from_utf8(item.announcement);
                                               value.join_approval = item.join_approval;
                                               if (std::ranges::find(conversations, value.id, &conversation_data::id) == conversations.end())
                                               {
                                                   conversations.push_back(std::move(value));
                                               }
                                           }
                                           if (result->next)
                                           {
                                               if (connection == connection_generation_)
                                               {
                                                   get_conversations_page(result->next, std::move(conversations), generation, connection);
                                               }
                                           }
                                           else
                                           {
                                               emit conversations_received(std::move(conversations), {});
                                           }
                                       });
                               });
}

void client_bridge::set_conversation_muted(qint64 conversation, bool muted)
{
    auto const generation = connection_generation_.load();
    client_->set_conversation_muted(
        conversation,
        muted,
        [this, conversation, generation](auto result)
        {
            post_result(generation,
                        [this, conversation, result = std::move(result)]
                        {
                            if (result)
                            {
                                ++conversations_generation_;
                            }
                            emit mute_finished(conversation, result ? *result : false, result ? QString{} : display_error(result.error()));
                        });
        });
}

void client_bridge::set_conversation_pinned(qint64 conversation, bool pinned)
{
    auto const generation = connection_generation_.load();
    client_->set_conversation_pinned(
        conversation,
        pinned,
        [this, conversation, generation](auto result)
        {
            post_result(generation,
                        [this, conversation, result = std::move(result)]
                        {
                            if (result)
                            {
                                ++conversations_generation_;
                            }
                            emit pin_finished(conversation, result ? *result : false, result ? QString{} : display_error(result.error()));
                        });
        });
}

void client_bridge::set_group_pinned_message(qint64 conversation, std::optional<qint64> message)
{
    auto const generation = connection_generation_.load();
    auto finished = [this, conversation, generation](auto result)
    {
        post_result(generation,
                    [this, conversation, result = std::move(result)]
                    {
                        if (result)
                        {
                            ++conversations_generation_;
                        }
                        emit group_pin_finished(conversation, result ? QString{} : display_error(result.error()));
                    });
    };
    if (message)
    {
        client_->pin_group_message(conversation, *message, std::move(finished));
    }
    else
    {
        client_->unpin_group_message(conversation, std::move(finished));
    }
}

void client_bridge::open_direct_conversation(qint64 user, QString username)
{
    auto const generation = connection_generation_.load();
    client_->open_direct_conversation(user,
                                      [this, user, username = std::move(username), generation](auto result)
                                      {
                                          post_result(generation,
                                                      [this, user, username, result = std::move(result)]
                                                      {
                                                          conversation_data value;
                                                          if (result)
                                                          {
                                                              value.id = *result;
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
    client_->create_group(std::move(request_title),
                          std::move(values),
                          [this, title = std::move(title), count = members.size() + 1, generation](auto result)
                          {
                              post_result(generation,
                                          [this, title, count, result = std::move(result)]
                                          {
                                              conversation_data value;
                                              if (result)
                                              {
                                                  value.id = *result;
                                                  value.group = true;
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
    client_->join_group(to_utf8(token),
                        [this, generation](auto result)
                        {
                            post_result(generation,
                                        [this, result = std::move(result)]
                                        {
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
                                                value.member_count = result->member_count;
                                            }
                                            emit conversation_opened(value, result ? QString{} : display_error(result.error()));
                                        });
                        });
}

void client_bridge::group_invite(qint64 conversation, std::optional<bool> create)
{
    auto const generation = connection_generation_.load();
    auto finished = [this, conversation, create, generation](auto result)
    {
        post_result(generation,
                    [this, conversation, create, result = std::move(result)]
                    {
                        auto const error = result ? QString{} : display_error(result.error());
                        if (create.has_value())
                        {
                            if (result)
                            {
                                ++conversations_generation_;
                            }
                            emit group_action_finished(conversation, false, error);
                        }
                        emit group_invite_received(conversation, result && *result ? from_utf8(**result) : QString{}, error);
                    });
    };
    if (!create)
    {
        client_->get_group_invite(conversation, std::move(finished));
    }
    else if (*create)
    {
        client_->create_group_invite(conversation, std::move(finished));
    }
    else
    {
        client_->revoke_group_invite(conversation, std::move(finished));
    }
}

void client_bridge::set_group_join_approval(qint64 conversation, bool required)
{
    client_->set_group_join_approval(conversation, required, group_action_result(conversation));
}

void client_bridge::get_group_join_requests(qint64 conversation, qint64 before)
{
    auto const generation = connection_generation_.load();
    client_->get_group_join_requests(conversation,
                                     before > 0 ? std::optional<std::int64_t>(before) : std::nullopt,
                                     [this, conversation, before, generation](auto result)
                                     {
                                         post_result(generation,
                                                     [this, conversation, before, result = std::move(result)]
                                                     {
                                                         QList<user_data> values;
                                                         if (result)
                                                         {
                                                             for (auto const& request : result->requests)
                                                             {
                                                                 values.push_back(to_user_data(request.applicant));
                                                             }
                                                         }
                                                         emit group_join_requests_received(conversation,
                                                                                           std::move(values),
                                                                                           result ? result->next.value_or(0) : 0,
                                                                                           before > 0,
                                                                                           result ? QString{} : display_error(result.error()));
                                                     });
                                     });
}

void client_bridge::respond_group_join_request(qint64 conversation, qint64 user, bool accept)
{
    client_->respond_group_join_request(conversation, user, accept, group_action_result(conversation));
}

void client_bridge::get_members(qint64 conversation)
{
    auto const generation = connection_generation_.load();
    client_->get_members(conversation,
                         [this, conversation, generation](auto result)
                         {
                             post_result(generation,
                                         [this, conversation, result = std::move(result)]
                                         {
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
                                         });
                         });
}

void client_bridge::set_group_admin(qint64 conversation, qint64 user, bool admin)
{
    client_->set_group_admin(conversation, user, admin, group_action_result(conversation));
}

void client_bridge::rename_group(qint64 conversation, QString title)
{
    client_->rename_group(conversation, to_utf8(title), group_action_result(conversation));
}

void client_bridge::set_group_announcement(qint64 conversation, QString text)
{
    client_->set_group_announcement(conversation, to_utf8(text), group_action_result(conversation));
}

void client_bridge::transfer_group_owner(qint64 conversation, qint64 user)
{
    client_->transfer_group_owner(conversation, user, group_action_result(conversation));
}

void client_bridge::remove_group_member(qint64 conversation, qint64 user)
{
    client_->remove_group_member(conversation, user, group_action_result(conversation));
}

void client_bridge::invite_group_members(qint64 conversation, QList<qint64> members)
{
    client_->invite_group_members(conversation, std::vector<std::int64_t>(members.begin(), members.end()), group_action_result(conversation));
}

void client_bridge::leave_group(qint64 conversation) { client_->leave_group(conversation, group_action_result(conversation, true)); }

std::function<void(std::expected<bool, chat::error>)> client_bridge::group_action_result(qint64 conversation, bool leaving)
{
    return [this, conversation, leaving, generation = connection_generation_.load()](auto result)
    {
        post_result(generation,
                    [this, conversation, leaving, result = std::move(result)]
                    {
                        if (result)
                        {
                            ++conversations_generation_;
                        }
                        emit group_action_finished(conversation, leaving && result.has_value(), result ? QString{} : display_error(result.error()));
                    });
    };
}

void client_bridge::get_contacts()
{
    auto const generation = connection_generation_.load();
    auto const request = ++contacts_generation_;
    client_->get_contacts(
        [this, generation, request](auto result)
        {
            post_result(generation,
                        [this, request, result = std::move(result)]
                        {
                            if (request != contacts_generation_)
                            {
                                return;
                            }
                            if (!result)
                            {
                                emit contacts_received({}, display_error(result.error()));
                                return;
                            }

                            QList<user_data> contacts;
                            contacts.reserve(static_cast<qsizetype>(result->size()));
                            for (auto const& item : *result)
                            {
                                contacts.push_back(to_user_data(item));
                            }
                            emit contacts_received(std::move(contacts), {});
                        });
        });
}

void client_bridge::get_presence()
{
    auto const generation = connection_generation_.load();
    client_->get_presence(
        [this, generation](auto result)
        {
            post_result(generation,
                        [this, result = std::move(result)]
                        {
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
    auto const connection = connection_generation_.load();
    client_->get_messages(
        conversation,
        before,
        [this, conversation, before, after, generation, connection](auto result)
        {
            post_result(
                connection,
                [this, conversation, before, after, generation, connection, result = std::move(result)]() mutable
                {
                    if (generation != messages_generation_)
                    {
                        return;
                    }
                    if (!result)
                    {
                        if (result.error().kind == chat::error_kind::rpc && result.error().code == chat::conversation_unavailable)
                        {
                            emit conversation_changed(conversation, true);
                            return;
                        }
                        emit messages_received(conversation, {}, {}, before.has_value(), after.has_value(), false, display_error(result.error()));
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
                    emit messages_received(
                        conversation, std::move(messages), std::move(positions), before.has_value(), after.has_value(), result->has_more, {});
                    if (after && result->has_more && next > *after && generation == messages_generation_ && connection == connection_generation_)
                    {
                        get_messages(conversation, {}, next);
                    }
                });
        },
        after);
}

void client_bridge::set_typing(qint64 conversation, bool typing)
{
    client_->set_typing(conversation, typing, [](auto) {});
}

void client_bridge::send_message(qint64 conversation, QString text, qint64 reply_to, qint64 client_id)
{
    client_->send_message(
        conversation,
        to_utf8(text),
        [this, client_id](auto result)
        {
            auto error = result ? QString{} : display_error(result.error());
            QMetaObject::invokeMethod(this, [this, client_id, error = std::move(error)] { emit text_saved(client_id, error); }, Qt::QueuedConnection);
        },
        reply_to > 0 ? std::optional<std::int64_t>(reply_to) : std::nullopt,
        client_id);
}

void client_bridge::publish_pending_text(chat::pending_text const& item)
{
    QString rejection;
    if (item.state == chat::outbox_state::failed)
    {
        rejection = item.rejection == chat::conversation_unavailable ? QStringLiteral("会话不可用") : QStringLiteral("服务器拒绝了消息");
    }
    emit pending_text_changed(item.conversation,
                              item.client_id,
                              from_utf8(item.text),
                              item.reply_to,
                              item.created,
                              item.state == chat::outbox_state::sending,
                              std::move(rejection));
}

void client_bridge::restore_texts()
{
    client_->pending_texts(
        [this, generation = connection_generation_.load()](auto result)
        {
            post_result(generation,
                        [this, result = std::move(result)]
                        {
                            if (!result)
                            {
                                emit error(display_error(result.error()));
                                return;
                            }
                            QList<qint64> ids;
                            ids.reserve(static_cast<qsizetype>(result->size()));
                            for (auto const& item : *result)
                            {
                                ids.push_back(item.client_id);
                                publish_pending_text(item);
                            }
                            emit pending_texts_restored(std::move(ids));
                        });
        });
}

void client_bridge::change_pending_text(qint64 client_id, bool discard)
{
    auto handler = [this, client_id, discard, generation = connection_generation_.load()](auto result)
    {
        post_result(generation,
                    [this, client_id, discard, result = std::move(result)]
                    {
                        if (!result)
                        {
                            emit error(display_error(result.error()));
                        }
                        else if (discard)
                        {
                            emit pending_text_discarded(client_id);
                        }
                        else
                        {
                            restore_texts();
                        }
                    });
    };
    if (discard)
    {
        client_->discard_text(client_id, std::move(handler));
    }
    else
    {
        client_->retry_text(client_id, std::move(handler));
    }
}

void client_bridge::change_file(qint64 conversation, QString key, bool add)
{
    auto handler = [this, conversation, g = connection_generation_.load()](auto result)
    {
        post_result(g,
                    [this, conversation, result = std::move(result)]
                    { emit files_changed(conversation, result ? QString{} : display_error(result.error())); });
    };
    if (add)
    {
        client_->add_conversation_file(conversation, to_utf8(key), std::move(handler));
    }
    else
    {
        client_->remove_conversation_file(conversation, to_utf8(key), std::move(handler));
    }
}
void client_bridge::list_files(qint64 conversation, QString search, QString category, qint64 before, QString key)
{
    client_->list_conversation_files(conversation,
                                     to_utf8(search),
                                     to_utf8(category),
                                     before,
                                     to_utf8(key),
                                     [this, conversation, before, n = ++files_generation_, g = connection_generation_.load()](auto r)
                                     {
                                         post_result(g,
                                                     [this, conversation, before, n, r = std::move(r)]() mutable
                                                     {
                                                         if (n == files_generation_)
                                                         {
                                                             emit files_received(conversation,
                                                                                 r ? std::move(*r) : chat::conversation_file_page{},
                                                                                 before != 0,
                                                                                 r ? QString{} : display_error(r.error()));
                                                         }
                                                     });
                                     });
}
void client_bridge::download_file(qint64 conversation, QString key, QString path, bool open)
{
    client_->download_conversation_file(conversation,
                                        to_utf8(key),
                                        to_utf8(path),
                                        [this, conversation, path, open, g = connection_generation_.load()](auto r)
                                        {
                                            post_result(
                                                g,
                                                [this, conversation, path, open, r = std::move(r)]
                                                { emit file_downloaded(conversation, path, open, r ? QString{} : display_error(r.error())); });
                                        });
}
void client_bridge::send_attachment(qint64 conversation, QString path, qint64 reply_to, qint64 client_id, bool group_only)
{
    auto progress = [this, client_id, g = connection_generation_.load()](auto bytes, auto total)
    { post_result(g, [this, client_id, bytes, total] { emit attachment_progress(client_id, bytes, total); }); };
    if (group_only)
    {
        client_->upload_conversation_file(
            conversation,
            to_utf8(path),
            client_id,
            [this, conversation, client_id, g = connection_generation_.load()](auto r)
            {
                post_result(g,
                            [this, conversation, client_id, r = std::move(r)]
                            {
                                auto error = r ? QString{} : display_error(r.error());
                                emit group_file_finished(client_id, error);
                                emit files_changed(conversation, error);
                            });
            },
            std::move(progress));
    }
    else
    {
        client_->send_attachment(conversation,
                                 to_utf8(path),
                                 send_result(conversation, client_id, path),
                                 reply_to > 0 ? std::optional<std::int64_t>{reply_to} : std::nullopt,
                                 client_id,
                                 std::move(progress));
    }
}
void client_bridge::restore_attachments()
{
    client_->pending_attachments(
        [this, g = connection_generation_.load()](auto result)
        {
            post_result(g,
                        [this, result = std::move(result)]
                        {
                            if (!result)
                            {
                                emit error(display_error(result.error()));
                                return;
                            }
                            for (auto const& t : *result)
                            {
                                emit attachment_restored(
                                    t.conversation, t.client_id, from_utf8(t.path), t.size, t.reply_to, t.confirmed, t.group_file);
                            }
                        });
        });
}
void client_bridge::interrupt_attachment(qint64 id, bool cancel, QString path)
{
    client_->interrupt_attachment(id,
                                  cancel,
                                  [this, id, cancel, path = std::move(path), g = connection_generation_.load()](auto result)
                                  {
                                      if (result && cancel)
                                      {
                                          remove_clipboard_image(path, id);
                                      }
                                      post_result(g,
                                                  [this, id, cancel, result = std::move(result)]
                                                  {
                                                      if (!result)
                                                      {
                                                          emit error(display_error(result.error()));
                                                          return;
                                                      }
                                                      if (cancel)
                                                      {
                                                          emit attachment_cancelled(id);
                                                      }
                                                  });
                                  });
}

std::function<void(std::expected<chat::message, chat::error>)> client_bridge::send_result(qint64 conversation, qint64 client_id, QString path)
{
    // Release the acknowledged source even if the GUI has already changed sessions.
    return [this, conversation, client_id, path = std::move(path), generation = connection_generation_.load()](auto result)
    {
        if (result)
        {
            remove_clipboard_image(path, client_id);
        }
        post_result(generation,
                    [this, conversation, client_id, result = std::move(result)]
                    {
                        auto status = send_status::sent;
                        if (!result)
                        {
                            auto const& error = result.error();
                            if (error.kind == chat::error_kind::transport)
                            {
                                status = send_status::lost;
                            }
                            else if (error.kind == chat::error_kind::rpc && error.code == chat::conversation_unavailable)
                            {
                                status = send_status::unavailable;
                            }
                            else
                            {
                                status = send_status::refused;
                            }
                        }
                        emit send_finished(conversation,
                                           client_id,
                                           result ? to_message_data(*result) : message_data{},
                                           result ? QString{} : display_error(result.error()),
                                           status);
                    });
    };
}

void client_bridge::get_attachment(qint64 conversation, qint64 message, QString target)
{
    client_->get_attachment(conversation,
                            message,
                            to_utf8(target),
                            [this, conversation, message, target, g = connection_generation_.load()](auto result)
                            {
                                post_result(
                                    g,
                                    [this, conversation, message, target, result = std::move(result)]
                                    { emit attachment_received(conversation, message, target, result ? QString{} : display_error(result.error())); });
                            });
}

void client_bridge::delete_message(qint64 conversation, qint64 message)
{
    auto const generation = connection_generation_.load();
    client_->delete_message(conversation,
                            message,
                            [this, conversation, generation](auto result)
                            {
                                post_result(generation,
                                            [this, conversation, result = std::move(result)]
                                            {
                                                emit message_updated(conversation,
                                                                     result ? to_message_data(*result) : message_data{},
                                                                     result ? QString{} : display_error(result.error()));
                                            });
                            });
}

void client_bridge::get_message_image(qint64 conversation, qint64 message)
{
    auto tmp = std::make_shared<QTemporaryDir>();
    if (!tmp->isValid())
    {
        emit message_image_received(conversation, message, {}, tmp->errorString());
        return;
    }
    auto path = tmp->filePath("preview");
    client_->get_attachment(
        conversation,
        message,
        to_utf8(path),
        [this, tmp, path, conversation, message, g = connection_generation_.load()](auto result)
        {
            QFile file(path);
            auto error = result ? QString{} : display_error(result.error());
            QByteArray bytes;
            if (result)
            {
                if (file.open(QIODevice::ReadOnly))
                {
                    bytes = file.read(chat::max_image_preview_size);
                }
                if (!file.isOpen() || file.error() != QFileDevice::NoError)
                {
                    error = file.errorString();
                }
            }
            post_result(g,
                        [this, conversation, message, bytes = std::move(bytes), error = std::move(error)]
                        { emit message_image_received(conversation, message, bytes, error); });
        },
        chat::max_image_preview_size);
}

void client_bridge::edit_message(qint64 conversation, qint64 message, QString text)
{
    auto const generation = connection_generation_.load();
    client_->edit_message(conversation,
                          message,
                          to_utf8(text),
                          [this, conversation, generation](auto result)
                          {
                              post_result(generation,
                                          [this, conversation, result = std::move(result)]
                                          {
                                              emit message_updated(conversation,
                                                                   result ? to_message_data(*result) : message_data{},
                                                                   result ? QString{} : display_error(result.error()));
                                          });
                          });
}

void client_bridge::set_message_reaction(qint64 conversation, qint64 message, QString emoji)
{
    auto const generation = connection_generation_.load();
    client_->set_message_reaction(conversation,
                                  message,
                                  to_utf8(emoji),
                                  [this, conversation, message, generation](auto result) mutable
                                  {
                                      post_result(generation,
                                                  [this, conversation, message, result = std::move(result)]() mutable
                                                  {
                                                      emit reaction_changed(conversation,
                                                                            message,
                                                                            result ? result->revision : 0,
                                                                            result ? to_reactions(result->reactions) : QList<reaction_data>{},
                                                                            result ? QString{} : display_error(result.error()));
                                                  });
                                  });
}

void client_bridge::search_users(QString query, quint64 request)
{
    auto const generation = connection_generation_.load();
    client_->search_users(to_utf8(query),
                          [this, generation, request](auto result)
                          {
                              post_result(generation,
                                          [this, request, result = std::move(result)]
                                          {
                                              if (!result)
                                              {
                                                  emit users_received({}, display_error(result.error()), request);
                                                  return;
                                              }

                                              QList<user_data> users;
                                              users.reserve(static_cast<qsizetype>(result->size()));
                                              for (auto const& item : *result)
                                              {
                                                  users.push_back(to_user_data(item));
                                              }
                                              emit users_received(std::move(users), {}, request);
                                          });
                          });
}

void client_bridge::search_messages(qint64 conversation, QString query, qint64 before)
{
    auto request_query = to_utf8(query);
    auto const generation = ++search_generation_;
    auto const connection = connection_generation_.load();
    client_->search_messages(
        conversation,
        std::move(request_query),
        before > 0 ? std::optional<std::int64_t>{before} : std::nullopt,
        [this, conversation, query = std::move(query), before, generation, connection](auto result) mutable
        {
            post_result(connection,
                        [this, conversation, query = std::move(query), before, generation, result = std::move(result)]() mutable
                        {
                            if (generation != search_generation_)
                            {
                                return;
                            }
                            if (!result)
                            {
                                emit message_search_received(conversation, std::move(query), before, {}, {}, false, display_error(result.error()));
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
                            emit message_search_received(
                                conversation, std::move(query), before, std::move(messages), std::move(positions), result->has_more, {});
                        });
        });
}

quint64 client_bridge::search_all_messages(QString query, qint64 before)
{
    auto request = ++global_search_request_;
    client_->search_all_messages(to_utf8(query),
                                 before ? std::optional<std::int64_t>{before} : std::nullopt,
                                 [this, request, g = connection_generation_.load()](auto result)
                                 {
                                     post_result(g,
                                                 [this, request, result = std::move(result)]() mutable
                                                 {
                                                     if (result)
                                                     {
                                                         emit global_search_received(request, std::move(*result), {});
                                                     }
                                                     else
                                                     {
                                                         emit global_search_received(request, {}, display_error(result.error()));
                                                     }
                                                 });
                                 });
    return request;
}

quint64 client_bridge::get_search_context(qint64 conversation, qint64 anchor)
{
    auto request = ++global_search_request_;
    client_->get_messages_around(conversation,
                                 anchor,
                                 [this, request, anchor, g = connection_generation_.load()](auto result)
                                 {
                                     post_result(g,
                                                 [this, request, anchor, result = std::move(result)]
                                                 {
                                                     QList<message_data> messages;
                                                     if (result)
                                                     {
                                                         for (auto const& m : result->messages)
                                                         {
                                                             messages.push_back(to_message_data(m));
                                                         }
                                                     }
                                                     emit search_context_received(
                                                         request, anchor, std::move(messages), result ? QString{} : display_error(result.error()));
                                                 });
                                 });
    return request;
}

void client_bridge::get_history(qint64 conversation, QString query, qint64 before)
{
    auto const generation = ++history_generation_;
    auto const connection = connection_generation_.load();
    auto const cursor = before > 0 ? std::optional<std::int64_t>{before} : std::nullopt;
    auto receive = [this, conversation, generation, connection](auto result)
    {
        post_result(connection,
                    [this, conversation, generation, result = std::move(result)]
                    {
                        if (generation != history_generation_)
                        {
                            return;
                        }
                        if (!result)
                        {
                            emit history_received(conversation, {}, false, display_error(result.error()));
                            return;
                        }
                        QList<message_data> messages;
                        for (auto const& value : result->messages)
                        {
                            messages.push_back(to_message_data(value));
                        }
                        emit history_received(conversation, std::move(messages), result->has_more, {});
                    });
    };
    if (query.isEmpty())
    {
        client_->get_messages(conversation, cursor, std::move(receive));
    }
    else
    {
        client_->search_messages(conversation, to_utf8(query), cursor, std::move(receive));
    }
}

std::function<void(std::expected<chat::friendship_result, chat::error>)> client_bridge::friend_request_result(qint64 user)
{
    return [this, generation = connection_generation_.load(), user](auto result)
    {
        auto error = result ? QString{} : display_error(result.error());
        post_result(generation, [this, user, error = std::move(error)] { emit friend_request_finished(user, error); });
    };
}

void client_bridge::add_contact(qint64 user) { client_->send_friend_request(user, friend_request_result(user)); }

void client_bridge::get_friend_requests()
{
    auto const generation = connection_generation_.load();
    auto const request = ++friend_requests_generation_;
    client_->get_friend_requests(
        [this, generation, request](auto result)
        {
            post_result(generation,
                        [this, request, result = std::move(result)]
                        {
                            if (request != friend_requests_generation_)
                            {
                                return;
                            }
                            QList<user_data> incoming, outgoing;
                            if (!result)
                            {
                                emit friend_requests_received({}, {}, display_error(result.error()));
                                return;
                            }
                            for (auto const& request : result->incoming)
                            {
                                incoming.push_back(to_user_data(request.user));
                            }
                            for (auto const& request : result->outgoing)
                            {
                                outgoing.push_back(to_user_data(request.user));
                            }
                            emit friend_requests_received(std::move(incoming), std::move(outgoing), {});
                        });
        });
}

void client_bridge::respond_friend_request(qint64 user, bool accept) { client_->respond_friend_request(user, accept, friend_request_result(user)); }

void client_bridge::cancel_friend_request(qint64 user) { client_->cancel_friend_request(user, friend_request_result(user)); }

void client_bridge::remove_contact(qint64 user)
{
    auto const generation = connection_generation_.load();
    client_->remove_contact(
        user,
        [this, generation](auto result)
        {
            post_result(generation, [this, result = std::move(result)] { emit contact_removed(result ? QString{} : display_error(result.error())); });
        });
}

void client_bridge::mark_read(qint64 user, qint64 message)
{
    auto const generation = connection_generation_.load();
    client_->mark_read(user,
                       message,
                       [this, user, generation](std::expected<std::int64_t, chat::error> result)
                       {
                           post_result(generation,
                                       [this, user, result = std::move(result)]
                                       { emit read_marked(user, result ? *result : 0, result ? QString{} : display_error(result.error())); });
                       });
}

void client_bridge::get_avatar(qint64 user, qint64 revision)
{
    auto const generation = connection_generation_.load();
    client_->get_avatar(user,
                        revision,
                        [this, generation, user, revision](auto result)
                        {
                            post_result(generation,
                                        [this, user, revision, result = std::move(result)]
                                        {
                                            if (!result)
                                            {
                                                emit avatar_received(user, {revision, true}, {});
                                                return;
                                            }
                                            emit avatar_received(user, result->state, QByteArray(result->data.data(), result->data.size()));
                                        });
                        });
}

void client_bridge::set_avatar(QByteArray data)
{
    auto const generation = connection_generation_.load();
    client_->set_avatar(std::string(data.constData(), data.size()),
                        [this, generation](auto result)
                        {
                            post_result(generation,
                                        [this, result = std::move(result)]
                                        { emit avatar_update_finished(result ? QString{} : display_error(result.error())); });
                        });
}

void client_bridge::clear_avatar()
{
    auto const generation = connection_generation_.load();
    client_->clear_avatar(
        [this, generation](auto result)
        {
            post_result(generation,
                        [this, result = std::move(result)] { emit avatar_update_finished(result ? QString{} : display_error(result.error())); });
        });
}
