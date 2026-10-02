#include "message_model.hpp"
#include "avatar.hpp"
#include "message_images.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

message_model::message_model(QObject* parent, avatar_cache* avatars, message_images* images)
    : QAbstractListModel(parent), avatars_(avatars), images_(images)
{
    if (images_)
    {
        connect(images_, &message_images::changed, this, [this](qint64 message) {
            for (int row = 0; row < messages_.size(); ++row)
            {
                if (messages_[row].id == message)
                {
                    emit dataChanged(index(row, 0), index(row, 0), {image_role, image_status_role});
                }
            }
        });
    }
    if (avatars_)
    {
        connect(avatars_, &avatar_cache::changed, this, [this](qint64 user) {
            for (int row = 0; row < messages_.size(); ++row)
            {
                auto const& item = messages_[row];
                if (item.from == user)
                {
                    emit dataChanged(index(row, 0), index(row, 0), {Qt::DecorationRole});
                }
            }
        });
    }
}

int message_model::rowCount(QModelIndex const& parent) const
{
    return parent.isValid() ? 0 : messages_.size();
}

QVariant message_model::data(QModelIndex const& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= messages_.size())
    {
        return {};
    }

    auto const& message = messages_[index.row()];
    auto const outgoing = message.from == self_user_;
    switch (role)
    {
        case id_role:
            return message.id;
        case from_role:
            return message.from;
        case timestamp_role:
            return message.timestamp;
        case Qt::DisplayRole:
        case text_role:
            if (message.deleted)
            {
                return QStringLiteral("消息已删除");
            }
            if (message.attachment)
            {
                auto const& file = *message.attachment;
                return QStringLiteral("%1：%2\n%3 KiB")
                    .arg(file.media_type.startsWith(QStringLiteral("image/")) ? QStringLiteral("图片") : QStringLiteral("文件"),
                         file.filename, QString::number(file.size / 1024.0, 'f', 1));
            }
            return message.text;
        case attachment_name_role:
            return !message.deleted && message.attachment ? message.attachment->filename : QString{};
        case attachment_type_role:
            return !message.deleted && message.attachment ? message.attachment->media_type : QString{};
        case attachment_size_role:
            return !message.deleted && message.attachment ? message.attachment->size : qint64{0};
        case image_role:
            return images_ && !message.deleted ? QVariant::fromValue(images_->image(message.id)) : QVariant{};
        case image_status_role:
            return images_ && !message.deleted ? QVariant(images_->status(message.id)) : QVariant{};
        case reactions_role:
            return QVariant::fromValue(message.deleted ? QList<reaction_data>{} : message.reactions);
        case own_reaction_role:
            if (!message.deleted)
            {
                for (auto const& reaction : message.reactions)
                {
                    if (reaction.users.contains(self_user_)) { return reaction.emoji; }
                }
            }
            return QString{};
        case outgoing_role:
            return outgoing;
        case mentions_role:
            return QVariant::fromValue(message.deleted ? QList<mention_data>{} : message.mentions);
        case mentioned_role:
            return !message.deleted && group_ && std::ranges::any_of(message.mentions,
                [this](auto const& mention) { return mention.user == self_user_; });
        case sender_name_role:
            return message.username;
        case deleted_role:
            return message.deleted;
        case edited_at_role:
            return message.edited_at;
        case reply_id_role:
            return message.reply.id;
        case reply_text_role:
            return !message.deleted && message.reply.id > 0
                       ? QStringLiteral("↪ %1\n%2")
                             .arg(message.reply.username,
                                  message.reply.deleted ? QStringLiteral("消息已删除") : message.reply.text)
                       : QString{};
        case read_role:
            if (!outgoing)
            {
                return false;
            }
            if (group_ && !members_.isEmpty())
            {
                return !read_members(message.id).isEmpty();
            }
            for (auto it = read_positions_.cbegin(); it != read_positions_.cend(); ++it)
            {
                if (it.key() != self_user_ && it.value() >= message.id)
                {
                    return true;
                }
            }
            return !group_ && read_positions_.size() == 1 && read_positions_.value(self_user_) >= message.id;
        case read_count_role:
            if (!group_ || members_.isEmpty() || message.deleted)
            {
                return {};
            }
            return static_cast<int>(read_members(message.id).size());
        case Qt::DecorationRole:
            return avatars_ ? QVariant::fromValue(avatars_->image(message.from)) : QVariant{};
        default:
            return {};
    }
}

void message_model::reset(qint64 conversation, bool group)
{
    beginResetModel();
    conversation_ = conversation;
    group_ = group;
    read_positions_.clear();
    members_.clear();
    messages_.clear();
    endResetModel();
}

