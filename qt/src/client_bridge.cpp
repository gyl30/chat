#include "client_bridge.hpp"

#include <QSysInfo>
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

read_positions to_read_positions(std::vector<chat::read_position> const& values)
{
    read_positions positions;
    for (auto const& value : values)
    {
        positions.insert(value.user, value.message);
    }
    return positions;
}

user_data to_user_data(chat::user const& user)
{
    return {user.id, from_utf8(user.username), false, 0, user.avatar, {}, {}, from_utf8(user.nickname)};
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

quoted_message_data client_bridge::to_reply_data(std::optional<chat::quoted_message> const& reply)
{
    return reply ? quoted_message_data{reply->id,
                                       user_display_name(from_utf8(reply->username), from_utf8(reply->nickname), friend_remark(reply->from)),
                                       from_utf8(reply->text),
                                       reply->edited_at.value_or(0),
                                       reply->deleted,
                                       reply->from}
                 : quoted_message_data{};
}

message_data client_bridge::to_message_data(chat::message const& value)
{
    message_data message;
    message.forwarded_user = value.forwarded_user;
    message.forwarded_from = user_display_name(from_utf8(value.forwarded_from), {}, friend_remark(value.forwarded_user));
    message.id = value.id;
    message.acknowledgement_only = value.acknowledgement_only;
    message.conversation = value.conversation;
    message.from = value.from;
    message.username = user_display_name(from_utf8(value.username), from_utf8(value.nickname), friend_remark(value.from));
    message.avatar = value.avatar;
    message.reaction_revision = value.reaction_revision;
    message.reactions = to_reactions(value.reactions);
    message.mentions = to_mentions(value.mentions);
    message.timestamp = value.timestamp;
    message.text = from_utf8(value.text);
    message.poll = value.poll;
    message.content = QString::fromStdString(value.content);
    message.markdown = value.markdown;
    message.mention_all = value.mention_all;
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

QList<message_data> client_bridge::to_messages(std::vector<chat::message> const& values)
{
    QList<message_data> messages;
    for (auto const& value : values)
    {
        messages.push_back(to_message_data(value));
    }
    return messages;
}

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
    // Revocation and socket closure are emitted in order from the SDK thread.
    client_->set_login_revoked_handler([this] { emit login_revoked(); });
    client_->set_login_sessions_changed_handler([this] { post_result(connection_generation_.load(), [this] { emit login_sessions_changed(); }); });
    client_->set_error_handler([this](chat::error const& value) { emit error(display_error(value)); });
    client_->set_outbox_handler([this](chat::pending_text item)
                                { post_result(connection_generation_.load(), [this, item = std::move(item)] { publish_pending_text(item); }); });
    client_->set_message_handler(
        [this](chat::message message)
        { post_result(connection_generation_.load(), [this, message = std::move(message)] { emit message_received(to_message_data(message)); }); });
    client_->set_poll_change_handler(
        [this](chat::poll_update update)
        { post_result(connection_generation_.load(), [this, update = std::move(update)] { emit poll_changed(update); }); });
    client_->set_message_updated_handler(
        [this](chat::message value)
        {
            post_result(connection_generation_.load(),
                        [this, value = std::move(value)] { emit message_updated(value.conversation, to_message_data(value), {}); });
        });
    client_->set_bookmark_changed_handler(
        [this](qint64 message)
        {
            auto const generation = connection_generation_.load();
            post_result(generation, [this, message] { emit bookmark_changed(message); });
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
    client_->set_conversation_view_handler(
        [this](chat::conversation_view_event view)
        {
            post_result(connection_generation_.load(),
                        [this, view = std::move(view)]
                        {
                            if (view.action == "notifications")
                            {
                                emit notifications_changed(view.conversation, static_cast<chat::notification_level>(*view.level), *view.until);
                            }
                            emit personal_view_changed(view.conversation, from_utf8(view.action), view.through.value_or(0));
                        });
        });
    client_->set_conversation_handler(
        [this](std::int64_t id, bool removed)
        {
            post_result(connection_generation_.load(),
                        [this, id, removed]
                        {
                            if (removed)
                            {
                                ++conversations_generation_;
                            }
                            emit conversation_changed(id, removed);
                        });
        });
    client_->set_contacts_changed_handler(
        [this]
        {
            post_result(connection_generation_.load(),
                        [this]
                        {
                            get_contacts();
                            get_conversations();
                        });
        });
    client_->set_profile_changed_handler(
        [this](std::int64_t id)
        {
            post_result(connection_generation_.load(),
                        [this, id]
                        {
                            get_contacts();
                            get_conversations();
                            get_friend_requests();
                            get_user_profile(id);
                            emit profile_changed(id);
                        });
        });
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

void client_bridge::close(bool keep_local_account)
{
    ++connection_generation_;
    client_->close(keep_local_account);
}

void client_bridge::authenticate(QString const& username, QString const& password)
{
    auto const generation = connection_generation_.load();
    auto done = [this, generation](auto result)
    {
        post_result(generation,
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
    };
    if (password.isEmpty())
    {
        client_->resume_session(to_utf8(username), std::move(done));
        return;
    }
    client_->authenticate(to_utf8(username),
                          to_utf8(password),
                          std::move(done),
                          to_utf8(QSysInfo::prettyProductName().left(32)),
                          to_utf8(QSysInfo::kernelType().left(10) + QStringLiteral(" desktop")));
}

quint64 client_bridge::get_login_sessions()
{
    auto const request = ++login_sessions_request_;
    client_->get_login_sessions(
        [this, request, generation = connection_generation_.load()](auto result)
        {
            post_result(generation,
                        [this, request, result = std::move(result)]() mutable
                        {
                            emit login_sessions_received(request,
                                                         result ? std::move(*result) : std::vector<chat::login_session>{},
                                                         result ? QString{} : display_error(result.error()));
                        });
        });
    return request;
}

chat::client::group_action_handler client_bridge::login_management_result(QString success)
{
    return [this, success = std::move(success), generation = connection_generation_.load()](auto result)
    {
        post_result(generation,
                    [this, success, result = std::move(result)]
                    { emit login_management_finished(result ? success : display_error(result.error()), result.has_value()); });
    };
}

void client_bridge::revoke_login_session(qint64 id) { client_->revoke_login_session(id, login_management_result(QStringLiteral("该会话已退出"))); }

void client_bridge::revoke_other_login_sessions() { client_->revoke_other_login_sessions(login_management_result(QStringLiteral("其他会话已退出"))); }

void client_bridge::change_password(QString const& current, QString const& replacement)
{
    client_->change_password(to_utf8(current), to_utf8(replacement), login_management_result(QStringLiteral("密码已修改，其他会话已退出")));
}

void client_bridge::logout_session()
{
    client_->logout_session([this](auto result) { emit logout_finished(result ? QString{} : display_error(result.error())); });
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
                                               value.folder = item.folder;
                                               value.nickname = from_utf8(item.nickname);
                                               value.group = item.kind == chat::conversation_kind::group;
                                               value.user = item.user;
                                               value.username = from_utf8(item.username);
                                               value.remark = from_utf8(item.remark);
                                               value.avatar = item.avatar;
                                               value.member_count = item.member_count;
                                               value.last_message_id = item.last.id;
                                               value.last_timestamp = item.last.timestamp;
                                               value.last_text = item.last.deleted ? QStringLiteral("消息已撤回") : from_utf8(item.last.text);
                                               value.unread = item.unread;
                                               value.notifications = item.notifications;
                                               value.mute_until = item.mute_until;
                                               value.marked_unread = item.marked_unread;
                                               value.unread_mention = item.unread_mention;
                                               value.pinned = item.pinned;
                                               value.pinned_message = to_reply_data(item.pinned_message);
                                               value.announcement = from_utf8(item.announcement);
                                               value.description = from_utf8(item.description);
                                               value.join_approval = item.join_approval;
                                               value.speech_role = item.speech_role;
                                               value.group_muted = item.group_muted;
                                               value.speech_until = item.speech_until;
                                               value.server_clock_delta = item.server_time - QDateTime::currentMSecsSinceEpoch();
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
                                               conversation_folder_list folders;
                                               for (auto const& folder : result->folders)
                                               {
                                                   folders.append({folder.id, from_utf8(folder.name)});
                                               }
                                               emit conversation_folders_received(std::move(folders));
                                               emit conversations_received(std::move(conversations), {});
                                           }
                                       });
                               });
}

