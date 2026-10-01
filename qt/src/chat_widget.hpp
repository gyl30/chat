#ifndef CHAT_QT_SRC_CHAT_WIDGET_HPP
#define CHAT_QT_SRC_CHAT_WIDGET_HPP

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#include "conversation_data.hpp"
#include "message_data.hpp"
#include "presence_data.hpp"
#include "user_data.hpp"

class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPushButton;
class QSortFilterProxyModel;
class QStackedWidget;
class QToolButton;
class conversation_model;
class message_model;
class user_model;

class chat_widget final : public QWidget
{
    Q_OBJECT

   public:
    explicit chat_widget(QWidget* parent = nullptr);

    void set_user(QString const& username);
    void set_loading();
    void set_error(QString message);
    void set_connection_available(bool available);
    void set_connection_status(QString text, bool retry_enabled);
    void set_conversations(QList<conversation_data> conversations);
    void set_contacts(QList<user_data> contacts);
    void set_presences(QList<presence_data> users);
    void set_presence(presence_data user);
    void set_contacts_error(QString message);
    void set_add_contact_search_results(QList<user_data> users);
    void set_add_contact_search_error(QString message);
    void finish_add_contact();
    void set_messages(qint64 user, QList<message_data> messages, qint64 read_message, bool older);
    void set_read_message(qint64 user, qint64 message);
    void add_message(qint64 user, message_data message);
    void add_sent_message(qint64 user, qint64 message, qint64 timestamp, QString text);
    void set_message_error(qint64 user, QString message);

    qint64 active_user() const noexcept;
    qint64 latest_message_id() const;

   signals:
    void conversation_selected(qint64 user);
    void older_messages_requested(qint64 user, qint64 before);
    void send_message_requested(qint64 user, QString text);
    void add_contact_search_requested(QString query);
    void contact_add_requested(qint64 user);
    void logout_requested();
    void reconnect_requested();

   private:
    void show_conversations_section();
    void show_contacts_section();
    void show_add_contact_section();
    void filter_contacts(QString const& query);
    void search_users();
    void select_contact(QModelIndex const& index);
    void select_add_user(QModelIndex const& index);
    void select_conversation(QModelIndex const& index);
    void open_chat(qint64 user, QString username);
    void request_older_messages();
    void send_current_message();
    void show_user_details(qint64 user, QString const& username);
    void update_chat_header(QString const& username);
    void update_chat_presence();
    void set_message_status(QString message);

    QLabel* profile_avatar_ = nullptr;
    QToolButton* chats_navigation_ = nullptr;
    QToolButton* contacts_navigation_ = nullptr;
    QToolButton* logout_navigation_ = nullptr;
    QToolButton* sidebar_back_button_ = nullptr;
    QToolButton* add_contact_button_ = nullptr;
    QLabel* section_title_ = nullptr;
    QStackedWidget* sidebar_pages_ = nullptr;
    QListView* conversations_view_ = nullptr;
    QLabel* conversations_status_ = nullptr;
    QLineEdit* contact_search_ = nullptr;
    QListView* contacts_view_ = nullptr;
    QLabel* contacts_status_ = nullptr;
    QLineEdit* add_user_search_ = nullptr;
    QListView* add_users_view_ = nullptr;
    QLabel* add_users_status_ = nullptr;
    QPushButton* chat_title_ = nullptr;
    QLabel* chat_presence_ = nullptr;
    QToolButton* connection_status_ = nullptr;
    QLabel* message_status_ = nullptr;
    QListView* messages_view_ = nullptr;
    QLineEdit* message_edit_ = nullptr;
    QToolButton* send_button_ = nullptr;
    conversation_model* conversations_ = nullptr;
    user_model* contacts_ = nullptr;
    QSortFilterProxyModel* contacts_filter_ = nullptr;
    user_model* add_users_ = nullptr;
    message_model* messages_ = nullptr;
    QHash<qint64, presence_data> presence_;
    qint64 active_user_ = 0;
    QString active_username_;
    bool messages_loaded_ = false;
    bool messages_loading_ = false;
    bool history_exhausted_ = false;
    bool connection_available_ = true;
};

#endif
