#ifndef CHAT_QT_SRC_MESSAGE_MODEL_HPP
#define CHAT_QT_SRC_MESSAGE_MODEL_HPP

#include <QAbstractListModel>
#include <QList>
#include <QString>

#include "message_data.hpp"

class avatar_cache;

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
        read_role,
        reply_id_role,
        reply_text_role,
        edited_at_role,
        deleted_role,
        attachment_name_role,
        attachment_type_role,
        attachment_size_role,
    };

    explicit message_model(QObject* parent = nullptr, avatar_cache* avatars = nullptr);

    int rowCount(QModelIndex const& parent = {}) const override;
    QVariant data(QModelIndex const& index, int role) const override;

    void set_self_user(qint64 user);
    void reset(qint64 conversation, bool group = false);
    int merge_messages(QList<message_data> messages);
    bool add_message(message_data message);
    void update_message(message_data const& message);
    void set_read_message(qint64 user, qint64 message);
    void set_read_positions(read_positions positions);
    qint64 first_message_id() const;
    qint64 last_message_id() const;

   private:
    avatar_cache* avatars_;
    QList<message_data> messages_;
    qint64 self_user_ = 0;
    qint64 conversation_ = 0;
    bool group_ = false;
    read_positions read_positions_;
};

#endif
