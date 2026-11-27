#ifndef CHAT_QT_SRC_CREATE_GROUP_DIALOG_HPP
#define CHAT_QT_SRC_CREATE_GROUP_DIALOG_HPP

#include <QDialog>
#include <QList>

class avatar_cache;
class user_model;
struct user_data;
class empty_state;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

class create_group_dialog final : public QDialog
{
   public:
    explicit create_group_dialog(QWidget* parent, bool invite = false);
    void set_candidates(QList<user_data> const& candidates, avatar_cache const* avatars);
    void set_contacts(user_model const& contacts, qint64 self, avatar_cache const& avatars);
    QString group_title() const;
    QList<qint64> selected_members() const;

   private:
    void update_selection();
    void filter_contacts();

    QLineEdit* search_;
    QLabel* selected_count_;
    QListWidget* selected_;
    QListWidget* list_;
    empty_state* empty_;
    QLineEdit* title_;
    QPushButton* proceed_;
};
#endif
