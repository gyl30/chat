#include "conversation_model.hpp"
#include "avatar.hpp"

#include <utility>

conversation_model::conversation_model(QObject* parent, avatar_cache* avatars) : QAbstractListModel(parent), avatars_(avatars)
{
    if (avatars_)
    {
        connect(avatars_,
                &avatar_cache::changed,
                this,
                [this](qint64 user)
                {
                    for (int row = 0; row < conversations_.size(); ++row)
                    {
                        auto const& item = conversations_[row];
                        if (item.avatar_key() == user)
                        {
                            auto ix = index_for_conversation(item.id);
                            emit dataChanged(ix, ix, {Qt::DecorationRole});
                        }
                    }
                });
    }
}

Qt::ItemFlags conversation_model::flags(QModelIndex const& index) const
{
    auto result = QAbstractListModel::flags(index);
    if (index.isValid() && !folders_.isEmpty())
    {
        result |= Qt::ItemIsDropEnabled;
        if (conversation_at(index))
        {
            result |= Qt::ItemIsDragEnabled;
        }
    }
    return result;
}

int conversation_model::rowCount(QModelIndex const& parent) const { return parent.isValid() ? 0 : rows_.size(); }

QVariant conversation_model::data(QModelIndex const& index, int role) const
{
    if (!index.isValid())
    {
        return {};
    }
    int const entry = rows_[index.row()];
    if (entry < 0)
    {
        auto const folder = -entry - 1;
        if (role == folder_header_role)
        {
            return true;
        }
        auto id = folder < folders_.size() ? folders_[folder].first : qint64(0);
        if (role == unread_role)
        {
            quint64 count = 0;
            for (auto const& room : conversations_)
            {
                if (room.folder == id)
                {
                    count += room.unread;
                }
            }
            return count;
        }
        if (role == collapsed_role)
        {
            return collapsed_.contains(id);
        }
        if (role == folder_role)
        {
            return id;
        }
        if (role == Qt::DisplayRole)
        {
            return folder < folders_.size() ? folders_[folder].second : QStringLiteral("未分组");
        }
        return {};
    }
    auto const* item = conversation_at(index);
    if (!item)
    {
        return {};
    }

    switch (role)
    {
        case Qt::DisplayRole:
            return item->display_name();
        case Qt::ToolTipRole:
        case Qt::AccessibleTextRole:
        {
            QStringList labels{item->display_name()};
            if (item->pinned)
            {
                labels << QStringLiteral("已置顶");
            }
            if (item->muted())
            {
                labels << QStringLiteral("免打扰");
            }
            if (item->unread_mention)
            {
                labels << QStringLiteral("有人提及你");
            }
            if (item->unread)
            {
                labels << QStringLiteral("%1 条未读消息").arg(item->unread);
            }
            if (item->marked_unread)
            {
                labels << QStringLiteral("已标记未读");
            }
            auto const description = labels.join(QStringLiteral(" · "));
            return role == Qt::ToolTipRole ? QStringLiteral("<qt>%1</qt>").arg(description.toHtmlEscaped()) : description;
        }
        case username_role:
            return item->username;
        case folder_role:
            return item->folder;
        case group_role:
            return item->group;
        case last_text_role:
            return item->last_text;
        case last_timestamp_role:
            return item->last_timestamp;
        case unread_mention_role:
            return item->unread_mention;
        case unread_role:
            return QVariant::fromValue(item->unread);
        case online_role:
            return item->online;
        case muted_role:
            return item->muted();
        case marked_role:
            return item->marked_unread;
        case pinned_role:
            return item->pinned;
        case avatar_key_role:
            return item->avatar_key();
        case Qt::DecorationRole:
            return avatars_ ? QVariant::fromValue(avatars_->image(item->avatar_key())) : QVariant{};
        default:
            return {};
    }
}

void conversation_model::set_folders(conversation_folder_list folders)
{
    if (folders_ == folders)
    {
        return;
    }
    beginResetModel();
    folders_ = std::move(folders);
    build_rows();
    endResetModel();
}

void conversation_model::build_rows()
{
    rows_.clear();
    if (folders_.isEmpty())
    {
        for (int i = 0; i < conversations_.size(); ++i)
        {
            rows_.append(i);
        }
        return;
    }
    QHash<qint64, QList<int>> groups;
    for (int i = 0; i < conversations_.size(); ++i)
    {
        groups[conversations_[i].folder].append(i);
    }
    for (int i = 0; i < folders_.size(); ++i)
    {
        rows_.append(-i - 1);
        rows_.append(groups.take(folders_[i].first));
    }
    rows_.append(-static_cast<int>(folders_.size()) - 1);
    // A folder may have been deleted between two pages of the same refresh.
    for (int i = 0; i < conversations_.size(); ++i)
    {
        if (groups.contains(conversations_[i].folder))
        {
            rows_.append(i);
        }
    }
}

void conversation_model::set_conversations(QList<conversation_data> conversations)
{
    if (avatars_)
    {
        for (auto const& item : conversations)
        {
            avatars_->observe(item.avatar_key(), item.avatar);
        }
    }
    beginResetModel();
    conversations_ = std::move(conversations);
    build_rows();
    endResetModel();
}

void conversation_model::discard_through(qint64 conversation, qint64 floor)
{
    auto ix = index_for_conversation(conversation);
    if (!ix.isValid())
    {
        return;
    }
    auto& item = conversations_[rows_[ix.row()]];
    if (item.pinned_message.id <= floor)
    {
        item.pinned_message = {};
    }
    if (item.last_message_id > 0 && item.last_message_id <= floor)
    {
        item.last_message_id = 0;
        item.last_timestamp = 0;
        item.last_text.clear();
        item.unread = 0;
        item.unread_mention = false;
        emit dataChanged(ix, ix, {last_text_role, last_timestamp_role, unread_role, unread_mention_role});
    }
}
void conversation_model::remove_conversation(qint64 conversation)
{
    auto const item = index_for_conversation(conversation);
    if (!item.isValid())
    {
        return;
    }
    beginResetModel();
    conversations_.removeAt(rows_[item.row()]);
    build_rows();
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
        auto const item_index = index_for_conversation(item.id);
        emit dataChanged(item_index, item_index, {online_role});
        return;
    }
}

void conversation_model::set_notifications(qint64 conversation, chat::notification_level level, qint64 until)
{
    auto ix = index_for_conversation(conversation);
    if (ix.isValid())
    {
        auto& item = conversations_[rows_[ix.row()]];
        item.notifications = level;
        item.mute_until = until;
        emit dataChanged(ix, ix, {muted_role});
    }
}
void conversation_model::set_pinned(qint64 conversation, bool pinned)
{
    auto const item_index = index_for_conversation(conversation);
    if (!item_index.isValid() || conversations_[rows_[item_index.row()]].pinned == pinned)
    {
        return;
    }
    conversations_[rows_[item_index.row()]].pinned = pinned;
    emit dataChanged(item_index, item_index, {pinned_role});
}

conversation_data const* conversation_model::conversation_at(QModelIndex const& index) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size() || rows_[index.row()] < 0)
    {
        return nullptr;
    }
    return &conversations_[rows_[index.row()]];
}

QModelIndex conversation_model::index_for_conversation(qint64 conversation) const
{
    for (int row = 0; row < rows_.size(); ++row)
    {
        if (rows_[row] >= 0 && conversations_[rows_[row]].id == conversation)
        {
            return index(row, 0);
        }
    }
    return {};
}