void client_bridge::change_history_view(qint64 conversation, bool hide)
{
    auto done = [this, generation = connection_generation_.load()](auto result)
    {
        if (!result)
        {
            post_result(generation, [this, message = display_error(result.error())] { emit error(message); });
        }
    };
    if (hide)
    {
        client_->hide_conversation(conversation, std::move(done));
    }
    else
    {
        client_->get_messages(conversation,
                              {},
                              [this, conversation, g = connection_generation_.load()](auto r)
                              {
                                  if (!r)
                                  {
                                      post_result(g, [this, msg = display_error(r.error())] { emit error(msg); });
                                      return;
                                  }
                                  auto through = r->messages.empty() ? r->visible_after : r->messages.back().id;
                                  post_result(g, [this, conversation, through] { emit clear_boundary_ready(conversation, through); });
                              });
    }
}
void client_bridge::clear_history(qint64 conversation, qint64 through)
{
    client_->clear_history(conversation,
                           through,
                           [this, conversation, g = connection_generation_.load()](auto r)
                           {
                               post_result(g,
                                           [this, conversation, r = std::move(r)]
                                           {
                                               if (r)
                                               {
                                                   emit personal_view_changed(conversation, QStringLiteral("clear"), *r);
                                               }
                                               else
                                               {
                                                   emit error(display_error(r.error()));
                                               }
                                           });
                           });
}
void client_bridge::set_notification_level(qint64 conversation, chat::notification_level level, qint64 until)
{
    client_->set_notification_level(conversation, level, until, settings_result());
}
void client_bridge::create_conversation_folder(QString name) { client_->create_conversation_folder(to_utf8(name), settings_result()); }
void client_bridge::rename_conversation_folder(qint64 folder, QString name)
{
    client_->rename_conversation_folder(folder, to_utf8(name), settings_result());
}
void client_bridge::delete_conversation_folder(qint64 folder) { client_->delete_conversation_folder(folder, settings_result()); }
void client_bridge::reorder_conversation_folder(qint64 folder, bool up) { client_->reorder_conversation_folder(folder, up, settings_result()); }
void client_bridge::move_conversation_to_folder(qint64 conversation, qint64 folder)
{
    client_->move_conversation_to_folder(conversation, folder, settings_result());
}
void client_bridge::mark_read_scope(qint64 conversation, qint64 folder)
{
    client_->mark_conversations_read(
        conversation > 0 ? std::optional(conversation) : std::nullopt, folder >= 0 ? std::optional(folder) : std::nullopt, settings_result());
}
void client_bridge::set_unread_mark(qint64 conversation, bool marked) { client_->set_unread_mark(conversation, marked, settings_result()); }
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
                                                 values.push_back(member_data{user.id,
                                                                              from_utf8(user.username),
                                                                              user.role,
                                                                              user.avatar,
                                                                              user.speech_until,
                                                                              from_utf8(user.nickname),
                                                                              from_utf8(user.remark)});
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

