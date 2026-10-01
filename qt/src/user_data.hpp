#ifndef CHAT_QT_SRC_USER_DATA_HPP
#define CHAT_QT_SRC_USER_DATA_HPP

#include <QList>
#include <chat/avatar.hpp>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

struct user_data
{
    qint64 id = 0;
    QString username;
    bool online = false;
    qint64 last_seen = 0;
    chat::avatar_state avatar;
};

Q_DECLARE_METATYPE(user_data)
Q_DECLARE_METATYPE(QList<user_data>)

#endif
