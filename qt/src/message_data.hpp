#ifndef CHAT_QT_SRC_MESSAGE_DATA_HPP
#define CHAT_QT_SRC_MESSAGE_DATA_HPP

#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include <QHash>

struct quoted_message_data
{
    qint64 id = 0;
    QString username;
    QString text;
    qint64 edited_at = 0;
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
    quoted_message_data reply;
};

Q_DECLARE_METATYPE(message_data)
Q_DECLARE_METATYPE(QList<message_data>)

using read_positions = QHash<qint64, qint64>;
Q_DECLARE_METATYPE(read_positions)

#endif