void client_bridge::set_group_description(qint64 conversation, QString text)
{
    client_->set_group_description(conversation, to_utf8(text), group_action_result(conversation));
}
void client_bridge::set_group_avatar(qint64 group, QByteArray data)
{
    auto done = [this, group, g = connection_generation_.load()](auto r)
    { post_result(g, [this, group, r = std::move(r)] { emit group_action_finished(group, false, r ? QString{} : display_error(r.error())); }); };
    if (data.isEmpty())
    {
        client_->clear_avatar(std::move(done), group);
    }
    else
    {
        client_->set_avatar(std::string(data.constData(), data.size()), std::move(done), group);
    }
}
void client_bridge::set_group_announcement(qint64 conversation, QString text)
{
    client_->set_group_announcement(conversation, to_utf8(text), group_action_result(conversation));
}

void client_bridge::transfer_group_owner(qint64 conversation, qint64 user)
{
    client_->transfer_group_owner(conversation, user, group_action_result(conversation));
}

void client_bridge::set_group_mute(qint64 conversation, bool muted)
{
    client_->set_group_mute(conversation, muted, group_action_result(conversation));
}
void client_bridge::set_group_member_mute(qint64 conversation, qint64 user, qint64 seconds)
{
    client_->set_group_member_mute(conversation, user, seconds, group_action_result(conversation));
}
void client_bridge::dissolve_group(qint64 conversation) { client_->dissolve_group(conversation, group_action_result(conversation, true)); }
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

