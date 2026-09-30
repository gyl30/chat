#ifndef CHAT_QT_SRC_CHAT_WIDGET_HPP
#define CHAT_QT_SRC_CHAT_WIDGET_HPP

#include <QList>
#include <QString>
#include <QWidget>

#include "conversation_data.hpp"

class QLabel;
class QListView;
class QModelIndex;
class conversation_model;

class chat_widget final : public QWidget
{
   public:
    explicit chat_widget(QWidget* parent = nullptr);

    void set_user(QString const& username);
    void set_loading();
    void set_error(QString message);
    void set_conversations(QList<conversation_data> conversations);

   private:
    void select_conversation(QModelIndex const& index);

    QLabel* profile_avatar_ = nullptr;
    QLabel* profile_name_ = nullptr;
    QListView* conversations_view_ = nullptr;
    QLabel* conversations_status_ = nullptr;
    QLabel* chat_title_ = nullptr;
    QLabel* chat_placeholder_ = nullptr;
    QLabel* detail_name_ = nullptr;
    conversation_model* conversations_ = nullptr;
};

#endif
