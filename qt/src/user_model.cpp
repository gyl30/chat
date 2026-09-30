#include "user_model.hpp"

#include <utility>

user_model::user_model(QObject* parent) : QAbstractListModel(parent) {}

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
        default:
            return {};
    }
}

void user_model::set_users(QList<user_data> users)
{
    beginResetModel();
    users_ = std::move(users);
    endResetModel();
}

user_data const* user_model::user_at(QModelIndex const& index) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= users_.size())
    {
        return nullptr;
    }
    return &users_[index.row()];
}
