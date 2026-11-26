#include "message_model.hpp"
#include "download_tasks.hpp"
#include "avatar.hpp"
#include "emoji_segments.hpp"
#include "message_images.hpp"

#include <QDateTime>
#include <QStringList>

#include <algorithm>
#include <iterator>
#include <utility>

namespace
{

bool merge_poll(message_data& message, chat::poll_summary const& summary)
{
    if (message.deleted || (message.poll && message.poll->revision >= summary.revision))
    {
        return false;
    }
    message.poll = summary;
    return true;
}

bool update_quote(quoted_message_data& reply, message_data const& target)
{
    if (reply.id != target.id || !reply.id || reply.deleted || (!target.deleted && target.edited_at <= reply.edited_at))
    {
        return false;
    }
    reply = {target.id, target.username, grapheme_prefix(target.text, 160), target.edited_at, target.deleted, target.from};
    return true;
}

void merge_reply(quoted_message_data& reply, const QList<message_data>& messages)
{
    if (reply.id)
    {
        const auto source = std::ranges::lower_bound(messages, reply.id, {}, &message_data::id);
        if (source != messages.end())
        {
            update_quote(reply, *source);
        }
    }
}

bool merge_update(message_data& current, const message_data& update)
{
    if (current.deleted)
    {
        return false;
    }
    if (update.deleted)
    {
        current = update;
        return true;
    }
    bool changed = false;
    if (update.edited_at > current.edited_at)
    {
        // Editing changes the body, independently of reactions, poll votes and the quoted source.
        current.text = update.text;
        current.edited_at = update.edited_at;
        current.mentions = update.mentions;
        current.markdown = update.markdown;
        changed = true;
    }
    if (update.reaction_revision > current.reaction_revision)
    {
        current.reaction_revision = update.reaction_revision;
        current.reactions = update.reactions;
        changed = true;
    }
    if (update.reply.id == current.reply.id && !current.reply.deleted && (update.reply.deleted || update.reply.edited_at > current.reply.edited_at))
    {
        current.reply = update.reply;
        changed = true;
    }
    return (update.poll && merge_poll(current, *update.poll)) || changed;
}

}    // namespace

