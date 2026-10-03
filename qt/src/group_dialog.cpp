#include "group_dialog.hpp"
#include <chat/text.hpp>
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
#include <QMenu>

#include <algorithm>

group_dialog::group_dialog(qint64 conversation, qint64 self_user, QString const& title, QString const& announcement, bool join_approval,
                           QWidget* parent, avatar_cache* avatars)
    : QDialog(parent), avatars_(avatars), conversation_(conversation), self_user_(self_user), title_(title),
      announcement_(announcement)
{
    setObjectName(QStringLiteral("groupDialog"));
    setWindowTitle(title + QStringLiteral(" · 群资料"));
    resize(520, 680);
    auto* outer = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("groupTabs"));
    outer->addWidget(tabs_, 1);
    auto* overview = new QWidget(tabs_);
    auto* overview_layout = new QVBoxLayout(overview);
    overview_title_ = new QLabel(title, overview);
    overview_title_->setObjectName(QStringLiteral("groupOverviewTitle"));
    overview_title_->setTextFormat(Qt::PlainText);
    overview_title_->setWordWrap(true);
    overview_title_->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 600;"));
    overview_layout->addWidget(overview_title_);
    overview_count_ = new QLabel(QStringLiteral("正在加载成员…"), overview);
    overview_count_->setObjectName(QStringLiteral("groupOverviewCount"));
    overview_layout->addWidget(overview_count_);
    overview_layout->addWidget(new QLabel(QStringLiteral("群公告"), overview));
    overview_announcement_ = new QLabel(announcement.isEmpty() ? QStringLiteral("暂无公告") : announcement, overview);
    overview_announcement_->setObjectName(QStringLiteral("groupOverviewAnnouncement"));
    overview_announcement_->setTextFormat(Qt::PlainText);
    overview_announcement_->setWordWrap(true);
    overview_announcement_->setMaximumHeight(120);
    overview_announcement_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    overview_layout->addWidget(overview_announcement_);
    auto* full_announcement = new QPushButton(QStringLiteral("查看完整公告"), overview);
    full_announcement->setObjectName(QStringLiteral("groupReadAnnouncementButton"));
    overview_layout->addWidget(full_announcement);
    connect(full_announcement, &QPushButton::clicked, this, [this] {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("群公告"));
        dialog.resize(480, 400);
        auto* layout = new QVBoxLayout(&dialog);
        auto* text = new QPlainTextEdit(announcement_, &dialog);
        text->setReadOnly(true);
        layout->addWidget(text);
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(close);
        dialog.exec();
    });
    overview_pin_ = new QLabel(QStringLiteral("暂无置顶消息"), overview);
    overview_pin_->setObjectName(QStringLiteral("groupOverviewPinned"));
    overview_pin_->setTextFormat(Qt::PlainText);
    overview_pin_->setWordWrap(true);
    overview_layout->addWidget(overview_pin_);
    overview_invite_ = new QLabel(overview);
    overview_invite_->setObjectName(QStringLiteral("groupOverviewInvite"));
    overview_invite_->setTextFormat(Qt::PlainText);
    overview_invite_->setWordWrap(true);
    overview_invite_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    overview_layout->addWidget(overview_invite_);
    preview_ = new QListWidget(overview);
    preview_->setObjectName(QStringLiteral("groupMemberPreview"));
    preview_->setIconSize(QSize(32, 32));
    preview_->setMaximumHeight(210);
    overview_layout->addWidget(preview_);
    auto* all_members = new QPushButton(QStringLiteral("查看全部成员"), overview);
    all_members->setObjectName(QStringLiteral("groupAllMembersButton"));
    overview_layout->addWidget(all_members);
    connect(all_members, &QPushButton::clicked, this, [this] { tabs_->setCurrentIndex(1); });
    manage_button_ = new QPushButton(QStringLiteral("群管理"), overview);
    manage_button_->setObjectName(QStringLiteral("groupManageButton"));
    overview_layout->addWidget(manage_button_);
    connect(manage_button_, &QPushButton::clicked, this, [this] { tabs_->setCurrentIndex(3); });
    overview_layout->addStretch();
    tabs_->addTab(overview, QStringLiteral("群资料"));
    auto* management = new QWidget(tabs_);
    auto* layout = new QVBoxLayout(management);
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
    tabs_->addTab(management, QStringLiteral("管理"));
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
    auto* member_hint = new QLabel(QStringLiteral("群主 · 管理员 · 成员，按角色排列；右键查看成员操作"), members_page);
    member_hint->setWordWrap(true);
    members_layout->addWidget(member_hint);
    members_layout->addWidget(list_, 1);
    status_ = new QLabel(QStringLiteral("正在加载成员…"), this);
    status_->setObjectName(QStringLiteral("groupStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    outer->addWidget(status_);
    admin_button_ = new QPushButton(QStringLiteral("设为管理员"), this);
    admin_button_->setObjectName(QStringLiteral("groupAdminButton"));
    admin_button_->setAutoDefault(false);
    transfer_button_ = new QPushButton(QStringLiteral("转让群主"), this);
    transfer_button_->setObjectName(QStringLiteral("groupTransferButton"));
    transfer_button_->setAutoDefault(false);
    admin_button_->hide();
    transfer_button_->hide();
    remove_button_ = new QPushButton(QStringLiteral("移除成员"), this);
    remove_button_->setObjectName(QStringLiteral("groupRemoveButton"));
    remove_button_->setAutoDefault(false);
    invite_button_ = new QPushButton(QStringLiteral("从联系人邀请"), this);
    invite_button_->setObjectName(QStringLiteral("groupInviteButton"));
    invite_button_->setAutoDefault(false);
    remove_button_->hide();
    members_layout->addWidget(invite_button_);
    leave_button_ = new QPushButton(QStringLiteral("退出群聊"), this);
    leave_button_->setObjectName(QStringLiteral("groupLeaveButton"));
    leave_button_->setAutoDefault(false);
    overview_layout->addWidget(leave_button_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list_, &QListWidget::currentRowChanged, this, [this] { update_actions(); });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        if (auto* item = list_->itemAt(point)) { list_->setCurrentItem(item); }
        if (!list_->currentItem()) { return; }
        update_actions();
        QMenu menu(this);
        auto const target = members_[list_->currentRow()].id;
        menu.addAction(QStringLiteral("个人资料"), [this, target] {
            auto found = std::find_if(members_.begin(), members_.end(), [target](auto const& member) { return member.id == target; });
            if (found != members_.end()) { emit user_requested(found->id, found->username); }
        });
        for (auto* button : {admin_button_, transfer_button_, remove_button_})
        {
            if (button->isEnabled())
            {
                menu.addAction(button->text(), [this, target, button] {
                    auto found = std::find_if(members_.begin(), members_.end(), [target](auto const& member) { return member.id == target; });
                    if (found == members_.end()) { return; }
                    list_->setCurrentRow(static_cast<int>(std::distance(members_.begin(), found)));
                    update_actions();
                    if (button->isEnabled()) { button->click(); }
                });
            }
        }
        menu.exec(list_->viewport()->mapToGlobal(point));
    });
    connect(preview_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        auto const id = item->data(Qt::UserRole).toLongLong();
        auto found = std::find_if(members_.begin(), members_.end(), [id](auto const& member) { return member.id == id; });
        if (found != members_.end()) { emit user_requested(found->id, found->username); }
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        auto const row = list_->row(item);
        if (row >= 0 && row < members_.size()) { emit user_requested(members_[row].id, members_[row].username); }
    });
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
        auto const text = announcement_edit_->toPlainText();
        if (chat::text_is_blank(text.toUtf8().toStdString()))
        {
            announcement_edit_->clear();
            emit announcement_requested({});
        }
        else
        {
            emit announcement_requested(text);
        }
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
        emit rename_requested(title_edit_->text());
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
    std::stable_sort(members_.begin(), members_.end(), [](auto const& a, auto const& b) {
        return static_cast<int>(a.role) > static_cast<int>(b.role);
    });
    list_->clear();
    preview_->clear();
    overview_count_->setText(QStringLiteral("%1 位成员").arg(members_.size()));
    for (auto const& member : members_)
    {
        auto const role = member.role == chat::member_role::owner ? QStringLiteral("群主") :
            member.role == chat::member_role::admin ? QStringLiteral("管理员") : QStringLiteral("成员");
        if (avatars_) { avatars_->observe(member.id, member.avatar); }
        auto* item = new QListWidgetItem(member.username + QStringLiteral(" · ") + role +
            (member.id == self_user_ ? QStringLiteral(" · 你") : QString{}), list_);
        item->setData(Qt::UserRole, member.id);
        item->setIcon(avatar_icon(member.username, 32, avatars_ ? avatars_->image(member.id) : QPixmap{}));
        if (preview_->count() < 5)
        {
            auto* preview = new QListWidgetItem(item->icon(), item->text(), preview_);
            preview->setData(Qt::UserRole, member.id);
        }
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
        overview_title_->setText(title_);
        setWindowTitle(title_ + QStringLiteral(" · 群资料"));
    }
    if (announcement_ != found->announcement)
    {
        auto const modified = announcement_edit_->toPlainText() != announcement_;
        announcement_ = found->announcement;
        if (!modified) { announcement_edit_->setPlainText(announcement_); }
    }
    overview_announcement_->setText(announcement_.isEmpty() ? QStringLiteral("暂无公告") : announcement_);
    overview_pin_->setText(found->pinned_message.id > 0
        ? QStringLiteral("置顶消息 · %1：%2").arg(found->pinned_message.username,
            found->pinned_message.deleted ? QStringLiteral("消息已删除") : found->pinned_message.text)
        : QStringLiteral("暂无置顶消息"));
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
    tabs_->setTabText(2, QStringLiteral("入群申请 (%1%2)").arg(requests_->count()).arg(next ? QStringLiteral("+") : QString{}));
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
    tabs_->setTabVisible(2, manager);
    tabs_->setTabVisible(3, manager);
    manage_button_->setVisible(manager);
    overview_invite_->setVisible(manager);
    overview_invite_->setText(invite_edit_->text().isEmpty() ? QStringLiteral("邀请链接 · 尚未创建") : invite_edit_->text());
    if (!manager)
    {
        next_request_ = 0;
        if (requests_->count() > 0) { QSignalBlocker blocked(requests_); requests_->clear(); }
    }
    accept_request_button_->setEnabled(enabled && manager && requests_->currentItem());
    reject_request_button_->setEnabled(enabled && manager && requests_->currentItem());
    more_requests_button_->setEnabled(enabled && manager && next_request_ > 0);
    if (!manager) { invite_edit_->clear(); overview_invite_->clear(); }
    create_invite_button_->setEnabled(enabled && manager && invite_edit_->text().isEmpty());
    copy_invite_button_->setEnabled(enabled && manager && !invite_edit_->text().isEmpty());
    revoke_invite_button_->setEnabled(enabled && manager && !invite_edit_->text().isEmpty());
    title_edit_->setEnabled(enabled && manager);
    if (!manager && announcement_edit_->toPlainText() != announcement_) { announcement_edit_->setPlainText(announcement_); }
    announcement_edit_->setReadOnly(!enabled || !manager);
    auto const announcement = announcement_edit_->toPlainText();
    auto const announcement_bytes = announcement.toUtf8();
    auto const announcement_value = chat::text_is_blank(announcement_bytes.toStdString()) ? QString{} : announcement;
    announcement_button_->setEnabled(enabled && manager && announcement_value != announcement_ &&
        announcement_bytes.size() <= 4096 && !announcement.contains(QChar{}));
    announcement_button_->setToolTip(announcement.toUtf8().size() > 4096 ? QStringLiteral("公告超过 4 KiB，请缩短后保存。") : QString{});
    clear_announcement_button_->setEnabled(enabled && manager && !announcement_.isEmpty());
    rename_button_->setEnabled(enabled && manager && chat::valid_group_title(title_edit_->text().toUtf8().toStdString()) &&
        title_edit_->text() != title_);
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