chat::client::group_action_handler client_bridge::settings_result()
{
    return [this, generation = connection_generation_.load()](auto r)
    {
        if (!r)
        {
            post_result(generation, [this, message = display_error(r.error())] { emit error(message); });
        }
    };
}
void client_bridge::set_friend_remark(qint64 user, QString remark) { client_->set_friend_remark(user, to_utf8(remark), settings_result()); }
void client_bridge::move_friend(qint64 user, QString group) { client_->move_friend(user, to_utf8(group), settings_result()); }
void client_bridge::create_friend_group(QString name) { client_->create_friend_group(to_utf8(name), settings_result()); }
void client_bridge::rename_friend_group(QString group, QString name)
{
    client_->rename_friend_group(to_utf8(group), to_utf8(name), settings_result());
}
void client_bridge::delete_friend_group(QString group) { client_->delete_friend_group(to_utf8(group), settings_result()); }
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
                            contacts.reserve(static_cast<qsizetype>(result->users.size()));
                            for (auto const& item : result->users)
                            {
                                contacts.push_back({item.id,
                                                    from_utf8(item.username),
                                                    false,
                                                    0,
                                                    {item.avatar_revision, item.has_avatar},
                                                    from_utf8(item.remark),
                                                    from_utf8(item.group),
                                                    from_utf8(item.nickname)});
                            }
                            QStringList groups;
                            for (auto const& name : result->groups)
                            {
                                groups.append(from_utf8(name));
                            }
                            emit friend_groups_received(std::move(groups));
                            emit contacts_received(std::move(contacts), {});
                        });
        });
}

void client_bridge::get_user_profile(qint64 user)
{
    client_->get_user_profile(user,
                              [this, user, g = connection_generation_.load()](auto result)
                              {
                                  post_result(g,
                                              [this, user, result = std::move(result)]
                                              {
                                                  if (!result)
                                                  {
                                                      emit error(display_error(result.error()));
                                                      return;
                                                  }
                                                  emit profile_received(
                                                      user, from_utf8(result->username), from_utf8(result->nickname), from_utf8(result->signature));
                                              });
                              });
}
void client_bridge::set_user_profile(QString nickname, QString signature)
{
    client_->set_user_profile(to_utf8(nickname), to_utf8(signature), settings_result());
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

void client_bridge::get_messages(qint64 conversation, std::optional<qint64> before, std::optional<qint64> after, bool capture_unread)
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
                        emit messages_received(conversation, {}, {}, before.has_value(), after.has_value(), false, {}, display_error(result.error()));
                        return;
                    }
                    auto messages = to_messages(result->messages);
                    auto positions = to_read_positions(result->read_positions);
                    emit history_boundary(conversation, result->visible_after, after.has_value() && result->hidden);
                    auto const next = result->messages.empty() ? 0 : result->messages.back().id;
                    emit messages_received(conversation,
                                           std::move(messages),
                                           std::move(positions),
                                           before.has_value(),
                                           after.has_value(),
                                           result->has_more,
                                           result->first_unread,
                                           {});
                    if (after && result->has_more && next > *after && generation == messages_generation_ && connection == connection_generation_)
                    {
                        get_messages(conversation, {}, next);
                    }
                });
        },
        after,
        capture_unread);
}

