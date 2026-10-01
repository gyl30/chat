#ifndef CHAT_QT_SRC_PRESENCE_DATA_HPP
#define CHAT_QT_SRC_PRESENCE_DATA_HPP

#include <QList>
#include <QMetaType>
#include <QtGlobal>

struct presence_data
{
    qint64 user = 0;
    bool online = false;
    qint64 last_seen = 0;
};

Q_DECLARE_METATYPE(presence_data)
Q_DECLARE_METATYPE(QList<presence_data>)

#endif