int message_model::merge_messages(QList<message_data> messages)
{
    for (auto const& message : messages)
    {
        update_message(message);
    }
    auto merged = messages_;
    for (auto& message : messages)
    {
        if (message.conversation != conversation_)
        {
            continue;
        }
        auto found = std::find_if(merged.cbegin(), merged.cend(), [id = message.id](message_data const& value) {
            return value.id == id;
        });
        if (found == merged.cend())
        {
            merged.push_back(std::move(message));
        }
    }

    if (merged.size() == messages_.size())
    {
        return 0;
    }

    std::sort(merged.begin(), merged.end(), [](message_data const& lhs, message_data const& rhs) { return lhs.id < rhs.id; });
    auto const inserted = merged.size() - messages_.size();
    beginResetModel();
    messages_ = std::move(merged);
    endResetModel();
    return inserted;
}

bool message_model::add_message(message_data message)
{
    if (message.conversation != conversation_)
    {
        return false;
    }
    if (avatars_) { avatars_->observe(message.from, message.avatar); }
    auto found = std::find_if(messages_.cbegin(), messages_.cend(), [id = message.id](message_data const& value) {
        return value.id == id;
    });
    if (found != messages_.cend())
    {
        return false;
    }

    auto position = std::lower_bound(messages_.cbegin(), messages_.cend(), message.id, [](message_data const& value, qint64 id) {
        return value.id < id;
    });
    auto const row = static_cast<int>(std::distance(messages_.cbegin(), position));
    beginInsertRows({}, row, row);
    messages_.insert(row, std::move(message));
    endInsertRows();
    return true;
}

void message_model::update_message(message_data const& message)
{
    if (message.deleted && images_) { images_->remove(message.conversation, message.id); }
    if (message.conversation != conversation_)
    {
        return;
    }
    if (avatars_) { avatars_->observe(message.from, message.avatar); }
    for (int row = 0; row < messages_.size(); ++row)
    {
        auto& current = messages_[row];
        bool changed = false;
        if (current.id == message.id)
        {
            if (!current.deleted && (message.deleted || message.edited_at > current.edited_at))
            {
                auto reply = current.reply;
                auto revision = current.reaction_revision;
                auto reactions = current.reactions;
                current = message;
                if (!current.deleted && revision > current.reaction_revision)
                {
                    current.reaction_revision = revision;
                    current.reactions = std::move(reactions);
                }
                if ((reply.deleted && !current.reply.deleted) || reply.edited_at > current.reply.edited_at)
                {
                    current.reply = std::move(reply);
                }
                changed = true;
            }
            if (!current.deleted && message.reaction_revision > current.reaction_revision)
            {
                current.reaction_revision = message.reaction_revision;
                current.reactions = message.reactions;
                changed = true;
            }
            if (message.reply.id == current.reply.id && !current.reply.deleted &&
                (message.reply.deleted || message.reply.edited_at > current.reply.edited_at))
            {
                current.reply = message.reply;
                changed = true;
            }
        }
        if (current.reply.id == message.id && !current.reply.deleted &&
            (message.deleted || message.edited_at > current.reply.edited_at))
        {
            current.reply = {message.id, message.username, message.text.left(160), message.edited_at, message.deleted};
            changed = true;
        }
        if (changed)
        {
            emit dataChanged(index(row, 0), index(row, 0));
        }
    }
}

bool message_model::set_reactions(qint64 message, qint64 revision, QList<reaction_data> reactions)
{
    for (int row = 0; row < messages_.size(); ++row)
    {
        auto& current = messages_[row];
        if (current.id == message)
        {
            if (!current.deleted && revision > current.reaction_revision)
            {
                current.reaction_revision = revision;
                current.reactions = std::move(reactions);
                emit dataChanged(index(row, 0), index(row, 0), {reactions_role, own_reaction_role});
            }
            return true;
        }
    }
    return false;
}

void message_model::set_read_message(qint64 user, qint64 message)
{
    if (message <= read_positions_.value(user))
    {
        return;
    }

    read_positions_.insert(user, message);
    if (!messages_.isEmpty())
    {
        emit dataChanged(index(0, 0), index(messages_.size() - 1, 0), {read_role, read_count_role});
    }
}

void message_model::set_self_user(qint64 user)
{
    self_user_ = user;
}

void message_model::set_read_positions(read_positions positions)
{
    for (auto it = positions.begin(); it != positions.end(); ++it)
    {
        it.value() = std::max(it.value(), read_positions_.value(it.key()));
    }
    read_positions_ = std::move(positions);
    if (!messages_.isEmpty())
    {
        emit dataChanged(index(0, 0), index(messages_.size() - 1, 0), {read_role, read_count_role});
    }
}

void message_model::set_members(QList<member_data> members)
{
    members_ = std::move(members);
    if (!messages_.isEmpty())
    {
        emit dataChanged(index(0, 0), index(messages_.size() - 1, 0), {read_role, read_count_role});
    }
}

QList<member_data> message_model::read_members(qint64 message) const
{
    QList<member_data> result;
    if (group_)
    {
        for (auto const& member : members_)
        {
            if (member.id != self_user_ && read_positions_.value(member.id) >= message)
            {
                result.push_back(member);
            }
        }
    }
    return result;
}

qint64 message_model::first_message_id() const
{
    return messages_.isEmpty() ? 0 : messages_.front().id;
}

qint64 message_model::last_message_id() const
{
    return messages_.isEmpty() ? 0 : messages_.back().id;
}