void client_bridge::set_typing(qint64 conversation, bool typing)
{
    client_->set_typing(conversation, typing, [](auto) {});
}

void client_bridge::create_poll(qint64 room, QString question, QStringList options)
{
    std::vector<std::string> labels;
    for (auto const& label : options)
    {
        labels.push_back(to_utf8(label));
    }
    client_->create_poll(room,
                         to_utf8(question),
                         std::move(labels),
                         chat::client::new_client_id(),
                         [this, g = connection_generation_.load()](auto result)
                         {
                             post_result(g,
                                         [this, result = std::move(result)]
                                         {
                                             if (result)
                                             {
                                                 emit message_received(to_message_data(*result));
                                             }
                                             else
                                             {
                                                 emit error(display_error(result.error()));
                                             }
                                         });
                         });
}
chat::client::poll_handler client_bridge::poll_result(quint64 request, qint64 room, qint64 message, bool write)
{
    return [this, request, room, message, write, g = connection_generation_.load()](auto result)
    {
        post_result(g,
                    [this, request, room, message, write, result = std::move(result)]
                    {
                        emit poll_received(
                            request, room, message, result ? *result : chat::poll_state{}, result ? QString{} : display_error(result.error()));
                        if (result && write)
                        {
                            emit poll_changed({room, message, result->summary});
                        }
                    });
    };
}
void client_bridge::refresh_live_poll(qint64 room, qint64 message)
{
    // A vote or withdrawal can reach this connection before the creation frame.
    // The forward cursor includes a visible tombstone; around and get_poll exclude it.
    client_->get_messages(
        room,
        {},
        [this, room, message, g = connection_generation_.load()](auto result)
        {
            if (!result || result->messages.empty() || result->messages.front().id != message)
            {
                return;
            }
            post_result(g,
                        [this, room, m = std::move(result->messages.front())]
                        {
                            if (m.deleted)
                            {
                                emit message_updated(room, to_message_data(m), {});
                            }
                            else if (m.poll)
                            {
                                emit poll_changed({room, m.id, *m.poll});
                            }
                        });
        },
        message - 1);
}

