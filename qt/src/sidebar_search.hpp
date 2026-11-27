#ifndef CHAT_QT_SRC_SIDEBAR_SEARCH_HPP
#define CHAT_QT_SRC_SIDEBAR_SEARCH_HPP

#include <QWidget>
#include <QTimer>

class avatar_cache;
class empty_state;
class QLabel;
class client_bridge;
class conversation_model;
class user_model;
class QLineEdit;
class QListWidget;

class sidebar_search final : public QWidget
{
    Q_OBJECT
   public:
    sidebar_search(client_bridge& client, QLineEdit& input, user_model& contacts, conversation_model& rooms, avatar_cache& avatars, QWidget* parent);

   signals:
    void contact_requested(qint64 user, QString username);
    void room_requested(qint64 conversation);
    void message_requested(qint64 conversation, qint64 message);
    void more_requested(QString query);

   private:
    void refresh_directory();
    void restart();
    void search();
    void refresh_status();
    client_bridge& client_;
    QLineEdit& input_;
    user_model& contacts_;
    conversation_model& rooms_;
    avatar_cache& avatars_;
    QListWidget* results_;
    empty_state* empty_;
    QLabel* status_;
    QString error_;
    QTimer debounce_;
    quint64 request_ = 0;
};
#endif
