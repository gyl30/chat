#ifndef CHAT_QT_SRC_GROUP_DIALOG_HPP
#define CHAT_QT_SRC_GROUP_DIALOG_HPP

#include <QDialog>
#include "member_data.hpp"

class QLabel;
class QListWidget;
class QPushButton;

class group_dialog final : public QDialog
{
    Q_OBJECT

   public:
    group_dialog(qint64 conversation, qint64 self_user, QString const& title, QWidget* parent);
    void set_members(qint64 conversation, QList<member_data> members, QString const& error);
    void finish_action(qint64 conversation, QString const& error);
    void set_error(QString const& error);

   signals:
    void admin_requested(qint64 user, bool admin);

   private:
    void update_actions();

    qint64 conversation_;
    qint64 self_user_;
    QList<member_data> members_;
    bool pending_ = false;
    bool available_ = false;
    QListWidget* list_;
    QPushButton* admin_button_;
    QLabel* status_;
};

#endif
