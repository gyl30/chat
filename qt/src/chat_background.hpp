#ifndef CHAT_QT_SRC_CHAT_BACKGROUND_HPP
#define CHAT_QT_SRC_CHAT_BACKGROUND_HPP

class QPainter;
class QRect;

enum class chat_background
{
    solid,
    jade,
    sky,
    sand,
    night,
    count
};

void paint_chat_background(QPainter& painter, const QRect& area, chat_background choice, bool dark);

#endif
