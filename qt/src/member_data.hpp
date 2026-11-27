#ifndef CHAT_QT_SRC_MEMBER_DATA_HPP
#define CHAT_QT_SRC_MEMBER_DATA_HPP

#include <QList>
#include <chat/avatar.hpp>
#include <QMetaType>
#include <QString>
#include <chat/member.hpp>
#include "user_data.hpp"

struct member_data
{
    qint64 id = 0;
    QString username;
    chat::member_role role = chat::member_role::member;
    chat::avatar_state avatar;
    qint64 speech_until = 0;
    QString nickname{}, remark{};
    qint64 joined_message_id = 0;
    bool operator==(member_data const&) const = default;
    QString display_name() const { return user_display_name(username, nickname, remark); }
};

Q_DECLARE_METATYPE(member_data)
Q_DECLARE_METATYPE(QList<member_data>)

#endif