quint64 client_bridge::get_poll(qint64 room, qint64 message)
{
    auto request = ++content_request_;
    client_->get_poll(room, message, poll_result(request, room, message, false));
    return request;
}
quint64 client_bridge::vote_poll(qint64 room, qint64 message, qint64 choice)
{
    auto request = ++content_request_;
    client_->vote_poll(room, message, choice, poll_result(request, room, message, true));
    return request;
}
quint64 client_bridge::close_poll(qint64 room, qint64 message)
{
    auto request = ++content_request_;
    client_->close_poll(room, message, poll_result(request, room, message, true));
    return request;
}
quint64 client_bridge::get_poll_voters(qint64 room, qint64 message, qint64 choice, qint64 after)
{
    auto request = ++content_request_;
    client_->get_poll_voters(
        room,
        message,
        choice,
        after,
        [this, request, message, choice, after, g = connection_generation_.load()](auto result)
        {
            post_result(
                g,
                [this, request, message, choice, after, result = std::move(result)]
                {
                    emit voters_received(
                        request, message, choice, after, result ? *result : chat::poll_voters{}, result ? QString{} : display_error(result.error()));
                });
        });
    return request;
}
quint64 client_bridge::get_bundle(qint64 conversation, qint64 message, qint64 after)
{
    auto request = ++content_request_;
    client_->get_forwarded_messages(conversation,
                                    message,
                                    after,
                                    [this, request, conversation, message, g = connection_generation_.load()](auto result)
                                    {
                                        post_result(g,
                                                    [this, request, conversation, message, result = std::move(result)]
                                                    {
                                                        emit bundle_received(request,
                                                                             conversation,
                                                                             message,
                                                                             result ? *result : chat::forwarded_page{},
                                                                             result ? QString{} : display_error(result.error()));
                                                    });
                                    });
    return request;
}
void client_bridge::forward_bundle(qint64 target, QList<qint64> messages)
{
    client_->forward_bundle(target,
                            {messages.begin(), messages.end()},
                            chat::client::new_client_id(),
                            [this, g = connection_generation_.load()](auto result)
                            {
                                post_result(g,
                                            [this, result = std::move(result)]
                                            {
                                                if (result)
                                                {
                                                    emit message_received(to_message_data(*result));
                                                }
                                                else
                                                {
                                                    emit error(display_error(result.error()));
                                                }
                                            });
                            });
}
void client_bridge::forward_messages(qint64 target, QList<qint64> messages)
{
    if (messages.isEmpty())
    {
        return;
    }
    auto const source = messages.takeFirst();
    client_->forward_message(
        target,
        source,
        chat::client::new_client_id(),
        [this, target, messages = std::move(messages), generation = connection_generation_.load()](auto result) mutable
        {
            post_result(
                generation,
                [this, target, messages = std::move(messages), result = std::move(result)]() mutable
                {
                    if (!result)
                    {
                        emit error(QStringLiteral("转发中断，剩余 %1 条尚未确认。%2").arg(messages.size() + 1).arg(display_error(result.error())));
                        return;
                    }
                    emit message_received(to_message_data(*result));
                    forward_messages(target, std::move(messages));
                });
        });
}

void client_bridge::send_message(qint64 conversation, QString text, qint64 reply_to, qint64 client_id, qint64 revision, chat::text_options options)
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
        client_id,
        revision,
        options);
}

void client_bridge::save_draft(qint64 user, qint64 conversation, qint64 revision, qint64 reply, QString text, chat::text_options options)
{
    client_->save_draft(user,
                        {conversation, revision, reply, to_utf8(text), options},
                        [this, g = connection_generation_.load()](auto result)
                        {
                            if (!result)
                            {
                                post_result(g, [this, error = display_error(result.error())] { emit this->error(error); });
                            }
                        });
}

void client_bridge::restore_drafts(qint64 user)
{
    client_->load_drafts(user,
                         [this, user, g = connection_generation_.load()](auto result)
                         {
                             post_result(g,
                                         [this, user, result = std::move(result)]() mutable
                                         {
                                             if (!result)
                                             {
                                                 emit error(display_error(result.error()));
                                                 return;
                                             }
                                             QList<qint64> referenced;
                                             for (auto const& value : *result)
                                             {
                                                 if (value.reply)
                                                 {
                                                     referenced.push_back(value.conversation);
                                                 }
                                             }
                                             emit drafts_restored(user, std::move(*result));
                                             if (referenced.isEmpty())
                                             {
                                                 return;
                                             }
                                             client_->get_message_search_scopes(
                                                 [this, referenced = std::move(referenced), generation = connection_generation_.load()](
                                                     auto scopes) mutable
                                                 {
                                                     if (scopes)
                                                     {
                                                         referenced.removeIf(
                                                             [&](auto id)
                                                             {
                                                                 return std::ranges::find(*scopes, id, &chat::message_search_scope::id) !=
                                                                        scopes->end();
                                                             });
                                                         post_result(generation,
                                                                     [this, referenced = std::move(referenced)]
                                                                     { emit draft_conversations_unavailable(referenced); });
                                                     }
                                                 });
                                         });
                         });
}

