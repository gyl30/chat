#ifndef CHAT_QT_SRC_MESSAGE_DATA_HPP
#define CHAT_QT_SRC_MESSAGE_DATA_HPP

#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

struct message_data
{
    qint64 id = 0;
    qint64 from = 0;
    QString text;
};

Q_DECLARE_METATYPE(message_data)
Q_DECLARE_METATYPE(QList<message_data>)

#endif
