#include "conversation_model.hpp"

#include <utility>

conversation_model::conversation_model(QObject* parent) : QAbstractListModel(parent) {}

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
        case user_role:
            return item->user;
        case last_text_role:
            return item->last_text;
        case unread_role:
            return QVariant::fromValue(item->unread);
        default:
            return {};
    }
}

void conversation_model::set_conversations(QList<conversation_data> conversations)
{
    beginResetModel();
    conversations_ = std::move(conversations);
    endResetModel();
}

conversation_data const* conversation_model::conversation_at(QModelIndex const& index) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= conversations_.size())
    {
        return nullptr;
    }
    return &conversations_[index.row()];
}

QModelIndex conversation_model::index_for_user(qint64 user) const
{
    for (int row = 0; row < conversations_.size(); ++row)
    {
        if (conversations_[row].user == user)
        {
            return index(row, 0);
        }
    }
    return {};
}
