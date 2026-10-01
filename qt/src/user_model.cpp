#include "user_model.hpp"
#include "avatar.hpp"

#include <utility>

user_model::user_model(QObject* parent, avatar_cache* avatars) : QAbstractListModel(parent), avatars_(avatars)
{
    if (avatars_)
    {
        connect(avatars_, &avatar_cache::changed, this, [this](qint64 user) {
            for (int row = 0; row < users_.size(); ++row)
            {
                auto const& item = users_[row];
                if (item.id == user)
                {
                    emit dataChanged(index(row, 0), index(row, 0), {Qt::DecorationRole});
                }
            }
        });
    }
}

int user_model::rowCount(QModelIndex const& parent) const
{
    return parent.isValid() ? 0 : users_.size();
}

QVariant user_model::data(QModelIndex const& index, int role) const
{
    auto const* item = user_at(index);
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
        case online_role:
            return item->online;
        case last_seen_role:
            return item->last_seen;
        case Qt::DecorationRole:
            return avatars_ ? QVariant::fromValue(avatars_->image(item->id)) : QVariant{};
        default:
            return {};
    }
}

void user_model::set_users(QList<user_data> users)
{
    if (avatars_)
    {
        for (auto const& item : users)
        {
            avatars_->observe(item.id, item.avatar);
        }
    }
    beginResetModel();
    users_ = std::move(users);
    endResetModel();
}

void user_model::set_presence(qint64 user, bool online, qint64 last_seen)
{
    for (int row = 0; row < users_.size(); ++row)
    {
        auto& item = users_[row];
        if (item.id != user)
        {
            continue;
        }

        if (item.online == online && item.last_seen == last_seen)
        {
            return;
        }

        item.online = online;
        item.last_seen = last_seen;
        auto const item_index = index(row, 0);
        emit dataChanged(item_index, item_index, {online_role, last_seen_role});
        return;
    }
}

user_data const* user_model::user_at(QModelIndex const& index) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= users_.size())
    {
        return nullptr;
    }
    return &users_[index.row()];
}
