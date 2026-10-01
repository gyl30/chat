#include "group_dialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

group_dialog::group_dialog(qint64 conversation, qint64 self_user, QString const& title, QWidget* parent)
    : QDialog(parent), conversation_(conversation), self_user_(self_user), title_(title)
{
    setObjectName(QStringLiteral("groupDialog"));
    setWindowTitle(title + QStringLiteral(" · 群成员"));
    resize(480, 420);
    auto* layout = new QVBoxLayout(this);
    auto* name_row = new QHBoxLayout;
    title_edit_ = new QLineEdit(title, this);
    title_edit_->setObjectName(QStringLiteral("groupTitleEdit"));
    title_edit_->setPlaceholderText(QStringLiteral("群名称"));
    name_row->addWidget(title_edit_, 1);
    rename_button_ = new QPushButton(QStringLiteral("保存群名"), this);
    rename_button_->setObjectName(QStringLiteral("groupRenameButton"));
    rename_button_->setAutoDefault(false);
    name_row->addWidget(rename_button_);
    layout->addLayout(name_row);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("groupMembersList"));
    layout->addWidget(list_, 1);
    status_ = new QLabel(QStringLiteral("正在加载成员…"), this);
    status_->setObjectName(QStringLiteral("groupStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    admin_button_ = new QPushButton(QStringLiteral("设为管理员"), this);
    admin_button_->setObjectName(QStringLiteral("groupAdminButton"));
    admin_button_->setAutoDefault(false);
    layout->addWidget(admin_button_);
    invite_button_ = new QPushButton(QStringLiteral("从联系人邀请"), this);
    invite_button_->setObjectName(QStringLiteral("groupInviteButton"));
    invite_button_->setAutoDefault(false);
    layout->addWidget(invite_button_);
    leave_button_ = new QPushButton(QStringLiteral("退出群聊"), this);
    leave_button_->setObjectName(QStringLiteral("groupLeaveButton"));
    leave_button_->setAutoDefault(false);
    layout->addWidget(leave_button_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list_, &QListWidget::currentRowChanged, this, [this] { update_actions(); });
    connect(admin_button_, &QPushButton::clicked, this, [this] {
        auto const row = list_->currentRow();
        if (row < 0 || row >= members_.size() || !admin_button_->isEnabled())
        {
            return;
        }
        pending_ = true;
        update_actions();
        emit admin_requested(members_[row].id, members_[row].role != chat::member_role::admin);
    });
    connect(title_edit_, &QLineEdit::textChanged, this, [this] { update_actions(); });
    connect(rename_button_, &QPushButton::clicked, this, [this] {
        pending_ = true;
        update_actions();
        emit rename_requested(title_edit_->text().trimmed());
    });
    connect(invite_button_, &QPushButton::clicked, this, [this] {
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("groupInviteDialog"));
        dialog.setWindowTitle(QStringLiteral("邀请联系人"));
        dialog.resize(400, 360);
        auto* layout = new QVBoxLayout(&dialog);
        auto* list = new QListWidget(&dialog);
        layout->addWidget(list);
        for (auto const& contact : contacts_)
        {
            if (std::any_of(members_.begin(), members_.end(), [&](auto const& member) { return member.id == contact.id; }))
            {
                continue;
            }
            auto* item = new QListWidgetItem(contact.username, list);
            item->setData(Qt::UserRole, contact.id);
            item->setCheckState(Qt::Unchecked);
        }
        if (list->count() == 0)
        {
            layout->addWidget(new QLabel(QStringLiteral("联系人均已在群内，请先添加其他联系人。"), &dialog));
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("邀请"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }
        QList<qint64> ids;
        for (int i = 0; i < list->count(); ++i)
        {
            if (list->item(i)->checkState() == Qt::Checked)
            {
                ids.push_back(list->item(i)->data(Qt::UserRole).toLongLong());
            }
        }
        if (!ids.isEmpty())
        {
            pending_ = true;
            update_actions();
            emit invite_requested(std::move(ids));
        }
    });
    connect(leave_button_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("退出群聊"),
            QStringLiteral("退出后将无法继续访问这个群，重新加入需要群主或管理员邀请。确定退出？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        {
            pending_ = true;
            update_actions();
            emit leave_requested();
        }
    });
    update_actions();
}

