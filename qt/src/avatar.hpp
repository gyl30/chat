#ifndef CHAT_QT_SRC_AVATAR_HPP
#define CHAT_QT_SRC_AVATAR_HPP

#include <QColor>
#include <QIcon>
#include <QRect>
#include <QString>

class QPainter;

QString avatar_initial(QString const& username);
QColor avatar_background(QString const& username);
void paint_avatar(QPainter& painter, QRect const& rect, QString const& username, int font_size = 0);
QIcon avatar_icon(QString const& username, int size);

#endif
