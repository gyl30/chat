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
         id_role = Qt::UserRole + 1,
         user_role,
         group_role,
        username_role,
        last_text_role,
        last_timestamp_role,
        unread_role,
        online_role,
    };

    explicit conversation_model(QObject* parent = nullptr);

    int rowCount(QModelIndex const& parent = {}) const override;
    QVariant data(QModelIndex const& index, int role) const override;

    void set_conversations(QList<conversation_data> conversations);
    void set_online(qint64 user, bool online);
    conversation_data const* conversation_at(QModelIndex const& index) const;
     QModelIndex index_for_conversation(qint64 conversation) const;

   private:
    QList<conversation_data> conversations_;
};

#endif