void client_bridge::validate_draft_reply(qint64 conversation, qint64 anchor, qint64 revision)
{
    client_->get_messages_around(conversation,
                                 anchor,
                                 [this, conversation, anchor, revision, g = connection_generation_.load()](auto result)
                                 {
                                     post_result(g,
                                                 [this, conversation, anchor, revision, result = std::move(result)]
                                                 {
                                                     if (!result && result.error().code != chat::conversation_unavailable)
                                                     {
                                                         return;
                                                     }
                                                     if (result)
                                                     {
                                                         for (auto const& message : result->messages)
                                                         {
                                                             if (message.id == anchor && !message.deleted)
                                                             {
                                                                 emit draft_reply_validated(
                                                                     conversation, anchor, revision, to_message_data(message), false);
                                                                 return;
                                                             }
                                                         }
                                                     }
                                                     emit draft_reply_validated(conversation, anchor, revision, {}, true);
                                                 });
                                 });
}

void client_bridge::publish_pending_text(chat::pending_text const& item)
{
    QString rejection;
    if (item.state == chat::outbox_state::failed)
    {
        rejection = item.rejection == chat::conversation_unavailable ? QStringLiteral("会话不可用")
                    : item.rejection == chat::group_speech_muted     ? QStringLiteral("群聊禁言，解除后可手动重试")
                                                                     : QStringLiteral("服务器拒绝了消息");
    }
    emit pending_text_changed(item.conversation,
                              item.client_id,
                              from_utf8(item.text),
                              item.reply_to,
                              item.created,
                              item.state == chat::outbox_state::sending,
                              std::move(rejection),
                              item.rejection == chat::conversation_unavailable,
                              item.options);
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
void client_bridge::download_file(qint64 conversation, QString key, QString filename, QString path, bool open)
{
    emit download_requested(conversation, 0, std::move(key), std::move(filename), std::move(path), open);
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
                                auto const unavailable =
                                    !r && r.error().kind == chat::error_kind::rpc && r.error().code == chat::conversation_unavailable;
                                emit group_file_finished(client_id, error, unavailable);
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

void client_bridge::get_attachment(qint64 conversation, qint64 message, QString filename, QString target, qint64 item)
{
    emit download_requested(conversation, message, {}, std::move(filename), std::move(target), false, item);
}

void client_bridge::start_download(qint64 id,
                                   quint64 attempt,
                                   qint64 conversation,
                                   qint64 message,
                                   QString key,
                                   QString path,
                                   std::shared_ptr<chat::download_control> control,
                                   bool replace,
                                   qint64 item)
{
    // The task list survives reconnects; a completion from the failed connection still retires this attempt.
    auto progress = [this, id, attempt](qint64 bytes, qint64 total)
    {
        QMetaObject::invokeMethod(
            this, [this, id, attempt, bytes, total] { emit download_progress(id, attempt, bytes, total); }, Qt::QueuedConnection);
    };
    auto done = [this, id, attempt](auto result)
    {
        auto error = result ? QString{} : display_error(result.error());
        QMetaObject::invokeMethod(
            this, [this, id, attempt, error = std::move(error)] { emit download_finished(id, attempt, error); }, Qt::QueuedConnection);
    };
    chat::download_options options{std::move(control), std::move(progress), replace};
    if (message)
    {
        client_->get_attachment(conversation, message, to_utf8(path), std::move(done), chat::max_attachment_size, std::move(options), item);
    }
    else
    {
        client_->download_conversation_file(conversation, to_utf8(key), to_utf8(path), std::move(done), std::move(options));
    }
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

quint64 client_bridge::get_message_image(qint64 conversation, qint64 message, qint64 item)
{
    auto const request = item ? ++content_request_ : 0;
    auto tmp = std::make_shared<QTemporaryDir>();
    if (!tmp->isValid())
    {
        if (item)
        {
            post_result(connection_generation_.load(),
                        [this, request, message, item, error = tmp->errorString()]
                        { emit bundle_image_received(request, message, item, {}, error); });
        }
        else
        {
            emit message_image_received(conversation, message, {}, tmp->errorString());
        }
        return request;
    }
    auto path = tmp->filePath("preview");
    client_->get_attachment(
        conversation,
        message,
        to_utf8(path),
        [this, request, tmp, path, conversation, message, item, g = connection_generation_.load()](auto result)
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
                        [this, request, conversation, message, item, bytes = std::move(bytes), error = std::move(error)]
                        {
                            if (item)
                            {
                                emit bundle_image_received(request, message, item, bytes, error);
                            }
                            else
                            {
                                emit message_image_received(conversation, message, bytes, error);
                            }
                        });
        },
        chat::max_image_preview_size,
        {},
        item);
    return request;
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
                            auto messages = to_messages(result->messages);
                            auto positions = to_read_positions(result->read_positions);
                            emit message_search_received(
                                conversation, std::move(query), before, std::move(messages), std::move(positions), result->has_more, {});
                        });
        });
}

