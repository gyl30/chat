#ifndef CHAT_QT_SRC_CONVERSATION_MODEL_HPP
#define CHAT_QT_SRC_CONVERSATION_MODEL_HPP

#include <QAbstractListModel>
#include <QList>

#include "conversation_data.hpp"

class conversation_model final : public QAbstractListModel
{
   public:
    enum role
    {
        user_role = Qt::UserRole + 1,
        username_role,
        last_text_role,
        last_timestamp_role,
        unread_role,
    };

    explicit conversation_model(QObject* parent = nullptr);

    int rowCount(QModelIndex const& parent = {}) const override;
    QVariant data(QModelIndex const& index, int role) const override;

    void set_conversations(QList<conversation_data> conversations);
    conversation_data const* conversation_at(QModelIndex const& index) const;
    QModelIndex index_for_user(qint64 user) const;

   private:
    QList<conversation_data> conversations_;
};

#endif
