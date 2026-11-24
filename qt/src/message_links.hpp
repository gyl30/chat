#ifndef CHAT_QT_SRC_MESSAGE_LINKS_HPP
#define CHAT_QT_SRC_MESSAGE_LINKS_HPP

#include <QTextLayout>
#include <QUrl>

QUrl http_message_url(QString const& target);
QList<QTextLayout::FormatRange> message_links(QString const& body);

#endif