message_model::message_model(QObject* parent, avatar_cache* avatars, message_images* images)
    : QAbstractListModel(parent), avatars_(avatars), images_(images)
{
    if (images_)
    {
        connect(images_,
                &message_images::changed,
                this,
                [this](qint64 message)
                {
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
        connect(avatars_,
                &avatar_cache::changed,
                this,
                [this](qint64 user)
                {
                    for (int row = 0; row < messages_.size(); ++row)
                    {
                        auto const& item = messages_[row];
                        if (item.from == user ||
                            (item.poll && std::ranges::find(item.poll->recent, user, &chat::poll_voter::id) != item.poll->recent.end()))
                        {
                            emit dataChanged(index(row, 0), index(row, 0), {Qt::DecorationRole});
                        }
                    }
                });
    }
}

void message_model::set_downloads(download_tasks& downloads)
{
    downloads_ = &downloads;
    connect(&downloads,
            &download_tasks::changed,
            this,
            [this]
            {
                for (int row = 0; row < messages_.size(); ++row)
                {
                    if (messages_[row].attachment && !messages_[row].deleted)
                    {
                        emit dataChanged(index(row), index(row), {download_status_role});
                    }
                }
            });
}

QString message_model::participant_name(qint64 id) const
{
    if (id == self_user_)
    {
        return QStringLiteral("我");
    }
    const auto member = std::ranges::find(members_, id, &member_data::id);
    if (member != members_.end())
    {
        return member->display_name();
    }
    const auto sender = std::ranges::find(messages_, id, &message_data::from);
    return sender != messages_.end() ? sender->username : QStringLiteral("用户 %1").arg(id);
}

bool message_model::can_manage_group() const
{
    return group_ &&
           std::ranges::any_of(members_, [this](auto const& member) { return member.id == self_user_ && member.role != chat::member_role::member; });
}

int message_model::rowCount(QModelIndex const& parent) const { return parent.isValid() ? 0 : messages_.size() + pending_.size(); }

Qt::ItemFlags message_model::flags(QModelIndex const& index) const
{
    auto flags = QAbstractListModel::flags(index);
    if (index.isValid() && (index.row() >= messages_.size() || messages_[index.row()].deleted))
    {
        flags &= ~Qt::ItemIsSelectable;
    }
    return flags;
}

QVariant message_model::data(QModelIndex const& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
    {
        return {};
    }

    auto const pending = index.row() >= messages_.size();
    if (pending)
    {
        // Not stored yet: no ID, no readers, reactions or image, only its sending state.
        auto const& item = pending_[index.row() - messages_.size()];
        switch (role)
        {
            case send_state_role:
                if (item.failed)
                {
                    return failed;
                }
                return item.waiting ? waiting : sending;
            case send_error_role:
                return item.error;
            case read_role:
            case read_count_role:
            case image_role:
            case image_status_role:
                return {};
            default:
                break;
        }
    }
    auto const& message = pending ? pending_[index.row() - messages_.size()].message : messages_[index.row()];
    auto const outgoing = message.from == self_user_;
    switch (role)
    {
        case unread_boundary_role:
            return !pending && unread_boundary_ > 0 && unread_boundary_ == message.id;
        case id_role:
            return message.id;
        case client_id_role:
            return message.client_id;
        case send_state_role:
            return sent;
        case send_error_role:
            return QString{};
        case from_role:
            return message.from;
        case timestamp_role:
            return message.timestamp;
        case Qt::AccessibleTextRole:
        {
            QStringList parts{outgoing ? QStringLiteral("你") : message.username};
            if (message.timestamp > 0)
            {
                parts.push_back(QDateTime::fromMSecsSinceEpoch(message.timestamp).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
            }
            if (!message.deleted && message.reply.id > 0)
            {
                parts.push_back(QStringLiteral("回复 %1：%2")
                                    .arg(message.reply.username, message.reply.deleted ? QStringLiteral("消息已撤回") : message.reply.text));
            }
            parts.push_back(data(index, text_role).toString());
            if (pending)
            {
                auto const& item = pending_[index.row() - messages_.size()];
                parts.push_back(item.failed            ? QStringLiteral("发送失败：%1").arg(item.error)
                                : item.waiting         ? QStringLiteral("等待发送")
                                : item.error.isEmpty() ? QStringLiteral("发送中…")
                                                       : item.error);
            }
            if (!message.deleted)
            {
                if (message.edited_at > 0)
                {
                    parts.push_back(QStringLiteral("已编辑"));
                }
                if (data(index, mentioned_role).toBool())
                {
                    parts.push_back(QStringLiteral("提及你"));
                }
                auto const count = data(index, read_count_role);
                if (count.isValid())
                {
                    parts.push_back(QStringLiteral("已读 %1 人").arg(count.toInt()));
                }
                else if (outgoing && data(index, read_role).toBool())
                {
                    parts.push_back(QStringLiteral("已读"));
                }
                for (auto const& reaction : message.reactions)
                {
                    parts.push_back(QStringLiteral("表情回应 %1 %2 人").arg(reaction.emoji).arg(reaction.users.size()));
                }
            }
            return parts.join(QStringLiteral("；"));
        }
        case poll_draft_role:
            return poll_drafts_.value(message.id);
        case poll_pending_role:
            return pending_polls_.contains(index.data(id_role).toLongLong());
        case poll_role:
        {
            if (!message.poll || message.deleted)
            {
                return {};
            }
            return QVariant::fromValue(*message.poll);
        }
        case content_role:
            return message.deleted ? QString{} : message.content;
        case markdown_role:
            return message.markdown && !message.deleted;
        case Qt::DisplayRole:
        case text_role:
            if (message.deleted)
            {
                return QStringLiteral("消息已撤回");
            }
            if (message.attachment)
            {
                auto const& file = *message.attachment;
                return QStringLiteral("%1：%2\n%3 KiB")
                    .arg(file.media_type.startsWith(QStringLiteral("image/")) ? QStringLiteral("图片") : QStringLiteral("文件"),
                         file.filename,
                         QString::number(file.size / 1024.0, 'f', 1));
            }
            return message.text;
        case download_status_role:
            return downloads_ && message.attachment && !message.deleted ? downloads_->summary({message.conversation, message.id, 0, {}}) : QString{};
        case attachment_key_role:
            return message.attachment ? message.attachment->object_key : QString{};
        case attachment_name_role:
            return !message.deleted && message.attachment ? message.attachment->filename : QString{};
        case attachment_type_role:
            return !message.deleted && message.attachment ? message.attachment->media_type : QString{};
        case image_role:
            return images_ && !message.deleted ? QVariant::fromValue(images_->image(message.id)) : QVariant{};
        case image_status_role:
            return images_ && !message.deleted ? QVariant(images_->status(message.id)) : QVariant{};
        case reactions_role:
            return QVariant::fromValue(message.deleted ? QList<reaction_data>{} : message.reactions);
        case reaction_names_role:
        {
            QStringList groups;
            if (!message.deleted)
            {
                for (const auto& reaction : message.reactions)
                {
                    QStringList names;
                    for (auto id : reaction.users)
                    {
                        names.append(participant_name(id));
                    }
                    groups.append(names.join(QStringLiteral("、")));
                }
            }
            return groups;
        }
        case own_reaction_role:
            if (!message.deleted)
            {
                for (auto const& reaction : message.reactions)
                {
                    if (reaction.users.contains(self_user_))
                    {
                        return reaction.emoji;
                    }
                }
            }
            return QString{};
        case outgoing_role:
            return outgoing;
        case mentions_role:
            return QVariant::fromValue(message.deleted ? QList<mention_data>{} : message.mentions);
        case mention_all_role:
            return !message.deleted && message.mention_all;
        case mentioned_role:
            return !message.deleted && group_ &&
                   (message.mention_all || std::ranges::any_of(message.mentions, [this](auto const& mention) { return mention.user == self_user_; }));
        case forwarded_from_role:
            return message.deleted ? QString{} : message.forwarded_from;
        case sender_name_role:
            return message.username;
        case deleted_role:
            return message.deleted;
        case edited_at_role:
            return message.edited_at;
        case reply_target_role:
            return !pending && !message.deleted && !message.reply.deleted ? message.reply.id : 0;
        case reply_text_role:
            return !message.deleted && message.reply.id > 0
                       ? QStringLiteral("↪ %1\n%2")
                             .arg(message.reply.username, message.reply.deleted ? QStringLiteral("消息已撤回") : message.reply.text)
                       : QString{};
        case read_role:
            if (!outgoing)
            {
                return {};
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
            return false;
        case read_count_role:
            // Like other IMs, readers are shown only to the sender of a group message.
            if (!group_ || members_.isEmpty() || message.deleted || message.from != self_user_)
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
    end_page();
    pending_polls_.clear();
    poll_drafts_.clear();
    unread_boundary_ = 0;
    conversation_ = conversation;
    group_ = group;
    read_positions_.clear();
    members_.clear();
    messages_.clear();
    pending_.clear();
    endResetModel();
}

QModelIndex message_model::message_index(qint64 id) const
{
    auto it = std::ranges::lower_bound(messages_, id, {}, &message_data::id);
    return it != messages_.end() && it->id == id ? index(it - messages_.begin(), 0) : QModelIndex{};
}

QModelIndex message_model::next_incoming(qint64 after) const
{
    auto it = std::ranges::upper_bound(messages_, after, {}, &message_data::id);
    while (it != messages_.end() && (it->deleted || it->from == self_user_))
    {
        ++it;
    }
    return it == messages_.end() ? QModelIndex{} : index(it - messages_.begin(), 0);
}

void message_model::set_unread_boundary(qint64 id)
{
    if (id == unread_boundary_)
    {
        return;
    }
    auto old = message_index(unread_boundary_);
    unread_boundary_ = id;
    for (auto row : {old, message_index(id)})
    {
        if (row.isValid())
        {
            emit dataChanged(index(std::max(0, row.row() - 1), 0), row, {unread_boundary_role, Qt::SizeHintRole});
        }
    }
}

void message_model::set_pending(QList<pending_message> pending)
{
    pending.removeIf(
        [this](pending_message const& item)
        {
            return std::ranges::any_of(messages_,
                                       [&item](message_data const& stored)
                                       { return stored.client_id == item.message.client_id && stored.from == item.message.from; });
        });
    // Sends retain their relative order. Keep surviving client IDs in place so an unrelated
    // confirmation or failure cannot change the keyboard-focused message.
    for (int row = pending_.size() - 1; row >= 0; --row)
    {
        auto const client_id = pending_[row].message.client_id;
        if (std::ranges::any_of(pending, [client_id](auto const& item) { return item.message.client_id == client_id; }))
        {
            continue;
        }
        beginRemoveRows({}, messages_.size() + row, messages_.size() + row);
        pending_.removeAt(row);
        endRemoveRows();
    }
    for (int row = 0; row < pending.size(); ++row)
    {
        auto& item = pending[row];
        if (row == pending_.size() || pending_[row].message.client_id != item.message.client_id)
        {
            beginInsertRows({}, messages_.size() + row, messages_.size() + row);
            pending_.insert(row, std::move(item));
            endInsertRows();
        }
        else
        {
            pending_[row] = std::move(item);
            auto const changed = index(messages_.size() + row, 0);
            emit dataChanged(
                changed, changed, {Qt::DisplayRole, Qt::AccessibleTextRole, text_role, send_state_role, send_error_role, reply_text_role});
        }
    }
}

void message_model::discard_through(qint64 floor)
{
    if (page_changes_)
    {
        page_changes_->removeIf([floor](auto it) { return it.key() <= floor; });
    }
    poll_drafts_.removeIf([floor](auto it) { return it.key() <= floor; });
    auto end = std::ranges::upper_bound(messages_, floor, {}, &message_data::id);
    if (end != messages_.begin())
    {
        beginRemoveRows({}, 0, static_cast<int>(end - messages_.begin() - 1));
        messages_.erase(messages_.begin(), end);
        endRemoveRows();
    }
    for (int i = 0; i < messages_.size(); ++i)
    {
        auto& reply = messages_[i].reply;
        if (reply.id > 0 && reply.id <= floor)
        {
            reply = {};
            emit dataChanged(index(i), index(i), {reply_text_role});
        }
    }
}

void message_model::reconcile_page_row(message_data& message) const
{
    if (!page_changes_)
    {
        return;
    }
    if (auto it = page_changes_->constFind(message.id); it != page_changes_->cend())
    {
        merge_update(message, *it);
    }
    if (auto source = page_changes_->constFind(message.reply.id); source != page_changes_->cend())
    {
        update_quote(message.reply, *source);
    }
}

void message_model::discard_stored_rows()
{
    poll_drafts_.clear();
    if (!messages_.isEmpty())
    {
        beginRemoveRows({}, 0, messages_.size() - 1);
        messages_.clear();
        unread_boundary_ = 0;
        endRemoveRows();
    }
}

void message_model::merge_messages(QList<message_data> messages)
{
    for (auto& message : messages)
    {
        reconcile_page_row(message);
    }
    end_page();
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
        if (!std::ranges::binary_search(messages_, message.id, {}, &message_data::id))
        {
            merged.push_back(std::move(message));
        }
    }

    if (merged.size() == messages_.size())
    {
        return;
    }

    std::sort(merged.begin(), merged.end(), [](message_data const& lhs, message_data const& rhs) { return lhs.id < rhs.id; });
    for (auto& message : merged)
    {
        merge_reply(message.reply, merged);
    }
    // History adds messages; existing IDs and their persistent keyboard indexes survive.
    for (int row = 0; row < merged.size(); ++row)
    {
        if (row < messages_.size() && messages_[row].id == merged[row].id)
        {
            messages_[row] = std::move(merged[row]);
        }
        else
        {
            beginInsertRows({}, row, row);
            messages_.insert(row, std::move(merged[row]));
            endInsertRows();
        }
    }
    emit dataChanged(index(0, 0), index(messages_.size() - 1, 0));
}

bool message_model::add_message(message_data message)
{
    if (message.conversation != conversation_)
    {
        return false;
    }
    if (avatars_)
    {
        avatars_->observe(message.from, message.avatar);
        if (message.poll)
        {
            for (const auto& voter : message.poll->recent)
            {
                avatars_->observe(voter.id, voter.avatar);
            }
        }
    }
    reconcile_page_row(message);
    auto found = std::find_if(messages_.cbegin(), messages_.cend(), [id = message.id](message_data const& value) { return value.id == id; });
    if (found != messages_.cend())
    {
        return false;
    }

    merge_reply(message.reply, messages_);
    auto position =
        std::lower_bound(messages_.cbegin(), messages_.cend(), message.id, [](message_data const& value, qint64 id) { return value.id < id; });
    auto const row = static_cast<int>(std::distance(messages_.cbegin(), position));
    beginInsertRows({}, row, row);
    messages_.insert(row, std::move(message));
    endInsertRows();
    return true;
}

void message_model::update_message(message_data const& message)
{
    if (message.deleted)
    {
        poll_drafts_.remove(message.id);
    }
    if (message.deleted && images_)
    {
        images_->remove(message.conversation, message.id);
    }
    if (message.conversation != conversation_)
    {
        return;
    }
    if (page_changes_ && (message.deleted || message.edited_at))
    {
        auto& change = (*page_changes_)[message.id];
        if (!change.id)
        {
            change.id = message.id;
            change.from = message.from;
            change.username = message.username;
        }
        merge_update(change, message);
    }
    if (avatars_)
    {
        avatars_->observe(message.from, message.avatar);
        if (message.poll)
        {
            for (const auto& voter : message.poll->recent)
            {
                avatars_->observe(voter.id, voter.avatar);
            }
        }
    }
    for (int row = 0; row < messages_.size(); ++row)
    {
        auto& current = messages_[row];
        bool changed = current.id == message.id && merge_update(current, message);
        if (changed && current.poll && (current.poll->closed || current.poll->selection))
        {
            poll_drafts_.remove(current.id);
        }
        changed = update_quote(current.reply, message) || changed;
        if (changed)
        {
            emit dataChanged(index(row, 0), index(row, 0));
        }
    }
}

void message_model::set_poll_draft(qint64 id, qint64 choices)
{
    if (choices)
    {
        poll_drafts_.insert(id, choices);
    }
    else
    {
        poll_drafts_.remove(id);
    }
    auto ix = message_index(id);
    if (ix.isValid())
    {
        emit dataChanged(ix, ix, {poll_draft_role});
    }
}

void message_model::set_poll_pending(qint64 message, bool pending)
{
    if (pending)
    {
        pending_polls_.insert(message);
    }
    else
    {
        pending_polls_.remove(message);
    }
    if (auto row = message_index(message); row.isValid())
    {
        emit dataChanged(row, row, {poll_pending_role});
    }
}

void message_model::clear_poll_requests()
{
    for (auto id : std::exchange(pending_polls_, {}))
    {
        set_poll_pending(id, false);
    }
}

void message_model::set_poll(qint64 message, chat::poll_summary const& summary)
{
    auto it = std::ranges::lower_bound(messages_, message, {}, &message_data::id);
    if (it == messages_.end() || it->id != message)
    {
        if (page_changes_)
        {
            merge_poll((*page_changes_)[message], summary);
        }
        return;
    }
    if (merge_poll(*it, summary))
    {
        if (summary.closed || summary.selection)
        {
            poll_drafts_.remove(message);
        }
        if (avatars_)
        {
            for (const auto& voter : summary.recent)
            {
                avatars_->observe(voter.id, voter.avatar);
            }
        }
        auto row = index(static_cast<int>(it - messages_.begin()), 0);
        emit dataChanged(row, row, {poll_role, Qt::AccessibleTextRole});
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
                emit dataChanged(index(row, 0), index(row, 0), {reactions_role, own_reaction_role, Qt::AccessibleTextRole});
            }
            return true;
        }
    }
    if (page_changes_)
    {
        auto& change = (*page_changes_)[message];
        if (!change.deleted && revision > change.reaction_revision)
        {
            change.reaction_revision = revision;
            change.reactions = std::move(reactions);
        }
        return true;
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
        emit dataChanged(index(0, 0), index(messages_.size() - 1, 0), {read_role, read_count_role, Qt::AccessibleTextRole});
    }
}

void message_model::set_self_user(qint64 user) { self_user_ = user; }

void message_model::set_read_positions(read_positions positions)
{
    for (auto it = positions.begin(); it != positions.end(); ++it)
    {
        it.value() = std::max(it.value(), read_positions_.value(it.key()));
    }
    read_positions_ = std::move(positions);
    if (!messages_.isEmpty())
    {
        emit dataChanged(index(0, 0), index(messages_.size() - 1, 0), {read_role, read_count_role, Qt::AccessibleTextRole});
    }
}

void message_model::update_author(qint64 user, QString const& name)
{
    for (int i = 0; i < messages_.size(); ++i)
    {
        auto& m = messages_[i];
        if (m.from != user && m.reply.from != user && m.forwarded_user != user)
        {
            continue;
        }
        if (m.from == user)
        {
            m.username = name;
        }
        if (m.reply.from == user)
        {
            m.reply.username = name;
        }
        if (m.forwarded_user == user)
        {
            m.forwarded_from = name;
        }
        emit dataChanged(index(i, 0), index(i, 0), {sender_name_role, reply_text_role, forwarded_from_role, Qt::AccessibleTextRole});
    }
}

void message_model::update_member_nickname(qint64 user, QString const& nickname)
{
    auto it = std::ranges::find(members_, user, &member_data::id);
    if (it != members_.end())
    {
        it->nickname = nickname;
    }
}

void message_model::set_members(QList<member_data> members)
{
    members_ = std::move(members);
    if (!messages_.isEmpty())
    {
        emit dataChanged(index(0, 0), index(messages_.size() - 1, 0), {read_role, read_count_role, Qt::AccessibleTextRole});
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

qint64 message_model::first_message_id() const { return messages_.isEmpty() ? 0 : messages_.front().id; }

qint64 message_model::last_message_id() const { return messages_.isEmpty() ? 0 : messages_.back().id; }