void group_dialog::set_members(qint64 conversation, QList<member_data> members, QString const& error)
{
    if (conversation != conversation_)
    {
        return;
    }
    if (!error.isEmpty())
    {
        set_error(error);
        return;
    }
    auto const row = list_->currentRow();
    auto const selected = row >= 0 && row < members_.size() ? members_[row].id : 0;
    members_ = std::move(members);
    list_->clear();
    for (auto const& member : members_)
    {
        auto const role = member.role == chat::member_role::owner ? QStringLiteral("群主") :
            member.role == chat::member_role::admin ? QStringLiteral("管理员") : QStringLiteral("成员");
        auto* item = new QListWidgetItem(member.username + QStringLiteral(" · ") + role, list_);
        item->setData(Qt::UserRole, member.id);
        if (member.id == selected)
        {
            list_->setCurrentItem(item);
        }
    }
    available_ = true;
    status_->setText(QStringLiteral("共 %1 名成员；最多 3 名管理员，群主不计入。只有群主能任免管理员。").arg(members_.size()));
    update_actions();
}

void group_dialog::finish_action(qint64 conversation, bool left, QString const& error)
{
    if (conversation != conversation_)
    {
        return;
    }
    if (left)
    {
        accept();
        return;
    }
    pending_ = false;
    if (!error.isEmpty())
    {
        status_->setText(error);
    }
    update_actions();
}

void group_dialog::set_contacts(QList<user_data> contacts, QString const& error)
{
    contacts_ready_ = error.isEmpty();
    contacts_ = std::move(contacts);
    if (!error.isEmpty())
    {
        status_->setText(error);
    }
    update_actions();
}

void group_dialog::set_conversations(QList<conversation_data> conversations, QString const& error)
{
    if (!error.isEmpty())
    {
        return;
    }
    auto const found = std::find_if(conversations.begin(), conversations.end(), [this](auto const& value) {
        return value.id == conversation_;
    });
    if (found == conversations.end())
    {
        reject();
        return;
    }
    if (title_ != found->username)
    {
        title_ = found->username;
        title_edit_->setText(title_);
        setWindowTitle(title_ + QStringLiteral(" · 群成员"));
    }
    update_actions();
}

void group_dialog::set_error(QString const& error)
{
    available_ = false;
    pending_ = false;
    status_->setText(error);
    update_actions();
}

void group_dialog::update_actions()
{
    auto const self = std::find_if(members_.begin(), members_.end(), [this](auto const& member) {
        return member.id == self_user_;
    });
    auto const owner = self != members_.end() && self->role == chat::member_role::owner;
    auto const manager = self != members_.end() && self->role != chat::member_role::member;
    auto const enabled = available_ && !pending_;
    title_edit_->setEnabled(enabled && manager);
    rename_button_->setEnabled(enabled && manager && !title_edit_->text().trimmed().isEmpty() &&
        title_edit_->text().trimmed() != title_);
    invite_button_->setEnabled(enabled && manager && contacts_ready_);
    leave_button_->setEnabled(enabled && self != members_.end() && !owner);
    leave_button_->setToolTip(owner ? QStringLiteral("群主不能直接退出，当前不支持群主转让。") : QString{});
    auto const admins = std::count_if(members_.begin(), members_.end(), [](auto const& member) {
        return member.role == chat::member_role::admin;
    });
    auto const row = list_->currentRow();
    auto const selected = row >= 0 && row < members_.size();
    auto const admin = selected && members_[row].role == chat::member_role::admin;
    admin_button_->setText(admin ? QStringLiteral("取消管理员") : QStringLiteral("设为管理员"));
    admin_button_->setEnabled(available_ && !pending_ && owner && selected &&
        members_[row].role != chat::member_role::owner && (admin || admins < 3));
}
