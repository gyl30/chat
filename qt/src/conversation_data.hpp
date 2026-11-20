#ifndef CHAT_QT_SRC_CONVERSATION_DATA_HPP
#define CHAT_QT_SRC_CONVERSATION_DATA_HPP

#include <QList>
#include <chat/conversation.hpp>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include "message_data.hpp"

struct conversation_data
{
    qint64 id = 0;
    bool group = false;
    qint64 user = 0;
    QString username;
    QString remark{};
    qint64 last_message_id = 0;
    qint64 last_timestamp = 0;
    QString last_text;
    quint64 unread = 0;
    bool online = false;
    quint64 member_count = 0;
    chat::avatar_state avatar;
    chat::notification_level notifications = chat::notification_level::all;
    qint64 mute_until = 0;
    bool marked_unread = false;
    bool muted() const
    {
        return notifications == chat::notification_level::muted && (!mute_until || mute_until > QDateTime::currentMSecsSinceEpoch());
    }
    bool pinned = false;
    quoted_message_data pinned_message;
    QString announcement;
    bool join_approval = false;
};

Q_DECLARE_METATYPE(conversation_data)
Q_DECLARE_METATYPE(QList<conversation_data>)

#endif
