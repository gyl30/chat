#ifndef CHAT_QT_SRC_USER_MODEL_HPP
#define CHAT_QT_SRC_USER_MODEL_HPP

#include <QAbstractListModel>
#include <QList>

#include "user_data.hpp"

class user_model final : public QAbstractListModel
{
   public:
    enum role
    {
        id_role = Qt::UserRole + 1,
        username_role,
        online_role,
        last_seen_role,
    };

    explicit user_model(QObject* parent = nullptr);

    int rowCount(QModelIndex const& parent = {}) const override;
    QVariant data(QModelIndex const& index, int role) const override;

    void set_users(QList<user_data> users);
    void set_presence(qint64 user, bool online, qint64 last_seen);
    user_data const* user_at(QModelIndex const& index) const;

   private:
    QList<user_data> users_;
};

#endif
