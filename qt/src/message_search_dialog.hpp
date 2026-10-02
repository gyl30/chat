#ifndef CHAT_QT_SRC_MESSAGE_SEARCH_DIALOG_HPP
#define CHAT_QT_SRC_MESSAGE_SEARCH_DIALOG_HPP

#include <QDialog>

#include "message_data.hpp"

class avatar_cache;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class message_model;

class message_search_dialog final : public QDialog
{
    Q_OBJECT

   public:
    message_search_dialog(qint64 conversation, qint64 self_user, bool group, QString const& title, QString const& query,
                         QWidget* parent, avatar_cache* avatars = nullptr);
    void set_results(qint64 conversation, QString const& query, qint64 before, QList<message_data> messages,
                     read_positions positions, bool has_more, QString const& error_message);
    void set_reactions(qint64 conversation, qint64 message, qint64 revision, QList<reaction_data> reactions,
                       QString const& error);
    void update_message(qint64 conversation, message_data message, QString const& error);

   signals:
    void search_requested(QString query, qint64 before);

   private:
    void request_search(bool older);

    qint64 conversation_;
    bool group_;
    QString query_;
    qint64 before_ = 0;
    qint64 next_before_ = 0;
    QLineEdit* input_;
    QPushButton* search_button_;
    QPushButton* more_button_;
    QLabel* status_;
    QListView* results_;
    message_model* messages_;
};

#endif
