#ifndef CHAT_QT_SRC_PRESENCE_HPP
#define CHAT_QT_SRC_PRESENCE_HPP

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QtGlobal>

inline QString presence_text(bool online, qint64 last_seen)
{
    if (online)
    {
        return QStringLiteral("在线");
    }
    if (last_seen <= 0)
    {
        return {};
    }

    auto const value = QDateTime::fromMSecsSinceEpoch(last_seen).toLocalTime();
    auto const today = QDate::currentDate();
    if (value.date() == today)
    {
        return QStringLiteral("最后上线于 %1").arg(value.toString(QStringLiteral("HH:mm")));
    }
    if (value.date() == today.addDays(-1))
    {
        return QStringLiteral("最后上线于昨天 %1").arg(value.toString(QStringLiteral("HH:mm")));
    }
    if (value.date().year() == today.year())
    {
        return QStringLiteral("最后上线于 %1月%2日 %3")
            .arg(value.date().month())
            .arg(value.date().day())
            .arg(value.toString(QStringLiteral("HH:mm")));
    }
    return QStringLiteral("最后上线于 %1年%2月%3日 %4")
        .arg(value.date().year())
        .arg(value.date().month())
        .arg(value.date().day())
        .arg(value.toString(QStringLiteral("HH:mm")));
}

#endif
