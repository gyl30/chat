#ifndef CHAT_QT_SRC_MESSAGE_IMAGES_HPP
#define CHAT_QT_SRC_MESSAGE_IMAGES_HPP

#include <QByteArray>
#include <QCache>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QString>

class message_images final : public QObject
{
    Q_OBJECT
   public:
    explicit message_images(QObject* parent = nullptr) : QObject(parent), cache_(64 * 1024) {}
    void observe(qint64 conversation, qint64 message);
    void receive(qint64 conversation, qint64 message, QByteArray bytes, QString error);
    QPixmap image(qint64 message) const;
    QByteArray bytes(qint64 message) const;
    QString status(qint64 message) const;
    void discard_queued();
    void remove(qint64 conversation, qint64 message = 0);
    void retry();
    void clear();
   signals:
    void requested(qint64 conversation, qint64 message);
    void changed(qint64 message);
   private:
    void pump();
    struct entry
    {
        qint64 conversation;
        QByteArray bytes;
        QPixmap image;
        QString error;
    };
    QCache<qint64, entry> cache_;
    QHash<qint64, qint64> active_;
    QList<QPair<qint64, qint64>> queued_;
};

#endif
