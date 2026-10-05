#include "group_dialog.hpp"
#include <chat/text.hpp>
#include "avatar.hpp"
#include "theme.hpp"
#include "user_delegate.hpp"

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
#include <QScrollArea>
#include <QKeyEvent>
#include <QScrollBar>
#include <QTimer>

#include <algorithm>

group_dialog::group_dialog(qint64 conversation, qint64 self_user, QString const& title, QString const& announcement, bool join_approval,
                           QWidget* parent, avatar_cache* avatars)
    : QDialog(parent), avatars_(avatars), conversation_(conversation), self_user_(self_user), title_(title),
      announcement_(announcement)
{
    setObjectName(QStringLiteral("groupDialog"));
    setWindowTitle(title + QStringLiteral(" · 群资料"));
    resize(520, 680);
    setMinimumSize(420, 400);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(chat_theme::dialog_spacing);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("groupTabs"));
    outer->addWidget(tabs_, 1);
    auto* overview_scroll = new QScrollArea(tabs_);
    overview_scroll->setObjectName(QStringLiteral("groupOverviewScroll"));
    overview_scroll->setWidgetResizable(true);
    overview_scroll->setFrameShape(QFrame::NoFrame);
    overview_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* overview = new QWidget(overview_scroll);
    overview_scroll->setWidget(overview);
    auto* overview_layout = new QVBoxLayout(overview);
    overview_layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                                       chat_theme::dialog_padding, chat_theme::dialog_padding);
    overview_layout->setSpacing(chat_theme::dialog_spacing);
    auto* identity = new QHBoxLayout;
    identity->setSpacing(16);
    overview_avatar_ = new QLabel(overview);
    overview_avatar_->setObjectName(QStringLiteral("groupOverviewAvatar"));
    overview_avatar_->setFixedSize(64, 64);
    overview_avatar_->setPixmap(avatar_icon(title, 64).pixmap(64));
    identity->addWidget(overview_avatar_);
    auto* identity_text = new QVBoxLayout;
    identity_text->setSpacing(4);
    overview_title_ = new QLabel(title, overview);
    overview_title_->setObjectName(QStringLiteral("groupOverviewTitle"));
    overview_title_->setTextFormat(Qt::PlainText);
    overview_title_->setWordWrap(true);
    overview_title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identity_text->addWidget(overview_title_);
    overview_count_ = new QLabel(QStringLiteral("正在加载成员…"), overview);
    overview_count_->setObjectName(QStringLiteral("groupOverviewCount"));
    identity_text->addWidget(overview_count_);
    identity->addLayout(identity_text, 1);
    overview_layout->addLayout(identity);
    auto* announcement_heading = new QLabel(QStringLiteral("群公告"), overview);
    announcement_heading->setObjectName(QStringLiteral("groupSectionHeading"));
    overview_layout->addWidget(announcement_heading);
    overview_announcement_ = new QLabel(announcement.isEmpty() ? QStringLiteral("暂无公告") : announcement, overview);
    overview_announcement_->setObjectName(QStringLiteral("groupOverviewAnnouncement"));
    overview_announcement_->setTextFormat(Qt::PlainText);
    overview_announcement_->setWordWrap(true);
    overview_announcement_->setMaximumHeight(120);
    overview_announcement_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    overview_layout->addWidget(overview_announcement_);
    read_announcement_button_ = new QPushButton(QStringLiteral("查看完整公告"), overview);
    read_announcement_button_->setObjectName(QStringLiteral("groupReadAnnouncementButton"));
    read_announcement_button_->setVisible(!announcement.isEmpty());
    overview_layout->addWidget(read_announcement_button_, 0, Qt::AlignLeft);
    connect(read_announcement_button_, &QPushButton::clicked, this, [this] {
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("groupAnnouncementDialog"));
        dialog.setWindowTitle(QStringLiteral("群公告"));
        dialog.resize(480, 400);
        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                                   chat_theme::dialog_padding, chat_theme::dialog_padding);
        layout->setSpacing(chat_theme::dialog_spacing);
        auto* text = new QPlainTextEdit(announcement_, &dialog);
        text->setReadOnly(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        layout->addWidget(text);
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
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
    overview_layout->addWidget(preview_);
    auto* overview_actions = new QHBoxLayout;
    overview_actions->setSpacing(chat_theme::dialog_spacing);
    auto* all_members = new QPushButton(QStringLiteral("查看全部成员"), overview);
    all_members->setObjectName(QStringLiteral("groupAllMembersButton"));
    overview_actions->addWidget(all_members);
    overview_actions->addStretch();
    connect(all_members, &QPushButton::clicked, this, [this] { tabs_->setCurrentIndex(1); });
    manage_button_ = new QPushButton(QStringLiteral("群管理"), overview);
    manage_button_->setObjectName(QStringLiteral("groupManageButton"));
    overview_actions->addWidget(manage_button_);
    overview_layout->addLayout(overview_actions);
    connect(manage_button_, &QPushButton::clicked, this, [this] { tabs_->setCurrentIndex(3); });
    overview_layout->addStretch();
    tabs_->addTab(overview_scroll, QStringLiteral("群资料"));
    auto* management_scroll = new QScrollArea(tabs_);
    management_scroll->setObjectName(QStringLiteral("groupManagementScroll"));
    management_scroll->setWidgetResizable(true);
    management_scroll->setFrameShape(QFrame::NoFrame);
    management_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* management = new QWidget(management_scroll);
    management_scroll->setWidget(management);
    auto* layout = new QVBoxLayout(management);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                              chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    auto* name_heading = new QLabel(QStringLiteral("群名称"), management);
    name_heading->setObjectName(QStringLiteral("groupSectionHeading"));
    layout->addWidget(name_heading);
    auto* name_row = new QHBoxLayout;
    title_edit_ = new QLineEdit(title, this);
    title_edit_->setObjectName(QStringLiteral("groupTitleEdit"));
    title_edit_->setPlaceholderText(QStringLiteral("群名称"));
    title_edit_->setAccessibleName(QStringLiteral("群名称"));
    name_row->addWidget(title_edit_, 1);
    rename_button_ = new QPushButton(QStringLiteral("保存群名"), this);
    rename_button_->setObjectName(QStringLiteral("groupRenameButton"));
    rename_button_->setAutoDefault(false);
    name_row->addWidget(rename_button_);
    layout->addLayout(name_row);
    auto* edit_announcement_heading = new QLabel(QStringLiteral("群公告 · 纯文本，最多 4 KiB"), management);
    edit_announcement_heading->setObjectName(QStringLiteral("groupSectionHeading"));
    layout->addWidget(edit_announcement_heading);
    announcement_edit_ = new QPlainTextEdit(announcement, this);
    announcement_edit_->setObjectName(QStringLiteral("groupAnnouncementEdit"));
    announcement_edit_->setPlaceholderText(QStringLiteral("暂无公告"));
    announcement_edit_->setAccessibleName(QStringLiteral("群公告"));
    announcement_edit_->setFixedHeight(112);
    layout->addWidget(announcement_edit_);
    auto* announcement_row = new QHBoxLayout;
    announcement_row->setSpacing(chat_theme::dialog_spacing);
    announcement_row->addStretch();
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
    invite_layout->setSpacing(chat_theme::dialog_spacing);
    auto* invite_heading = new QLabel(QStringLiteral("邀请链接"), invite_controls_);
    invite_heading->setObjectName(QStringLiteral("groupSectionHeading"));
    invite_layout->addWidget(invite_heading);
    approval_ = new QCheckBox(QStringLiteral("通过邀请链接加入需要审批"), invite_controls_);
    approval_->setObjectName(QStringLiteral("groupJoinApprovalCheck"));
    approval_->setChecked(join_approval);
    invite_layout->addWidget(approval_);
    invite_edit_ = new QLineEdit(invite_controls_);
    invite_edit_->setObjectName(QStringLiteral("groupInviteLinkEdit"));
    invite_edit_->setReadOnly(true);
    invite_edit_->setPlaceholderText(QStringLiteral("尚无邀请链接"));
    invite_edit_->setAccessibleName(QStringLiteral("群邀请链接"));
    invite_layout->addWidget(invite_edit_);
    auto* invite_row = new QHBoxLayout;
    invite_row->setSpacing(chat_theme::dialog_spacing);
    invite_row->addStretch();
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
    layout->addStretch();
    auto* members_page = new QWidget(tabs_);
    auto* members_layout = new QVBoxLayout(members_page);
    members_layout->setContentsMargins(0, 0, 0, 0);
    members_layout->setSpacing(chat_theme::dialog_spacing);
    tabs_->addTab(members_page, QStringLiteral("成员"));
    auto* requests_page = new QWidget(tabs_);
    auto* requests_layout = new QVBoxLayout(requests_page);
    requests_layout->setContentsMargins(0, 0, 0, 0);
    requests_status_ = new QLabel(QStringLiteral("正在加载申请…"), requests_page);
    requests_status_->setObjectName(QStringLiteral("groupJoinRequestsStatus"));
    requests_status_->setWordWrap(true);
    requests_status_->setContentsMargins(chat_theme::dialog_padding, 16, chat_theme::dialog_padding, 8);
    requests_layout->addWidget(requests_status_);
    requests_ = new QListWidget(requests_page);
    requests_->setObjectName(QStringLiteral("groupJoinRequestsList"));
    requests_layout->addWidget(requests_, 1);
    auto* decisions = new QHBoxLayout;
    decisions->setContentsMargins(16, 0, 16, 0);
    decisions->setSpacing(chat_theme::dialog_spacing);
    decisions->addStretch();
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
    tabs_->addTab(management_scroll, QStringLiteral("管理"));
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("groupMembersList"));
    list_->setMinimumHeight(120);
    for (auto* list : {list_, preview_, requests_})
    {
        auto* delegate = new user_delegate(list);
        list->setItemDelegate(delegate);
        list->setUniformItemSizes(true);
        list->setFrameShape(QFrame::NoFrame);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setMouseTracking(true);
        list->installEventFilter(this);
        list->viewport()->installEventFilter(this);
        if (list != preview_)
        {
            connect(delegate, &user_delegate::avatar_clicked, this, [this, list](QModelIndex const& index) {
                auto* item = list->item(index.row());
                emit user_requested(item->data(Qt::UserRole).toLongLong(), item->text());
            });
        }
    }
    list_->setAccessibleName(QStringLiteral("群成员"));
    preview_->setAccessibleName(QStringLiteral("群成员预览"));
    requests_->setAccessibleName(QStringLiteral("待处理入群申请"));
    if (avatars_)
    {
        connect(avatars_, &avatar_cache::changed, this, [this](qint64 user) {
            for (auto* list : {list_, preview_, requests_})
            {
                for (int row = 0; row < list->count(); ++row)
                {
                    auto* item = list->item(row);
                    if (item->data(Qt::UserRole).toLongLong() == user)
                    { item->setData(Qt::DecorationRole, avatars_->image(user)); }
                }
            }
        });
    }
    auto* member_hint = new QLabel(QStringLiteral("群主和管理员排列在前；右键查看成员操作。最多 3 名管理员，由群主任免。"), members_page);
    member_hint->setObjectName(QStringLiteral("groupDetailHint"));
    member_hint->setContentsMargins(chat_theme::dialog_padding, 16, chat_theme::dialog_padding, 8);
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
    overview_layout->addWidget(leave_button_, 0, Qt::AlignLeft);
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
        layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                                   chat_theme::dialog_padding, chat_theme::dialog_padding);
        layout->setSpacing(chat_theme::dialog_spacing);
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

