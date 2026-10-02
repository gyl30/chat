#include "conversation_model.hpp"
#include "avatar.hpp"

#include <utility>

conversation_model::conversation_model(QObject* parent, avatar_cache* avatars) : QAbstractListModel(parent), avatars_(avatars)
{
    if (avatars_)
    {
        connect(avatars_, &avatar_cache::changed, this, [this](qint64 user) {
            for (int row = 0; row < conversations_.size(); ++row)
            {
                auto const& item = conversations_[row];
                if (item.user == user && !item.group)
                {
                    emit dataChanged(index(row, 0), index(row, 0), {Qt::DecorationRole});
                }
            }
        });
    }
}

int conversation_model::rowCount(QModelIndex const& parent) const
{
    return parent.isValid() ? 0 : conversations_.size();
}

QVariant conversation_model::data(QModelIndex const& index, int role) const
{
    auto const* item = conversation_at(index);
    if (!item)
    {
        return {};
    }

    switch (role)
    {
        case Qt::DisplayRole:
        case username_role:
            return item->username;
        case id_role:
            return item->id;
        case group_role:
            return item->group;
        case user_role:
            return item->user;
        case last_text_role:
            return item->last_text;
        case last_timestamp_role:
            return item->last_timestamp;
        case unread_role:
            return QVariant::fromValue(item->unread);
        case online_role:
            return item->online;
        case muted_role:
            return item->muted;
        case pinned_role:
            return item->pinned;
        case pinned_message_role:
            return QVariant::fromValue(item->pinned_message);
        case Qt::DecorationRole:
            return avatars_ ? QVariant::fromValue(avatars_->image(item->user)) : QVariant{};
        default:
            return {};
    }
}

void conversation_model::set_conversations(QList<conversation_data> conversations)
{
    if (avatars_)
    {
        for (auto const& item : conversations)
        {
            if (!item.group) avatars_->observe(item.user, item.avatar);
        }
    }
    beginResetModel();
    conversations_ = std::move(conversations);
    endResetModel();
}

void conversation_model::set_online(qint64 user, bool online)
{
    for (int row = 0; row < conversations_.size(); ++row)
    {
        auto& item = conversations_[row];
        if (item.group || item.user != user)
        {
            continue;
        }

        if (item.online == online)
        {
            return;
        }

        item.online = online;
        auto const item_index = index(row, 0);
        emit dataChanged(item_index, item_index, {online_role});
        return;
    }
}

void conversation_model::set_muted(qint64 conversation, bool muted)
{
    auto const item_index = index_for_conversation(conversation);
    if (!item_index.isValid() || conversations_[item_index.row()].muted == muted) { return; }
    conversations_[item_index.row()].muted = muted;
    emit dataChanged(item_index, item_index, {muted_role});
}

void conversation_model::set_pinned(qint64 conversation, bool pinned)
{
    auto const item_index = index_for_conversation(conversation);
    if (!item_index.isValid() || conversations_[item_index.row()].pinned == pinned) { return; }
    conversations_[item_index.row()].pinned = pinned;
    emit dataChanged(item_index, item_index, {pinned_role});
}

conversation_data const* conversation_model::conversation_at(QModelIndex const& index) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= conversations_.size())
    {
        return nullptr;
    }
    return &conversations_[index.row()];
}

QModelIndex conversation_model::index_for_conversation(qint64 conversation) const
{
    for (int row = 0; row < conversations_.size(); ++row)
    {
        if (conversations_[row].id == conversation)
        {
            return index(row, 0);
        }
    }
    return {};
}
