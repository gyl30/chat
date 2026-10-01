#ifndef CHAT_QT_SRC_AVATAR_HPP
#define CHAT_QT_SRC_AVATAR_HPP

#include <QColor>
#include <QIcon>
#include <QRect>
#include <QString>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QImage>
#include <QByteArray>
#include <chat/avatar.hpp>

Q_DECLARE_METATYPE(chat::avatar_state)

class QPainter;

QString avatar_initial(QString const& username);
QColor avatar_background(QString const& username);
void paint_avatar(QPainter& painter, QRect const& rect, QString const& username, int font_size = 0, QPixmap const& image = {});
QIcon avatar_icon(QString const& username, int size, QPixmap const& image = {});

QImage decode_avatar(QByteArray const& bytes);

class avatar_cache final : public QObject
{
    Q_OBJECT
   public:
    explicit avatar_cache(QObject* parent = nullptr) : QObject(parent) {}
    void observe(qint64 user, chat::avatar_state state);
    void receive(qint64 user, chat::avatar_state state, QByteArray const& bytes);
    QPixmap image(qint64 user) const;
    chat::avatar_state state(qint64 user) const;
    void retry();
    void clear();
   signals:
    void requested(qint64 user, qint64 revision);
    void changed(qint64 user);

   private:
    struct entry
    {
        chat::avatar_state state;
        QPixmap image;
        bool attempted = false;
    };
    QHash<qint64, entry> entries_;
};

#endif
