#ifndef CHAT_QT_SRC_MESSAGE_DATA_HPP
#define CHAT_QT_SRC_MESSAGE_DATA_HPP

#include <QList>
#include <chat/avatar.hpp>
#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include <QHash>
#include <optional>

struct attachment_data
{
    QString filename;
    QString media_type;
    qint64 size = 0;
};

struct quoted_message_data
{
    qint64 id = 0;
    QString username;
    QString text;
    qint64 edited_at = 0;
    bool deleted = false;
};
Q_DECLARE_METATYPE(quoted_message_data)

struct message_data
{
    qint64 id = 0;
    qint64 conversation = 0;
    qint64 from = 0;
    QString username;
    qint64 timestamp = 0;
    QString text;
    qint64 edited_at = 0;
    bool deleted = false;
    quoted_message_data reply;
    std::optional<attachment_data> attachment;
    chat::avatar_state avatar;
};

Q_DECLARE_METATYPE(message_data)
Q_DECLARE_METATYPE(QList<message_data>)

using read_positions = QHash<qint64, qint64>;
Q_DECLARE_METATYPE(read_positions)

#endif
