#ifndef CHAT_QT_SRC_CONVERSATION_DATA_HPP
#define CHAT_QT_SRC_CONVERSATION_DATA_HPP

#include <QList>
#include <chat/conversation.hpp>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QtGlobal>
#include "message_data.hpp"
#include "user_data.hpp"

using conversation_folder_list = QList<QPair<qint64, QString>>;

struct conversation_data
{
    qint64 id = 0;
    bool group = false;
    qint64 folder = 0;
    qint64 user = 0;
    QString username;
    QString remark{};
    QString nickname{};
    QString display_name() const { return user_display_name(username, nickname, remark); }
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
    QString description;
    qint64 avatar_key() const { return group ? -id : user; }
    bool join_approval = false;
    chat::member_role speech_role = chat::member_role::member;
    bool group_muted = false;
    qint64 speech_until = 0;
    qint64 server_clock_delta = 0;
    qint64 server_now() const { return QDateTime::currentMSecsSinceEpoch() + server_clock_delta; }
    bool speech_blocked() const
    {
        return group && speech_role == chat::member_role::member && (group_muted || speech_until == -1 || speech_until > server_now());
    }
};

Q_DECLARE_METATYPE(conversation_data)
Q_DECLARE_METATYPE(QList<conversation_data>)

#endif