bool group_dialog::eventFilter(QObject* object, QEvent* event)
{
    if (event->type() == QEvent::Resize)
    {
        for (auto* list : {list_, preview_, requests_})
        {
            if (object == list->viewport())
            {
                QTimer::singleShot(0, list, [list] {
                    if (list->hasFocus() && list->currentItem()) { list->scrollToItem(list->currentItem()); }
                });
            }
        }
    }
    if (event->type() == QEvent::KeyPress && (object == list_ || object == preview_ || object == requests_))
    {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            auto* item = static_cast<QListWidget*>(object)->currentItem();
            if (item) { emit user_requested(item->data(Qt::UserRole).toLongLong(), item->text()); }
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
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
        auto const description = role + (member.id == self_user_ ? QStringLiteral(" · 你") : QString{});
        auto* item = new QListWidgetItem(member.username, list_);
        item->setData(Qt::UserRole, member.id);
        item->setData(Qt::StatusTipRole, description);
        item->setData(Qt::DecorationRole, avatars_ ? avatars_->image(member.id) : QPixmap{});
        item->setToolTip(member.username + QStringLiteral(" · ") + description);
        if (preview_->count() < 3)
        {
            auto* preview = new QListWidgetItem(*item);
            preview_->addItem(preview);
        }
        if (member.id == selected)
        {
            list_->setCurrentItem(item);
        }
    }
    preview_->setFixedHeight(preview_->count() * chat_theme::dialog_row_height);
    available_ = true;
    status_->clear();
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
    status_->setText(error);
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
        overview_avatar_->setPixmap(avatar_icon(title_, 64).pixmap(64));
        setWindowTitle(title_ + QStringLiteral(" · 群资料"));
    }
    if (announcement_ != found->announcement)
    {
        auto const modified = announcement_edit_->toPlainText() != announcement_;
        announcement_ = found->announcement;
        if (!modified) { announcement_edit_->setPlainText(announcement_); }
    }
    overview_announcement_->setText(announcement_.isEmpty() ? QStringLiteral("暂无公告") : announcement_);
    read_announcement_button_->setVisible(!announcement_.isEmpty());
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
    if (!error.isEmpty()) { status_->setText(error); status_->show(); return; }
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
    if (!error.isEmpty())
    {
        status_->setText(error);
        requests_status_->setText(requests_->count() == 0 ? QStringLiteral("无法加载申请") : QString{});
        update_actions();
        return;
    }
    auto const selected = requests_->currentItem() ? requests_->currentItem()->data(Qt::UserRole).toLongLong() : 0;
    auto const selected_visible = requests_->currentItem() &&
        requests_->viewport()->rect().intersects(requests_->visualItemRect(requests_->currentItem()));
    auto const scroll_position = requests_->verticalScrollBar()->value();
    if (!older) { requests_->clear(); }
    for (auto const& user : users)
    {
        if (avatars_) { avatars_->observe(user.id, user.avatar); }
        auto* item = new QListWidgetItem(user.username, requests_);
        item->setData(Qt::UserRole, user.id);
        item->setData(Qt::StatusTipRole, QStringLiteral("待审批"));
        item->setData(Qt::DecorationRole, avatars_ ? avatars_->image(user.id) : QPixmap{});
        item->setToolTip(user.username);
        if (user.id == selected) { requests_->setCurrentItem(item); }
    }
    requests_->doItemsLayout();
    requests_->verticalScrollBar()->setValue(scroll_position);
    if (selected_visible && requests_->currentItem()) { requests_->scrollToItem(requests_->currentItem()); }
    next_request_ = next;
    requests_status_->setText(requests_->count() == 0 ? QStringLiteral("暂无待处理申请") : QString{});
    tabs_->setTabText(2, QStringLiteral("入群申请 (%1%2)").arg(requests_->count()).arg(next ? QStringLiteral("+") : QString{}));
    update_actions();
}

