#ifndef CHAT_QT_SRC_CREATE_GROUP_DIALOG_HPP
#define CHAT_QT_SRC_CREATE_GROUP_DIALOG_HPP

#include <QDialog>
#include <QList>

class avatar_cache;
class user_model;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class create_group_dialog final : public QDialog
{
   public:
    explicit create_group_dialog(QWidget* parent);
    void set_contacts(user_model const& contacts, qint64 self, avatar_cache const& avatars);
    QString group_title() const;
    QList<qint64> selected_members() const;

   private:
    void update_selection();

    QLineEdit* search_;
    QLabel* selected_count_;
    QListWidget* selected_;
    QListWidget* list_;
    QLineEdit* title_;
    QPushButton* proceed_;
};
#endif
