#ifndef CHAT_QT_SRC_CHAT_WIDGET_HPP
#define CHAT_QT_SRC_CHAT_WIDGET_HPP

#include <QList>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#include "conversation_data.hpp"
#include "message_data.hpp"

class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPushButton;
class QToolButton;
class conversation_model;
class message_model;

class chat_widget final : public QWidget
{
    Q_OBJECT

   public:
    explicit chat_widget(QWidget* parent = nullptr);

    void set_user(QString const& username);
    void set_loading();
    void set_error(QString message);
    void set_conversations(QList<conversation_data> conversations);
    void set_messages(qint64 user, QList<message_data> messages, bool older);
    void add_message(qint64 user, message_data message);
    void add_sent_message(qint64 user, qint64 message, qint64 timestamp, QString text, bool realtime);
    void set_message_error(qint64 user, QString message);

    qint64 active_user() const noexcept;
    qint64 latest_message_id() const;

   signals:
    void conversation_selected(qint64 user);
    void older_messages_requested(qint64 user, qint64 before);
    void send_message_requested(qint64 user, QString text);

   private:
    void select_conversation(QModelIndex const& index);
    void request_older_messages();
    void send_current_message();
    void show_conversation_details();
    void show_conversation_details(conversation_data const& item);
    void update_conversation_details(conversation_data const& item);
    void set_message_status(QString message);

    QLabel* profile_avatar_ = nullptr;
    QListView* conversations_view_ = nullptr;
    QLabel* conversations_status_ = nullptr;
    QPushButton* chat_title_ = nullptr;
    QLabel* message_status_ = nullptr;
    QListView* messages_view_ = nullptr;
    QLineEdit* message_edit_ = nullptr;
    QToolButton* send_button_ = nullptr;
    conversation_model* conversations_ = nullptr;
    message_model* messages_ = nullptr;
    qint64 active_user_ = 0;
    bool messages_loaded_ = false;
    bool messages_loading_ = false;
    bool history_exhausted_ = false;
};

#endif
