#pragma once

#include <QMetaType>
#include <QString>

class QFont;
class QPainter;
class QRect;

enum class file_action
{
    none,
    download,
    pause,
    resume,
    cancel,
    open,
};

struct file_transfer
{
    file_action action = file_action::none;
    QString status;
    double progress = -1;
};

Q_DECLARE_METATYPE(file_transfer)

inline constexpr int file_row_height = 56;
QString file_size(qint64 bytes);
void paint_file_row(QPainter& painter,
                    QRect const& rect,
                    QFont font,
                    QString const& name,
                    QString const& details,
                    file_transfer const& transfer = {},
                    QStringView glyph = u"attach");
