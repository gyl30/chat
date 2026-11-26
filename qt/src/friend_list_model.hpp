#pragma once

#include "user_delegate.hpp"
#include <QAbstractListModel>
#include <QStringList>
#include "user_model.hpp"

// Presentation rows reference contacts in the shared user model.
class friend_list_model final : public QAbstractListModel
{
    Q_OBJECT
   public:
    enum role
    {
        header_role = user_model::last_seen_role + 1,
        collapsed_role,
        count_role
    };
    explicit friend_list_model(user_model& users);
    int rowCount(QModelIndex const& parent = {}) const override;
    QVariant data(QModelIndex const& index, int role) const override;
    Qt::ItemFlags flags(QModelIndex const& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(QModelIndexList const& indexes) const override;
    bool canDropMimeData(QMimeData const* data, Qt::DropAction action, int row, int column, QModelIndex const& parent) const override;
    bool dropMimeData(QMimeData const* data, Qt::DropAction action, int row, int column, QModelIndex const& parent) override;
    QModelIndex source_index(QModelIndex const& index) const;
    void set_account(QString key);
    void set_groups(QStringList const& groups);
    void set_query(QString const& query);
    void toggle(QModelIndex const& header);
    QStringList const& groups() const { return groups_; }

   signals:
    void move_requested(qint64 user, QString group);

   private:
    struct entry
    {
        int group;
        int source;
        int count;
    };
    void rebuild();
    user_model& users_;
    QStringList groups_{QString{}};
    QList<entry> rows_;
    QString settings_key_;
    QStringList collapsed_;
    QString query_;
};

class friend_list_delegate final : public user_delegate
{
   public:
    using user_delegate::user_delegate;
    void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, QStyleOptionViewItem const& option, QModelIndex const& index) override;
};
