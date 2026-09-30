#ifndef CHAT_QT_SRC_MESSAGE_MODEL_HPP
#define CHAT_QT_SRC_MESSAGE_MODEL_HPP

#include <QAbstractListModel>
#include <QList>
#include <QString>

#include "message_data.hpp"

class message_model final : public QAbstractListModel
{
   public:
    enum role
    {
        id_role = Qt::UserRole + 1,
        from_role,
        timestamp_role,
        text_role,
        outgoing_role,
        sender_name_role,
    };

    explicit message_model(QObject* parent = nullptr);

    int rowCount(QModelIndex const& parent = {}) const override;
    QVariant data(QModelIndex const& index, int role) const override;

    void set_self_username(QString username);
    void reset(qint64 peer_user, QString peer_username = {});
    int merge_messages(QList<message_data> messages);
    bool add_message(message_data message);
    qint64 first_message_id() const;
    qint64 last_message_id() const;

   private:
    QList<message_data> messages_;
    QString self_username_;
    QString peer_username_;
    qint64 peer_user_ = 0;
};

#endif
