#ifndef CHAT_QT_SRC_MESSAGE_PREVIEW_HPP
#define CHAT_QT_SRC_MESSAGE_PREVIEW_HPP

#include <QString>
#include <QtCore/Qt>

class avatar_cache;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace chat
{
struct message_search_hit;
}

namespace preview_role
{
enum
{
    message = Qt::UserRole,
    conversation,
    sender,
    avatar,
    title,
    time,
    note,
    directory,
    heading,
    more
};
}

QString matched_message_excerpt(QString const& body, QString const& query);
void set_preview_snippet(QListWidgetItem& item, QString snippet);
void set_message_preview(QListWidgetItem& item, chat::message_search_hit const& hit, QString snippet, QString note = {});
void style_message_previews(QListWidget& list, QLineEdit& input, avatar_cache* avatars, bool compact = false);

#endif
