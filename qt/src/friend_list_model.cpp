#include "icons.hpp"
#include "theme_manager.hpp"
#include <QPainter>
#include <QMouseEvent>
#include "friend_list_model.hpp"

#include <algorithm>
#include <QMimeData>
#include <QSettings>

namespace
{
class dragged_friend final : public QMimeData
{
   public:
    explicit dragged_friend(QModelIndex index) : source(index)
    {
        setData("application/x-chat-friend", QByteArray::number(index.data(user_model::id_role).toLongLong()));
    }
    QPersistentModelIndex source;
};
}    // namespace

friend_list_model::friend_list_model(user_model& users) : QAbstractListModel(&users), users_(users)
{
    connect(&users, &user_model::modelReset, this, &friend_list_model::rebuild);
    connect(&users,
            &user_model::dataChanged,
            this,
            [this](QModelIndex first, QModelIndex last, QList<int> const& roles)
            {
                for (int i = 0; i < rows_.size(); ++i)
                {
                    if (rows_[i].source >= first.row() && rows_[i].source <= last.row())
                    {
                        emit dataChanged(index(i), index(i), roles);
                    }
                }
            });
    rebuild();
}

int friend_list_model::rowCount(QModelIndex const& parent) const { return parent.isValid() ? 0 : rows_.size(); }

QModelIndex friend_list_model::source_index(QModelIndex const& ix) const
{
    return ix.model() == this ? users_.index(rows_.at(ix.row()).source, 0) : QModelIndex{};
}

QVariant friend_list_model::data(QModelIndex const& ix, int role) const
{
    if (!ix.isValid())
    {
        return {};
    }
    auto const& row = rows_[ix.row()];
    if (row.source >= 0)
    {
        return users_.index(row.source, 0).data(role);
    }
    auto const& group = groups_[row.group];
    switch (role)
    {
        case header_role:
            return true;
        case user_model::friend_group_role:
            return group;
        case Qt::DisplayRole:
            return group.isEmpty() ? QStringLiteral("我的好友") : group;
        case collapsed_role:
            return query_.isEmpty() && collapsed_.contains(group);
        case count_role:
            return row.count;
        default:
            return {};
    }
}

void friend_list_model::rebuild()
{
    beginResetModel();
    rows_.clear();
    QHash<QString, QList<int>> by_group;
    for (int i = 0; i < users_.rowCount(); ++i)
    {
        auto ix = users_.index(i, 0);
        if (query_.isEmpty() || ix.data(user_model::search_role).toString().contains(query_, Qt::CaseInsensitive))
        {
            by_group[ix.data(user_model::friend_group_role).toString()].append(i);
        }
    }
    for (int g = 0; g < groups_.size(); ++g)
    {
        auto members = by_group.take(groups_[g]);
        if (!query_.isEmpty() && members.isEmpty())
        {
            continue;
        }
        rows_.append({g, -1, int(members.size())});
        if (query_.isEmpty() && collapsed_.contains(groups_[g]))
        {
            continue;
        }
        std::sort(
            members.begin(),
            members.end(),
            [this](int a, int b)
            { return QString::localeAwareCompare(users_.user_at(users_.index(a, 0))->username, users_.user_at(users_.index(b, 0))->username) < 0; });
        for (int i : members)
        {
            rows_.append({g, i, 0});
        }
    }
    endResetModel();
}

void friend_list_model::set_account(QString key)
{
    settings_key_ = std::move(key);
    collapsed_ = QSettings().value(settings_key_).toStringList();
    groups_ = {QString{}};
    query_.clear();
    rebuild();
}

void friend_list_model::set_groups(QStringList const& groups)
{
    groups_ = {QString{}};
    groups_ += groups;
    if (collapsed_.removeIf([this](QString const& name) { return !groups_.contains(name); }))
    {
        QSettings().setValue(settings_key_, collapsed_);
    }
    rebuild();
}

void friend_list_model::set_query(QString const& query)
{
    if (query_ != query)
    {
        query_ = query;
        rebuild();
    }
}

void friend_list_model::toggle(QModelIndex const& ix)
{
    if (!ix.data(header_role).toBool() || !query_.isEmpty())
    {
        return;
    }
    QString group = ix.data(user_model::friend_group_role).toString();
    if (!collapsed_.removeOne(group))
    {
        collapsed_.append(group);
    }
    QSettings().setValue(settings_key_, collapsed_);
    rebuild();
}

Qt::ItemFlags friend_list_model::flags(QModelIndex const& ix) const
{
    if (!ix.isValid())
    {
        return Qt::NoItemFlags;
    }
    return QAbstractListModel::flags(ix) | (ix.data(header_role).toBool() ? Qt::ItemIsDropEnabled : Qt::ItemIsDragEnabled);
}

QStringList friend_list_model::mimeTypes() const { return {QStringLiteral("application/x-chat-friend")}; }

QMimeData* friend_list_model::mimeData(QModelIndexList const& indexes) const
{
    if (indexes.size() != 1 || !source_index(indexes.front()).isValid())
    {
        return nullptr;
    }
    return new dragged_friend(indexes.front());
}

bool friend_list_model::canDropMimeData(QMimeData const* data, Qt::DropAction action, int row, int, QModelIndex const& parent) const
{
    auto const* drag = dynamic_cast<dragged_friend const*>(data);
    return action == Qt::CopyAction && row == -1 && parent.data(header_role).toBool() && drag && drag->source.model() == this &&
           drag->source.data(user_model::friend_group_role) != parent.data(user_model::friend_group_role);
}

bool friend_list_model::dropMimeData(QMimeData const* data, Qt::DropAction action, int row, int column, QModelIndex const& parent)
{
    if (!canDropMimeData(data, action, row, column, parent))
    {
        return false;
    }
    auto const* drag = static_cast<dragged_friend const*>(data);
    emit move_requested(drag->source.data(user_model::id_role).toLongLong(), parent.data(user_model::friend_group_role).toString());
    return true;
}

void friend_list_delegate::paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    if (!index.data(friend_list_model::header_role).toBool())
    {
        user_delegate::paint(painter, option, index);
        return;
    }
    painter->save();
    auto row = option.rect;
    if (option.state & (QStyle::State_MouseOver | QStyle::State_HasFocus))
    {
        painter->fillRect(row, themed("subtle"));
    }
    auto color = themed("text_secondary");
    svg_icon(index.data(friend_list_model::collapsed_role).toBool() ? u"chevron-right" : u"chevron-down", color, {16, 16})
        .paint(painter, {row.left() + 12, row.center().y() - 7, 16, 16});
    painter->setPen(color);
    auto label = row.adjusted(36, 0, -16, 0);
    QString count = QString::number(index.data(friend_list_model::count_role).toInt());
    painter->drawText(label, Qt::AlignRight | Qt::AlignVCenter, count);
    label.adjust(0, 0, -option.fontMetrics.horizontalAdvance(count) - 12, 0);
    painter->drawText(label, Qt::AlignLeft | Qt::AlignVCenter, option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, label.width()));
    painter->restore();
}

QSize friend_list_delegate::sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const
{
    return index.data(friend_list_model::header_role).toBool() ? QSize(200, 38) : user_delegate::sizeHint(option, index);
}

bool friend_list_delegate::editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index)
{
    if (!index.data(friend_list_model::header_role).toBool())
    {
        return user_delegate::editorEvent(event, model, option, index);
    }
    if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
    {
        emit body_clicked(index);
        return true;
    }
    return false;
}
