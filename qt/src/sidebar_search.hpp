#ifndef CHAT_QT_SRC_SIDEBAR_SEARCH_HPP
#define CHAT_QT_SRC_SIDEBAR_SEARCH_HPP

#include <QWidget>
#include <QTimer>

class avatar_cache;
class client_bridge;
class conversation_model;
class user_model;
class QLineEdit;
class QListWidget;
class QPushButton;

class sidebar_search final : public QWidget
{
    Q_OBJECT
   public:
    sidebar_search(client_bridge& client, QLineEdit& input, user_model& contacts, conversation_model& rooms, avatar_cache& avatars, QWidget* parent);

   signals:
    void contact_requested(qint64 user, QString username);
    void room_requested(qint64 conversation);
    void message_requested(qint64 conversation, qint64 message);

   private:
    void refresh_directory();
    void restart();
    void search(qint64 before);
    client_bridge& client_;
    QLineEdit& input_;
    user_model& contacts_;
    conversation_model& rooms_;
    avatar_cache& avatars_;
    QListWidget* directory_;
    QListWidget* hits_;
    QPushButton* more_;
    QTimer debounce_;
    quint64 request_ = 0;
    qint64 next_ = 0;
};
#endif