void group_dialog::refresh_requests()
{
    next_request_ = 0;
    requests_status_->setText(QStringLiteral("正在加载申请…"));
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
    if (pending_) { status_->setText(QStringLiteral("正在处理…")); }
    status_->setVisible(!status_->text().isEmpty());
    invite_controls_->setVisible(manager);
    approval_->setEnabled(enabled && manager);
    tabs_->setTabVisible(2, manager);
    tabs_->setTabVisible(3, manager);
    manage_button_->setVisible(manager);
    overview_invite_->setVisible(manager);
    overview_invite_->setText(invite_edit_->text().isEmpty() ? QStringLiteral("邀请链接 · 尚未创建") : QStringLiteral("邀请链接 · 已创建"));
    if (!manager)
    {
        next_request_ = 0;
        if (requests_->count() > 0) { QSignalBlocker blocked(requests_); requests_->clear(); }
    }
    accept_request_button_->setEnabled(enabled && manager && requests_->currentItem());
    reject_request_button_->setEnabled(enabled && manager && requests_->currentItem());
    more_requests_button_->setEnabled(enabled && manager && next_request_ > 0);
    accept_request_button_->setVisible(manager && requests_->count() > 0);
    reject_request_button_->setVisible(manager && requests_->count() > 0);
    more_requests_button_->setVisible(manager && next_request_ > 0);
    requests_status_->setVisible(manager && !requests_status_->text().isEmpty());
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
    leave_button_->setVisible(self != members_.end() && !owner);
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
