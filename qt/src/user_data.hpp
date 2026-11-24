#ifndef CHAT_QT_SRC_USER_DATA_HPP
#define CHAT_QT_SRC_USER_DATA_HPP

#include <QList>
#include <chat/avatar.hpp>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

inline QString user_display_name(QString const& username, QString const& nickname, QString const& remark = {})
{
    return !remark.isEmpty() ? remark : !nickname.isEmpty() ? nickname : username;
}

struct user_data
{
    qint64 id = 0;
    QString username;
    bool online = false;
    qint64 last_seen = 0;
    chat::avatar_state avatar;
    QString remark{};
    QString group{};
    QString nickname{};
    QString display_name() const { return user_display_name(username, nickname, remark); }
};

Q_DECLARE_METATYPE(QList<user_data>)

#endif
