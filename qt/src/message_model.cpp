#include "message_model.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

message_model::message_model(QObject* parent) : QAbstractListModel(parent) {}

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
    auto const outgoing = message.from != peer_user_;
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
            return message.text;
        case outgoing_role:
            return outgoing;
        case sender_name_role:
            return outgoing ? self_username_ : peer_username_;
        case read_role:
            return outgoing && message.id <= read_message_;
        default:
            return {};
    }
}

void message_model::set_self_username(QString username) { self_username_ = std::move(username); }

void message_model::reset(qint64 peer_user, QString peer_username)
{
    beginResetModel();
    peer_user_ = peer_user;
    peer_username_ = std::move(peer_username);
    read_message_ = 0;
    messages_.clear();
    endResetModel();
}

int message_model::merge_messages(QList<message_data> messages)
{
    auto merged = messages_;
    for (auto& message : messages)
    {
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

void message_model::set_read_message(qint64 message)
{
    if (message <= read_message_)
    {
        return;
    }

    read_message_ = message;
    if (!messages_.isEmpty())
    {
        emit dataChanged(index(0, 0), index(messages_.size() - 1, 0), {read_role});
    }
}

qint64 message_model::first_message_id() const
{
    return messages_.isEmpty() ? 0 : messages_.front().id;
}

qint64 message_model::last_message_id() const
{
    return messages_.isEmpty() ? 0 : messages_.back().id;
}