quint64 client_bridge::get_message_bookmark(qint64 message)
{
    auto const request = ++bookmark_request_;
    client_->get_message_bookmark(
        message,
        [this, request, generation = connection_generation_.load()](auto result)
        {
            post_result(generation,
                        [this, request, result = std::move(result)]
                        { emit bookmark_state_received(request, result && result->bookmarked, result ? QString{} : display_error(result.error())); });
        });
    return request;
}

void client_bridge::set_message_bookmark(qint64 message, bool bookmarked)
{
    client_->set_message_bookmark(message,
                                  bookmarked,
                                  [this, message, bookmarked, generation = connection_generation_.load()](auto result)
                                  {
                                      post_result(generation,
                                                  [this, message, bookmarked, result = std::move(result)]
                                                  { emit bookmark_saved(message, bookmarked, result ? QString{} : display_error(result.error())); });
                                  });
}

quint64 client_bridge::list_message_bookmarks(QString const& query, qint64 before)
{
    auto const request = ++bookmark_request_;
    client_->list_message_bookmarks(to_utf8(query),
                                    before ? std::optional<std::int64_t>(before) : std::nullopt,
                                    [this, request, generation = connection_generation_.load()](auto result)
                                    {
                                        post_result(generation,
                                                    [this, request, result = std::move(result)]() mutable
                                                    {
                                                        emit bookmarks_received(request,
                                                                                result ? std::move(*result) : chat::bookmark_page{},
                                                                                result ? QString{} : display_error(result.error()));
                                                    });
                                    });
    return request;
}

quint64 client_bridge::search_all_messages(chat::message_search_filter const& filter, qint64 before)
{
    auto request = ++global_search_request_;
    client_->search_all_messages(filter,
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

quint64 client_bridge::get_message_search_scopes()
{
    auto const request = ++global_search_request_;
    client_->get_message_search_scopes(
        [this, request, g = connection_generation_.load()](auto result)
        {
            post_result(g,
                        [this, request, result = std::move(result)]() mutable
                        {
                            emit message_search_scopes_received(request,
                                                                result ? std::move(*result) : std::vector<chat::message_search_scope>{},
                                                                result ? QString{} : display_error(result.error()));
                        });
        });
    return request;
}

quint64 client_bridge::find_search_senders(QString query)
{
    auto const request = ++global_search_request_;
    search_users(std::move(query), request);
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
                                                     emit search_context_received(request,
                                                                                  anchor,
                                                                                  result ? to_messages(result->messages) : QList<message_data>{},
                                                                                  result ? QString{} : display_error(result.error()),
                                                                                  !result && result.error().code == chat::conversation_unavailable);
                                                 });
                                 });
    return request;
}

void client_bridge::get_history(qint64 conversation, chat::message_category category, QString query, qint64 before)
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
                        emit history_received(conversation, to_messages(result->messages), result->has_more, {});
                    });
    };
    client_->get_categorized_history(conversation, category, to_utf8(query), cursor, std::move(receive));
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
    auto done = [this, generation, user, revision](auto result)
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
    };
    if (user < 0)
    {
        client_->get_group_avatar(-user, revision, std::move(done));
    }
    else
    {
        client_->get_avatar(user, revision, std::move(done));
    }
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
