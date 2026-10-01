#ifndef CHAT_QT_SRC_CONVERSATION_DATA_HPP
#define CHAT_QT_SRC_CONVERSATION_DATA_HPP

#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

struct conversation_data
{
    qint64 id = 0;
    bool group = false;
    qint64 user = 0;
    QString username;
    qint64 last_id = 0;
    qint64 last_from = 0;
    qint64 last_timestamp = 0;
    QString last_text;
    quint64 unread = 0;
    bool online = false;
    quint64 member_count = 0;
};

Q_DECLARE_METATYPE(conversation_data)
Q_DECLARE_METATYPE(QList<conversation_data>)

#endif
