#ifndef CHAT_QT_SRC_ICONS_HPP
#define CHAT_QT_SRC_ICONS_HPP

#include <QColor>
#include <QIcon>
#include <QSize>
#include <QStringView>

QIcon svg_icon(QStringView name, QColor const& color, QSize size = QSize(20, 20));

#endif
