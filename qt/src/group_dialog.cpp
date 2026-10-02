#include "group_dialog.hpp"
#include "avatar.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QClipboard>
#include <QGuiApplication>
#include <QCheckBox>
#include <QTabWidget>
#include <QSignalBlocker>

#include <algorithm>

group_dialog::group_dialog(qint64 conversation, qint64 self_user, QString const& title, QString const& announcement, bool join_approval,
                           QWidget* parent, avatar_cache* avatars)
    : QDialog(parent), avatars_(avatars), conversation_(conversation), self_user_(self_user), title_(title),
      announcement_(announcement)
{
    setObjectName(QStringLiteral("groupDialog"));
    setWindowTitle(title + QStringLiteral(" · 群资料"));
    resize(480, 680);
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
    layout->addWidget(new QLabel(QStringLiteral("群公告（纯文本，最多 4 KiB）"), this));
    announcement_edit_ = new QPlainTextEdit(announcement, this);
    announcement_edit_->setObjectName(QStringLiteral("groupAnnouncementEdit"));
    announcement_edit_->setPlaceholderText(QStringLiteral("暂无公告"));
    announcement_edit_->setMaximumHeight(112);
    layout->addWidget(announcement_edit_);
    auto* announcement_row = new QHBoxLayout;
    announcement_button_ = new QPushButton(QStringLiteral("保存公告"), this);
    announcement_button_->setObjectName(QStringLiteral("groupAnnouncementButton"));
    announcement_button_->setAutoDefault(false);
    announcement_row->addWidget(announcement_button_);
    clear_announcement_button_ = new QPushButton(QStringLiteral("清空公告"), this);
    clear_announcement_button_->setObjectName(QStringLiteral("groupClearAnnouncementButton"));
    clear_announcement_button_->setAutoDefault(false);
    announcement_row->addWidget(clear_announcement_button_);
    layout->addLayout(announcement_row);
    invite_controls_ = new QWidget(this);
    auto* invite_layout = new QVBoxLayout(invite_controls_);
    invite_layout->setContentsMargins(0, 0, 0, 0);
    invite_layout->addWidget(new QLabel(QStringLiteral("邀请链接"), invite_controls_));
    approval_ = new QCheckBox(QStringLiteral("通过邀请链接加入需要审批"), invite_controls_);
    approval_->setObjectName(QStringLiteral("groupJoinApprovalCheck"));
    approval_->setChecked(join_approval);
    invite_layout->addWidget(approval_);
    invite_edit_ = new QLineEdit(invite_controls_);
    invite_edit_->setObjectName(QStringLiteral("groupInviteLinkEdit"));
    invite_edit_->setReadOnly(true);
    invite_edit_->setPlaceholderText(QStringLiteral("尚无邀请链接"));
    invite_layout->addWidget(invite_edit_);
    auto* invite_row = new QHBoxLayout;
    create_invite_button_ = new QPushButton(QStringLiteral("创建链接"), invite_controls_);
    create_invite_button_->setObjectName(QStringLiteral("groupCreateInviteButton"));
    copy_invite_button_ = new QPushButton(QStringLiteral("复制链接"), invite_controls_);
    copy_invite_button_->setObjectName(QStringLiteral("groupCopyInviteButton"));
    revoke_invite_button_ = new QPushButton(QStringLiteral("撤销链接"), invite_controls_);
    revoke_invite_button_->setObjectName(QStringLiteral("groupRevokeInviteButton"));
    for (auto* button : {create_invite_button_, copy_invite_button_, revoke_invite_button_})
    {
        button->setAutoDefault(false);
        invite_row->addWidget(button);
    }
    invite_layout->addLayout(invite_row);
    layout->addWidget(invite_controls_);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("groupTabs"));
    auto* members_page = new QWidget(tabs_);
    auto* members_layout = new QVBoxLayout(members_page);
    members_layout->setContentsMargins(0, 0, 0, 0);
    tabs_->addTab(members_page, QStringLiteral("成员"));
    auto* requests_page = new QWidget(tabs_);
    auto* requests_layout = new QVBoxLayout(requests_page);
    requests_layout->setContentsMargins(0, 0, 0, 0);
    requests_ = new QListWidget(requests_page);
    requests_->setObjectName(QStringLiteral("groupJoinRequestsList"));
    requests_->setIconSize(QSize(32, 32));
    requests_layout->addWidget(requests_, 1);
    auto* decisions = new QHBoxLayout;
    accept_request_button_ = new QPushButton(QStringLiteral("通过申请"), requests_page);
    accept_request_button_->setObjectName(QStringLiteral("groupAcceptRequestButton"));
    reject_request_button_ = new QPushButton(QStringLiteral("拒绝申请"), requests_page);
    reject_request_button_->setObjectName(QStringLiteral("groupRejectRequestButton"));
    for (auto* button : {accept_request_button_, reject_request_button_})
    {
        button->setAutoDefault(false);
        decisions->addWidget(button);
    }
    requests_layout->addLayout(decisions);
    more_requests_button_ = new QPushButton(QStringLiteral("加载更多申请"), requests_page);
    more_requests_button_->setObjectName(QStringLiteral("groupMoreRequestsButton"));
    more_requests_button_->setAutoDefault(false);
    requests_layout->addWidget(more_requests_button_);
    tabs_->addTab(requests_page, QStringLiteral("入群申请"));
    layout->addWidget(tabs_, 1);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("groupMembersList"));
    list_->setIconSize(QSize(32, 32));
    list_->setMinimumHeight(120);
    if (avatars_)
    {
        connect(avatars_, &avatar_cache::changed, this, [this](qint64 user) {
            for (int row = 0; row < members_.size(); ++row)
            {
                if (members_[row].id == user && row < list_->count())
                {
                    list_->item(row)->setIcon(avatar_icon(members_[row].username, 32, avatars_->image(user)));
                }
            }
            for (int row = 0; row < requests_->count(); ++row)
            {
                auto* item = requests_->item(row);
                if (item->data(Qt::UserRole).toLongLong() == user) { item->setIcon(avatar_icon(item->text(), 32, avatars_->image(user))); }
            }
        });
    }
    members_layout->addWidget(list_, 1);
    status_ = new QLabel(QStringLiteral("正在加载成员…"), this);
    status_->setObjectName(QStringLiteral("groupStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    admin_button_ = new QPushButton(QStringLiteral("设为管理员"), this);
    admin_button_->setObjectName(QStringLiteral("groupAdminButton"));
    admin_button_->setAutoDefault(false);
    transfer_button_ = new QPushButton(QStringLiteral("转让群主"), this);
    transfer_button_->setObjectName(QStringLiteral("groupTransferButton"));
    transfer_button_->setAutoDefault(false);
    auto* roles_row = new QHBoxLayout;
    roles_row->addWidget(admin_button_);
    roles_row->addWidget(transfer_button_);
    members_layout->addLayout(roles_row);
    remove_button_ = new QPushButton(QStringLiteral("移除成员"), this);
    remove_button_->setObjectName(QStringLiteral("groupRemoveButton"));
    remove_button_->setAutoDefault(false);
    invite_button_ = new QPushButton(QStringLiteral("从联系人邀请"), this);
    invite_button_->setObjectName(QStringLiteral("groupInviteButton"));
    invite_button_->setAutoDefault(false);
    auto* members_row = new QHBoxLayout;
    members_row->addWidget(remove_button_);
    members_row->addWidget(invite_button_);
    members_layout->addLayout(members_row);
    leave_button_ = new QPushButton(QStringLiteral("退出群聊"), this);
    leave_button_->setObjectName(QStringLiteral("groupLeaveButton"));
    leave_button_->setAutoDefault(false);
    members_layout->addWidget(leave_button_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list_, &QListWidget::currentRowChanged, this, [this] { update_actions(); });
    connect(requests_, &QListWidget::currentRowChanged, this, [this] { update_actions(); });
    connect(approval_, &QCheckBox::clicked, this, [this](bool required) {
        approval_->setChecked(!required);
        pending_ = true;
        update_actions();
        emit approval_requested(required);
    });
    connect(accept_request_button_, &QPushButton::clicked, this, [this] {
        auto const user = requests_->currentItem()->data(Qt::UserRole).toLongLong();
        pending_ = true;
        update_actions();
        emit request_response_requested(user, true);
    });
    connect(reject_request_button_, &QPushButton::clicked, this, [this] {
        auto const user = requests_->currentItem()->data(Qt::UserRole).toLongLong();
        pending_ = true;
        update_actions();
        emit request_response_requested(user, false);
    });
    connect(more_requests_button_, &QPushButton::clicked, this, [this] {
        pending_ = true;
        update_actions();
        emit requests_requested(next_request_);
    });
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
    connect(announcement_edit_, &QPlainTextEdit::textChanged, this, [this] { update_actions(); });
    connect(create_invite_button_, &QPushButton::clicked, this, [this] {
        pending_ = true;
        update_actions();
        emit invite_link_requested(true);
    });
    connect(revoke_invite_button_, &QPushButton::clicked, this, [this] {
        pending_ = true;
        update_actions();
        emit invite_link_requested(false);
    });
    connect(copy_invite_button_, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(invite_edit_->text());
    });
    connect(announcement_button_, &QPushButton::clicked, this, [this] {
        pending_ = true;
        update_actions();
        emit announcement_requested(announcement_edit_->toPlainText());
    });
    connect(clear_announcement_button_, &QPushButton::clicked, this, [this] {
        announcement_edit_->clear();
        pending_ = true;
        update_actions();
        emit announcement_requested({});
    });
    connect(transfer_button_, &QPushButton::clicked, this, [this] {
        auto const row = list_->currentRow();
        auto const target = members_[row];
        if (QMessageBox::question(this, QStringLiteral("转让群主"),
            QStringLiteral("将群主转让给 %1？你将成为管理员，之后可以退出群聊。").arg(target.username),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        {
            pending_ = true;
            update_actions();
            emit transfer_requested(target.id);
        }
    });
    connect(remove_button_, &QPushButton::clicked, this, [this] {
        auto const row = list_->currentRow();
        auto const target = members_[row];
        if (QMessageBox::question(this, QStringLiteral("移除成员"),
            QStringLiteral("将 %1 移出群聊？对方将无法继续访问群，重新加入需要邀请。").arg(target.username),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        {
            pending_ = true;
            update_actions();
            emit remove_requested(target.id);
        }
    });
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
        if (avatars_) { avatars_->observe(member.id, member.avatar); }
        auto* item = new QListWidgetItem(member.username + QStringLiteral(" · ") + role, list_);
        item->setData(Qt::UserRole, member.id);
        item->setIcon(avatar_icon(member.username, 32, avatars_ ? avatars_->image(member.id) : QPixmap{}));
        if (member.id == selected)
        {
            list_->setCurrentItem(item);
        }
    }
    available_ = true;
    status_->setText(QStringLiteral("共 %1 名成员；最多 3 名管理员，群主不计入。只有群主能任免管理员。").arg(members_.size()));
    update_actions();
    auto const self = std::find_if(members_.begin(), members_.end(), [this](auto const& member) { return member.id == self_user_; });
    if (self != members_.end() && self->role != chat::member_role::member)
    {
        emit invite_link_requested(std::nullopt);
        refresh_requests();
    }
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
        setWindowTitle(title_ + QStringLiteral(" · 群资料"));
    }
    if (announcement_ != found->announcement)
    {
        auto const modified = announcement_edit_->toPlainText() != announcement_;
        announcement_ = found->announcement;
        if (!modified) { announcement_edit_->setPlainText(announcement_); }
    }
    approval_->setChecked(found->join_approval);
    update_actions();
}

void group_dialog::set_error(QString const& error)
{
    available_ = false;
    pending_ = false;
    status_->setText(error);
    update_actions();
}

void group_dialog::set_invite(qint64 conversation, QString const& token, QString const& error)
{
    if (conversation != conversation_) { return; }
    auto const self = std::find_if(members_.begin(), members_.end(), [this](auto const& member) { return member.id == self_user_; });
    if (self == members_.end() || self->role == chat::member_role::member) { return; }
    if (!error.isEmpty()) { status_->setText(error); return; }
    invite_edit_->setText(token.isEmpty() ? QString{} : QStringLiteral("chat://join/") + token);
    invite_edit_->setCursorPosition(0);
    update_actions();
}

void group_dialog::set_requests(qint64 conversation, QList<user_data> users, qint64 next, bool older, QString const& error)
{
    if (conversation != conversation_) { return; }
    if (older) { pending_ = false; }
    auto const self = std::find_if(members_.begin(), members_.end(), [this](auto const& member) { return member.id == self_user_; });
    if (self == members_.end() || self->role == chat::member_role::member) { update_actions(); return; }
    if (!error.isEmpty()) { status_->setText(error); update_actions(); return; }
    if (!older) { requests_->clear(); }
    for (auto const& user : users)
    {
        if (avatars_) { avatars_->observe(user.id, user.avatar); }
        auto* item = new QListWidgetItem(user.username, requests_);
        item->setData(Qt::UserRole, user.id);
        item->setIcon(avatar_icon(user.username, 32, avatars_ ? avatars_->image(user.id) : QPixmap{}));
    }
    next_request_ = next;
    tabs_->setTabText(1, QStringLiteral("入群申请 (%1%2)").arg(requests_->count()).arg(next ? QStringLiteral("+") : QString{}));
    update_actions();
}

void group_dialog::refresh_requests()
{
    next_request_ = 0;
    update_actions();
    emit requests_requested(0);
}

void group_dialog::update_actions()
{
    auto const self = std::find_if(members_.begin(), members_.end(), [this](auto const& member) {
        return member.id == self_user_;
    });
    auto const owner = self != members_.end() && self->role == chat::member_role::owner;
    auto const manager = self != members_.end() && self->role != chat::member_role::member;
    auto const enabled = available_ && !pending_;
    invite_controls_->setVisible(manager);
    approval_->setEnabled(enabled && manager);
    tabs_->setTabVisible(1, manager);
    if (!manager)
    {
        next_request_ = 0;
        if (requests_->count() > 0) { QSignalBlocker blocked(requests_); requests_->clear(); }
    }
    accept_request_button_->setEnabled(enabled && manager && requests_->currentItem());
    reject_request_button_->setEnabled(enabled && manager && requests_->currentItem());
    more_requests_button_->setEnabled(enabled && manager && next_request_ > 0);
    if (!manager) { invite_edit_->clear(); }
    create_invite_button_->setEnabled(enabled && manager && invite_edit_->text().isEmpty());
    copy_invite_button_->setEnabled(enabled && manager && !invite_edit_->text().isEmpty());
    revoke_invite_button_->setEnabled(enabled && manager && !invite_edit_->text().isEmpty());
    title_edit_->setEnabled(enabled && manager);
    if (!manager && announcement_edit_->toPlainText() != announcement_) { announcement_edit_->setPlainText(announcement_); }
    announcement_edit_->setReadOnly(!enabled || !manager);
    auto const announcement = announcement_edit_->toPlainText();
    announcement_button_->setEnabled(enabled && manager && announcement != announcement_ && announcement.toUtf8().size() <= 4096);
    announcement_button_->setToolTip(announcement.toUtf8().size() > 4096 ? QStringLiteral("公告超过 4 KiB，请缩短后保存。") : QString{});
    clear_announcement_button_->setEnabled(enabled && manager && !announcement_.isEmpty());
    rename_button_->setEnabled(enabled && manager && !title_edit_->text().trimmed().isEmpty() &&
        title_edit_->text().trimmed() != title_);
    invite_button_->setEnabled(enabled && manager && contacts_ready_);
    leave_button_->setEnabled(enabled && self != members_.end() && !owner);
    leave_button_->setToolTip(owner ? QStringLiteral("请先将群主转让给一位管理员，再退出群聊。") : QString{});
    auto const admins = std::count_if(members_.begin(), members_.end(), [](auto const& member) {
        return member.role == chat::member_role::admin;
    });
    auto const row = list_->currentRow();
    auto const selected = row >= 0 && row < members_.size();
    auto const admin = selected && members_[row].role == chat::member_role::admin;
    admin_button_->setText(admin ? QStringLiteral("取消管理员") : QStringLiteral("设为管理员"));
    admin_button_->setEnabled(available_ && !pending_ && owner && selected &&
        members_[row].role != chat::member_role::owner && (admin || admins < 3));
    transfer_button_->setEnabled(enabled && owner && selected && admin);
    remove_button_->setEnabled(enabled && manager && selected && members_[row].id != self_user_ &&
        members_[row].role != chat::member_role::owner && (owner || !admin));
}
