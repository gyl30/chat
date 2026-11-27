#include "profile_controls.hpp"
#include "empty_state.hpp"
#include "group_dialog.hpp"
#include "group_files_page.hpp"
#include "create_group_dialog.hpp"
#include "chat_widget.hpp"
#include "presence.hpp"
#include "user_model.hpp"

#include <utility>

#include <chat/invite.hpp>
#include <chat/text.hpp>
#include "avatar.hpp"
#include "theme.hpp"
#include "theme_manager.hpp"
#include "icons.hpp"
#include "user_delegate.hpp"

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
#include <QStackedWidget>
#include <QToolButton>
#include <QSignalBlocker>
#include <QMenu>
#include <QScrollArea>
#include <QKeyEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QTimer>
#include <QInputDialog>

#include <algorithm>
#include <limits>

group_dialog::group_dialog(client_bridge& client,
                           qint64 conversation,
                           qint64 self_user,
                           QString const& title,
                           QString const& announcement,
                           bool join_approval,
                           QWidget* parent,
                           chat_widget& chat)
    : QDialog(parent),
      chat_(chat),
      avatars_(&chat.avatars()),
      conversation_(conversation),
      self_user_(self_user),
      title_(title),
      announcement_(announcement)
{
    setObjectName(QStringLiteral("groupDialog"));
    setWindowTitle(QStringLiteral("群资料"));
    resize(510, 635);
    setMinimumSize(360, 360);
    auto* outer = new QVBoxLayout(this);
    frame_dialog(*this);
    outer->setContentsMargins(0, 0, 0, 8);
    outer->setSpacing(0);
    auto* title_bar = outer->menuBar();
    auto* title_row = qobject_cast<QHBoxLayout*>(title_bar->layout());
    back_ = new QToolButton(title_bar);
    back_->setObjectName(QStringLiteral("groupProfileBack"));
    back_->setAccessibleName(QStringLiteral("返回群资料"));
    back_->setToolTip(back_->accessibleName());
    back_->setFixedSize(32, 32);
    back_->setIconSize(chat_theme::action_icon);
    back_->setProperty("navigationTool", true);
    auto back_icon = [this] { back_->setIcon(svg_icon(u"back", themed("text_secondary"), chat_theme::action_icon)); };
    back_icon();
    connect(&theme_manager::instance(), &theme_manager::changed, this, back_icon);
    title_row->insertWidget(0, back_);
    findChild<QLabel*>(QStringLiteral("dialogTitle"))->setFixedHeight(42);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("groupProfilePages"));
    outer->addWidget(pages_, 1);
    setup_join_requests();
    setup_overview();
    setup_management(join_approval);
    pages_->addWidget(management_scroll_);
    files_ = new group_files_page(client, conversation, self_user, pages_);
    pages_->addWidget(files_);
    files_->bind_uploads(chat_);
    connect(files_, &group_files_page::upload_requested, this, &group_dialog::upload_requested);
    connect(back_, &QToolButton::clicked, this, [this] { show_page(overview_scroll_); });
    connect(pages_,
            &QStackedWidget::currentChanged,
            this,
            [this]
            {
                update_title();
                if (pages_->currentWidget() == files_)
                {
                    files_->show_files();
                }
            });
    connect(overview_scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, &group_dialog::update_title);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("groupStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setAlignment(Qt::AlignCenter);
    outer->addWidget(status_);
    update_title();
    connect_member_actions();
    connect_approval_actions();
    connect_group_settings();
    speech_expiry_ = new QTimer(this);
    speech_expiry_->setSingleShot(true);
    speech_expiry_->setTimerType(Qt::PreciseTimer);
    connect(speech_expiry_,
            &QTimer::timeout,
            this,
            [this]
            {
                refresh_speech();
                update_actions();
            });
    update_actions();
}

void group_dialog::show_page(QWidget* page)
{
    if (pages_->currentWidget() == page)
    {
        return;
    }
    if (pages_->currentWidget() == overview_scroll_)
    {
        auto* focused = focusWidget();
        overview_focus_ = focused && overview_scroll_->isAncestorOf(focused) ? focused : nullptr;
    }
    setFocus(Qt::OtherFocusReason);
    pages_->setCurrentWidget(page);
    if (page == overview_scroll_ && overview_focus_ && overview_focus_->isVisible() && overview_focus_->isEnabled())
    {
        overview_focus_->setFocus(Qt::OtherFocusReason);
    }
}

void group_dialog::update_title()
{
    auto* current = pages_->currentWidget();
    auto* label = findChild<QLabel*>(QStringLiteral("dialogTitle"));
    back_->setVisible(current != overview_scroll_);
    if (current == overview_scroll_)
    {
        const bool scrolled = overview_scroll_->verticalScrollBar()->value() >= overview_count_->geometry().bottom();
        auto const heading = label->fontMetrics().elidedText(title_, Qt::ElideRight, label->width());
        label->setText(scrolled ? heading + u'\n' + overview_count_->text() : QString{});
    }
    else
    {
        label->setText(current == files_               ? QStringLiteral("群文件")
                       : current == management_scroll_ ? QStringLiteral("群管理")
                                                       : QStringLiteral("入群申请"));
    }
}

void group_dialog::setup_overview()
{
    auto* overview_scroll = overview_scroll_ = new QScrollArea(pages_);
    overview_scroll->setObjectName(QStringLiteral("groupOverviewScroll"));
    overview_scroll->setWidgetResizable(true);
    overview_scroll->setFrameShape(QFrame::NoFrame);
    overview_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* overview = new QWidget(overview_scroll);
    overview_scroll->setWidget(overview);
    auto* overview_layout = new QVBoxLayout(overview);
    overview_layout->setContentsMargins(
        chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
    overview_layout->setSpacing(chat_theme::dialog_spacing);
    overview_avatar_ = new QLabel(overview);
    overview_avatar_->setObjectName(QStringLiteral("groupOverviewAvatar"));
    overview_avatar_->setFixedSize(94, 94);
    overview_avatar_->setPixmap(avatar_icon(title_, 94, {}, conversation_).pixmap(94));
    overview_layout->addWidget(overview_avatar_, 0, Qt::AlignHCenter);
    overview_title_ = new QLabel(title_, overview);
    overview_title_->setObjectName(QStringLiteral("groupOverviewTitle"));
    overview_title_->setTextFormat(Qt::PlainText);
    overview_title_->setWordWrap(true);
    overview_title_->setAlignment(Qt::AlignCenter);
    overview_layout->addWidget(overview_title_);
    overview_count_ = new QLabel(QStringLiteral("正在加载成员…"), overview);
    overview_count_->setObjectName(QStringLiteral("groupOverviewCount"));
    overview_count_->setAlignment(Qt::AlignCenter);
    overview_layout->addWidget(overview_count_);

    auto* quick = new QHBoxLayout;
    quick->setSpacing(9);
    notification_ = profile_action(overview, QStringLiteral("静音"), {});
    notification_->setObjectName(QStringLiteral("groupNotificationButton"));
    leave_button_ = profile_action(overview, QStringLiteral("退出群聊"), u"logout", true);
    leave_button_->setObjectName(QStringLiteral("groupLeaveButton"));
    management_button_ = profile_action(overview, QStringLiteral("群管理"), u"settings");
    management_button_->setObjectName(QStringLiteral("groupManageButton"));
    for (auto* action : {notification_, leave_button_, management_button_})
    {
        quick->addWidget(action, 1);
    }
    overview_layout->addLayout(quick);
    connect(notification_,
            &QToolButton::clicked,
            this,
            [this]
            {
                if (auto const room = chat_.conversation(conversation_))
                {
                    emit notification_requested(!room->muted());
                }
            });
    connect(management_button_, &QToolButton::clicked, this, [this] { show_page(management_scroll_); });
    connect(&theme_manager::instance(), &theme_manager::changed, this, &group_dialog::refresh_notifications);
    connect(&chat_, &chat_widget::notification_expired, this, &group_dialog::refresh_notifications);
    refresh_notifications();
    description_ = new QLabel(overview);
    description_->setObjectName(QStringLiteral("groupDescription"));
    description_->setTextFormat(Qt::PlainText);
    description_->setWordWrap(true);
    description_heading_ = new QLabel(QStringLiteral("群简介"), overview);
    description_heading_->setObjectName(QStringLiteral("groupSectionHeading"));
    overview_layout->addWidget(description_);
    overview_layout->addWidget(description_heading_);
    description_heading_->hide();
    description_->hide();
    if (avatars_)
    {
        connect(avatars_,
                &avatar_cache::changed,
                this,
                [this](qint64 key)
                {
                    if (key == -conversation_)
                    {
                        overview_avatar_->setPixmap(avatar_icon(title_, 94, avatars_->image(key), conversation_).pixmap(94));
                    }
                });
    }
    auto* announcement_heading = announcement_heading_ = new QLabel(QStringLiteral("群公告"), overview);
    announcement_heading->setObjectName(QStringLiteral("groupSectionHeading"));
    overview_layout->addWidget(announcement_heading);
    announcement_heading_->setVisible(!announcement_.isEmpty());
    overview_announcement_ = new QLabel(announcement_, overview);
    overview_announcement_->setObjectName(QStringLiteral("groupOverviewAnnouncement"));
    overview_announcement_->setVisible(!announcement_.isEmpty());
    overview_announcement_->setTextFormat(Qt::PlainText);
    overview_announcement_->setWordWrap(true);
    overview_announcement_->setMaximumHeight(120);
    overview_announcement_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    overview_layout->addWidget(overview_announcement_);
    read_announcement_button_ = new QPushButton(QStringLiteral("查看完整公告"), overview);
    read_announcement_button_->setObjectName(QStringLiteral("groupReadAnnouncementButton"));
    read_announcement_button_->setVisible(!announcement_.isEmpty());
    overview_layout->addWidget(read_announcement_button_, 0, Qt::AlignLeft);
    connect(read_announcement_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                QDialog dialog(this);
                dialog.setObjectName(QStringLiteral("groupAnnouncementDialog"));
                dialog.setWindowTitle(QStringLiteral("群公告"));
                dialog.resize(480, 400);
                auto* layout = new QVBoxLayout(&dialog);
                frame_dialog(dialog);
                layout->setContentsMargins(
                    chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
                layout->setSpacing(chat_theme::dialog_spacing);
                auto* text = new QPlainTextEdit(announcement_, &dialog);
                text->setReadOnly(true);
                text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
                layout->addWidget(text);
                dialog.exec();
            });
    overview_pin_ = new QPushButton(overview);
    overview_pin_->setObjectName(QStringLiteral("groupOverviewPinned"));
    overview_pin_->setText(QStringLiteral("查看置顶消息"));
    overview_pin_->hide();
    connect(overview_pin_,
            &QPushButton::clicked,
            this,
            [this]
            {
                if (pin_id_ > cleared_through_)
                {
                    emit pinned_requested(pin_id_);
                }
            });
    overview_layout->addWidget(overview_pin_);
    overview_invite_ = new QLabel(overview);
    overview_invite_->setObjectName(QStringLiteral("groupOverviewInvite"));
    overview_invite_->setTextFormat(Qt::PlainText);
    overview_invite_->setWordWrap(true);
    overview_invite_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Inviting is reachable from the overview: managers copy (creating if needed) in one step.
    auto* invite_overview_row = new QHBoxLayout;
    invite_overview_row->setSpacing(chat_theme::dialog_spacing);
    invite_overview_row->addWidget(overview_invite_);
    overview_copy_invite_ = new QPushButton(QStringLiteral("复制邀请码"), overview);
    overview_copy_invite_->setObjectName(QStringLiteral("groupOverviewCopyInvite"));
    overview_copy_invite_->setAutoDefault(false);
    invite_overview_row->addWidget(overview_copy_invite_);
    invite_overview_row->addStretch();
    overview_layout->addLayout(invite_overview_row);
    overview_layout->addSpacing(6);
    auto* media = new QVBoxLayout;
    media->setSpacing(0);
    static const QStringView titles[]{u"图片", u"文件", u"链接"};
    static const QStringView icons[]{u"image", u"file", u"link"};
    for (int i = 0; i < 3; ++i)
    {
        auto* entry = profile_entry(overview, titles[i].toString(), icons[i]);
        media->addWidget(entry);
        connect(entry, &QPushButton::clicked, this, [this, category = i + 1] { emit history_requested(category); });
    }
    auto* group_files = profile_entry(overview, QStringLiteral("群文件"), u"folder");
    group_files->setObjectName(QStringLiteral("groupFilesEntry"));
    media->addWidget(group_files);
    connect(group_files, &QPushButton::clicked, this, [this] { show_page(files_); });
    requests_button_ = profile_entry(overview, QStringLiteral("入群申请"), u"user-plus");
    requests_button_->setObjectName(QStringLiteral("groupRequestsEntry"));
    media->addWidget(requests_button_);
    connect(requests_button_, &QPushButton::clicked, this, [this] { show_page(requests_page_); });
    overview_layout->addLayout(media);
    setup_members(overview_layout);
    overview_layout->addStretch();
    pages_->insertWidget(0, overview_scroll);
    pages_->setCurrentWidget(overview_scroll);
}

void group_dialog::refresh_notifications()
{
    auto const room = chat_.conversation(conversation_);
    display_notification_action(*notification_, room && room->muted());
}

void group_dialog::setup_management(bool join_approval)
{
    management_scroll_ = new QScrollArea(pages_);
    management_scroll_->setObjectName(QStringLiteral("groupManagementScroll"));
    management_scroll_->setWidgetResizable(true);
    management_scroll_->setFrameShape(QFrame::NoFrame);
    management_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* management = new QWidget(management_scroll_);
    management_scroll_->setWidget(management);
    auto* layout = new QVBoxLayout(management);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    auto* name_heading = new QLabel(QStringLiteral("群名称"), management);
    name_heading->setObjectName(QStringLiteral("groupSectionHeading"));
    layout->addWidget(name_heading);
    auto* name_row = new QHBoxLayout;
    title_edit_ = new QLineEdit(title_, this);
    title_edit_->setObjectName(QStringLiteral("groupTitleEdit"));
    title_edit_->setPlaceholderText(QStringLiteral("群名称"));
    title_edit_->setAccessibleName(QStringLiteral("群名称"));
    name_row->addWidget(title_edit_, 1);
    rename_button_ = new QPushButton(QStringLiteral("保存群名"), this);
    rename_button_->setObjectName(QStringLiteral("groupRenameButton"));
    rename_button_->setProperty("primary", true);
    rename_button_->setAutoDefault(false);
    name_row->addWidget(rename_button_);
    layout->addLayout(name_row);
    auto* profile_actions = new QHBoxLayout;
    edit_description_ = new QPushButton(QStringLiteral("编辑简介"), management);
    change_avatar_ = new QPushButton(QStringLiteral("更换群头像"), management);
    clear_avatar_ = new QPushButton(QStringLiteral("清除群头像"), management);
    change_avatar_->setObjectName(QStringLiteral("changeGroupAvatar"));
    clear_avatar_->setObjectName(QStringLiteral("clearGroupAvatar"));
    edit_description_->setObjectName(QStringLiteral("editGroupDescription"));
    for (auto* button : {edit_description_, change_avatar_, clear_avatar_})
    {
        profile_actions->addWidget(button);
    }
    layout->addLayout(profile_actions);
    connect(edit_description_,
            &QPushButton::clicked,
            this,
            [this]
            {
                bool ok = false;
                auto text = prompt_text(this, QStringLiteral("群简介"), QStringLiteral("最多 256 个字符"), description_->text(), ok, true);
                if (ok)
                {
                    emit description_requested(std::move(text));
                }
            });
    connect(change_avatar_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto image = choose_avatar(this);
                if (!image)
                {
                    status_->setText(image.error());
                }
                else if (!image->isEmpty())
                {
                    emit group_avatar_requested(std::move(*image));
                }
            });
    connect(clear_avatar_, &QPushButton::clicked, this, [this] { emit group_avatar_requested({}); });
    auto* edit_announcement_heading = new QLabel(QStringLiteral("群公告 · 纯文本，最多 4 KiB"), management);
    edit_announcement_heading->setObjectName(QStringLiteral("groupSectionHeading"));
    layout->addWidget(edit_announcement_heading);
    announcement_edit_ = new QPlainTextEdit(announcement_, this);
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
    announcement_button_->setProperty("primary", true);
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
    auto* invite_heading = new QLabel(QStringLiteral("邀请码"), invite_controls_);
    invite_heading->setObjectName(QStringLiteral("groupSectionHeading"));
    invite_layout->addWidget(invite_heading);
    approval_ = new QCheckBox(QStringLiteral("通过邀请码加入需要审批"), invite_controls_);
    approval_->setObjectName(QStringLiteral("groupJoinApprovalCheck"));
    approval_->setChecked(join_approval);
    invite_layout->addWidget(approval_);
    invite_edit_ = new QLineEdit(invite_controls_);
    invite_edit_->setObjectName(QStringLiteral("groupInviteLinkEdit"));
    invite_edit_->setReadOnly(true);
    invite_edit_->setPlaceholderText(QStringLiteral("尚未生成邀请码"));
    invite_edit_->setAccessibleName(QStringLiteral("群邀请码"));
    invite_layout->addWidget(invite_edit_);
    auto* invite_row = new QHBoxLayout;
    invite_row->setSpacing(chat_theme::dialog_spacing);
    invite_row->addStretch();
    create_invite_button_ = new QPushButton(QStringLiteral("生成邀请码"), invite_controls_);
    create_invite_button_->setObjectName(QStringLiteral("groupCreateInviteButton"));
    copy_invite_button_ = new QPushButton(QStringLiteral("复制邀请码"), invite_controls_);
    copy_invite_button_->setObjectName(QStringLiteral("groupCopyInviteButton"));
    revoke_invite_button_ = new QPushButton(QStringLiteral("撤销邀请码"), invite_controls_);
    revoke_invite_button_->setObjectName(QStringLiteral("groupRevokeInviteButton"));
    for (auto* button : {create_invite_button_, copy_invite_button_, revoke_invite_button_})
    {
        button->setAutoDefault(false);
        invite_row->addWidget(button);
    }
    invite_layout->addLayout(invite_row);
    layout->addWidget(invite_controls_);
    all_mute_ = new QCheckBox(QStringLiteral("全员禁言（仅限制普通成员）"), management);
    all_mute_->setObjectName(QStringLiteral("groupMuteAll"));
    layout->addWidget(all_mute_);
    dissolve_button_ = new QPushButton(QStringLiteral("解散群聊"), management);
    dissolve_button_->setObjectName(QStringLiteral("groupDissolveButton"));
    dissolve_button_->setAutoDefault(false);
    layout->addWidget(dissolve_button_, 0, Qt::AlignLeft);
    layout->addStretch();
}

void group_dialog::setup_members(QVBoxLayout* members_layout)
{
    auto* overview = members_layout->parentWidget();
    auto* header = new QHBoxLayout;
    auto* members_icon = new QLabel(overview);
    auto update_icon = [members_icon] { members_icon->setPixmap(svg_icon(u"groups", themed("text_secondary"), {23, 23}).pixmap(23, 23)); };
    update_icon();
    connect(&theme_manager::instance(), &theme_manager::changed, members_icon, update_icon);
    header->addWidget(members_icon);

    members_heading_ = new QLabel(QStringLiteral("成员"), overview);
    members_heading_->setObjectName(QStringLiteral("groupMembersTitle"));
    header->addWidget(members_heading_, 1);
    auto* search = new QToolButton(overview);
    search->setObjectName(QStringLiteral("groupMemberSearchButton"));
    search->setToolTip(QStringLiteral("搜索成员"));
    search->setAccessibleName(search->toolTip());
    search->setCheckable(true);
    search->setProperty("navigationTool", true);
    search->setFixedSize(32, 32);
    search->setIconSize(chat_theme::action_icon);
    auto search_icon = [search] { search->setIcon(svg_icon(u"search", themed("text_secondary"), chat_theme::action_icon)); };
    search_icon();
    connect(&theme_manager::instance(), &theme_manager::changed, search, search_icon);
    header->addWidget(search);
    members_layout->addLayout(header);
    list_ = new QListWidget(overview);
    list_->setObjectName(QStringLiteral("groupMembersList"));
    list_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setFixedHeight(0);
    members_empty_ = new empty_state(*list_->viewport(), QStringLiteral("groupMembersEmpty"));
    for (auto* list : {list_, requests_})
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
        connect(delegate,
                &user_delegate::avatar_clicked,
                this,
                [this, list](QModelIndex const& index)
                {
                    auto* item = list->item(index.row());
                    emit user_requested(item->data(Qt::UserRole).toLongLong(), item->text());
                });
    }
    list_->setAccessibleName(QStringLiteral("群成员"));
    requests_->setAccessibleName(QStringLiteral("待处理入群申请"));
    if (avatars_)
    {
        connect(avatars_,
                &avatar_cache::changed,
                this,
                [this](qint64 user)
                {
                    for (auto* list : {list_, requests_})
                    {
                        for (int row = 0; row < list->count(); ++row)
                        {
                            auto* item = list->item(row);
                            if (item->data(Qt::UserRole).toLongLong() == user)
                            {
                                item->setData(Qt::DecorationRole, avatars_->image(user));
                            }
                        }
                    }
                });
    }
    member_search_ = new QLineEdit(overview);
    style_search_field(*member_search_);
    member_search_->setPlaceholderText(QStringLiteral("搜索成员"));
    member_search_->setObjectName(QStringLiteral("groupMemberSearch"));
    member_search_->hide();
    members_layout->addWidget(member_search_);
    auto reveal_search = [this]
    {
        // Showing the field or shrinking the filtered list changes the scroll range.
        QTimer::singleShot(0,
                           this,
                           [this]
                           {
                               if (member_search_->isVisible() && member_search_->hasFocus())
                               {
                                   auto* last = members_empty_->isVisible() ? static_cast<QWidget*>(list_) : member_search_;
                                   auto const bottom = last->mapTo(overview_scroll_->widget(), last->rect().bottomLeft());
                                   overview_scroll_->ensureVisible(0, bottom.y(), 0, 6);
                               }
                           });
    };
    connect(search,
            &QToolButton::toggled,
            this,
            [this, search, reveal_search](bool on)
            {
                if (on)
                {
                    member_search_->show();
                    member_search_->setFocus(Qt::ShortcutFocusReason);
                    reveal_search();
                }
                else
                {
                    if (member_search_->hasFocus())
                    {
                        search->setFocus(Qt::OtherFocusReason);
                    }
                    member_search_->hide();
                    member_search_->clear();
                }
            });
    auto* close_search = new QShortcut(QKeySequence(Qt::Key_Escape), member_search_);
    close_search->setContext(Qt::WidgetShortcut);
    connect(close_search, &QShortcut::activated, search, [search] { search->setChecked(false); });
    connect(member_search_, &QLineEdit::textChanged, this, &group_dialog::filter_members);
    connect(member_search_, &QLineEdit::textChanged, this, reveal_search);
    members_layout->addWidget(list_);
    connect(list_,
            &QListWidget::currentItemChanged,
            this,
            [this](QListWidgetItem* item)
            {
                if (item && list_->hasFocus())
                {
                    auto const point = list_->mapTo(overview_scroll_->widget(), list_->visualItemRect(item).center());
                    overview_scroll_->ensureVisible(point.x(), point.y(), 0, chat_theme::user_row_height / 2);
                }
            });
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
    invite_button_ = new QPushButton(QStringLiteral("邀请好友"), this);
    invite_button_->setObjectName(QStringLiteral("groupInviteButton"));
    invite_button_->setAutoDefault(false);
    remove_button_->hide();
    member_mute_button_ = new QPushButton(QStringLiteral("成员禁言"), this);
    member_mute_button_->setObjectName(QStringLiteral("groupMemberMuteButton"));
    member_mute_button_->hide();
    members_layout->addWidget(invite_button_);
}

void group_dialog::setup_join_requests()
{
    auto* requests_page = new QWidget(pages_);
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
    accept_request_button_->setProperty("primary", true);
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
    requests_page_ = requests_page;
    pages_->addWidget(requests_page);
}

void group_dialog::connect_member_actions()
{
    connect(list_, &QListWidget::currentRowChanged, this, [this] { update_actions(); });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_,
            &QWidget::customContextMenuRequested,
            this,
            [this](QPoint point)
            {
                if (auto* item = list_->itemAt(point))
                {
                    list_->setCurrentItem(item);
                }
                if (!list_->currentItem())
                {
                    return;
                }
                update_actions();
                QMenu menu(this);
                auto const target = members_[list_->currentRow()].id;
                menu.addAction(QStringLiteral("个人资料"),
                               [this, target]
                               {
                                   auto found =
                                       std::find_if(members_.begin(), members_.end(), [target](auto const& member) { return member.id == target; });
                                   if (found != members_.end())
                                   {
                                       emit user_requested(found->id, found->username);
                                   }
                               });
                for (auto* button : {admin_button_, transfer_button_, remove_button_, member_mute_button_})
                {
                    if (button->isEnabled())
                    {
                        menu.addAction(button->text(),
                                       [this, target, button]
                                       {
                                           auto found = std::find_if(
                                               members_.begin(), members_.end(), [target](auto const& member) { return member.id == target; });
                                           if (found == members_.end())
                                           {
                                               return;
                                           }
                                           list_->setCurrentRow(static_cast<int>(std::distance(members_.begin(), found)));
                                           update_actions();
                                           if (button->isEnabled())
                                           {
                                               button->click();
                                           }
                                       });
                    }
                }
                menu.exec(list_->viewport()->mapToGlobal(point));
            });
    connect(list_,
            &QListWidget::itemDoubleClicked,
            this,
            [this](QListWidgetItem* item)
            {
                auto const row = list_->row(item);
                if (row >= 0 && row < members_.size())
                {
                    emit user_requested(members_[row].id, members_[row].username);
                }
            });
    connect(admin_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const row = list_->currentRow();
                if (row < 0 || row >= members_.size() || !admin_button_->isEnabled())
                {
                    return;
                }
                pending_ = true;
                update_actions();
                emit admin_requested(members_[row].id, members_[row].role != chat::member_role::admin);
            });
    connect(transfer_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const row = list_->currentRow();
                auto const target = members_[row];
                if (confirm_action(this,
                                   QStringLiteral("转让群主"),
                                   QStringLiteral("将群主转让给 %1？你将成为管理员，之后可以退出群聊。").arg(target.username),
                                   QStringLiteral("转让群主")))
                {
                    pending_ = true;
                    update_actions();
                    emit transfer_requested(target.id);
                }
            });
    connect(remove_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const row = list_->currentRow();
                auto const target = members_[row];
                if (confirm_action(this,
                                   QStringLiteral("移除成员"),
                                   QStringLiteral("将 %1 移出群聊？对方将无法继续访问群，重新加入需要邀请。").arg(target.username),
                                   QStringLiteral("移除成员")))
                {
                    pending_ = true;
                    update_actions();
                    emit remove_requested(target.id);
                }
            });
    connect(invite_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                create_group_dialog dialog(this, true);
                QList<user_data> candidates;
                for (auto const& contact : contacts_)
                {
                    if (std::ranges::find(members_, contact.id, &member_data::id) == members_.end())
                    {
                        candidates.push_back(contact);
                    }
                }
                if (candidates.isEmpty())
                {
                    status_->setText(QStringLiteral("没有可邀请的好友"));
                    update_actions();
                    return;
                }
                dialog.set_candidates(candidates, avatars_);
                if (dialog.exec() != QDialog::Accepted)
                {
                    return;
                }
                auto ids = dialog.selected_members();
                if (!ids.isEmpty())
                {
                    pending_ = true;
                    update_actions();
                    emit invite_requested(std::move(ids));
                }
            });
    connect(member_mute_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const row = list_->currentRow();
                if (row < 0 || row >= members_.size())
                {
                    return;
                }
                auto const target = members_[row];
                QStringList const choices{QStringLiteral("解除禁言"),
                                          QStringLiteral("10 分钟"),
                                          QStringLiteral("1 小时"),
                                          QStringLiteral("24 小时"),
                                          QStringLiteral("永久禁言")};
                constexpr qint64 seconds[] = {0, 600, 3600, 86400, -1};
                QInputDialog dialog(this);
                dialog.setWindowTitle(QStringLiteral("成员禁言"));
                dialog.setLabelText(target.username);
                dialog.setComboBoxItems(choices);
                dialog.setTextValue(choices[1]);
                dialog.setOkButtonText(QStringLiteral("确定"));
                dialog.setCancelButtonText(QStringLiteral("取消"));
                frame_dialog(dialog, false);
                bool const accepted = dialog.exec() == QDialog::Accepted;
                auto const selection = dialog.textValue();
                auto const choice = choices.indexOf(selection);
                if (accepted && choice >= 0 && available_)
                {
                    pending_ = true;
                    update_actions();
                    emit member_mute_requested(target.id, seconds[choice]);
                }
            });
    connect(all_mute_,
            &QCheckBox::clicked,
            this,
            [this](bool muted)
            {
                all_mute_->setChecked(group_muted_);
                pending_ = true;
                update_actions();
                emit group_mute_requested(muted);
            });
    connect(dissolve_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                if (confirm_action(this,
                                   QStringLiteral("解散群聊"),
                                   QStringLiteral("解散后所有成员将无法通过 Chat 访问这个群的历史和群文件，且不能再加入。确定解散？"),
                                   QStringLiteral("解散群聊")) &&
                    available_)
                {
                    pending_ = true;
                    update_actions();
                    emit dissolve_requested();
                }
            });
    connect(leave_button_,
            &QToolButton::clicked,
            this,
            [this]
            {
                if (confirm_action(this,
                                   QStringLiteral("退出群聊"),
                                   QStringLiteral("退出后将无法访问本群的聊天记录和群文件。确定退出群聊？"),
                                   QStringLiteral("退出群聊")))
                {
                    pending_ = true;
                    update_actions();
                    emit leave_requested();
                }
            });
}

void group_dialog::connect_approval_actions()
{
    connect(requests_, &QListWidget::currentRowChanged, this, [this] { update_actions(); });
    connect(approval_,
            &QCheckBox::clicked,
            this,
            [this](bool required)
            {
                approval_->setChecked(!required);
                pending_ = true;
                update_actions();
                emit approval_requested(required);
            });
    connect(accept_request_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const user = requests_->currentItem()->data(Qt::UserRole).toLongLong();
                pending_ = true;
                update_actions();
                emit request_response_requested(user, true);
            });
    connect(reject_request_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const user = requests_->currentItem()->data(Qt::UserRole).toLongLong();
                pending_ = true;
                update_actions();
                emit request_response_requested(user, false);
            });
    connect(more_requests_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                pending_ = true;
                update_actions();
                emit requests_requested(next_request_);
            });
}

void group_dialog::connect_group_settings()
{
    connect(title_edit_, &QLineEdit::textChanged, this, [this] { update_actions(); });
    connect(announcement_edit_, &QPlainTextEdit::textChanged, this, [this] { update_actions(); });
    connect(create_invite_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                pending_ = true;
                update_actions();
                emit invite_link_requested(true);
            });
    connect(revoke_invite_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                pending_ = true;
                update_actions();
                emit invite_link_requested(false);
            });
    connect(copy_invite_button_, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(invite_edit_->text()); });
    connect(overview_copy_invite_,
            &QPushButton::clicked,
            this,
            [this]
            {
                if (!invite_edit_->text().isEmpty())
                {
                    QGuiApplication::clipboard()->setText(invite_edit_->text());
                    status_->setText(QStringLiteral("邀请码已复制"));
                    update_actions();
                    return;
                }
                copy_after_create_ = true;
                pending_ = true;
                update_actions();
                emit invite_link_requested(true);
            });
    connect(announcement_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
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
    connect(clear_announcement_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                announcement_edit_->clear();
                pending_ = true;
                update_actions();
                emit announcement_requested({});
            });
    connect(rename_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                pending_ = true;
                update_actions();
                emit rename_requested(title_edit_->text());
            });
}

bool group_dialog::eventFilter(QObject* object, QEvent* event)
{
    if (event->type() == QEvent::Resize)
    {
        for (auto* list : {list_, requests_})
        {
            if (object == list->viewport())
            {
                QTimer::singleShot(0,
                                   list,
                                   [list]
                                   {
                                       if (list->hasFocus() && list->currentItem())
                                       {
                                           list->scrollToItem(list->currentItem());
                                       }
                                   });
            }
        }
    }
    if (event->type() == QEvent::KeyPress && (object == list_ || object == requests_))
    {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            auto* item = static_cast<QListWidget*>(object)->currentItem();
            if (item)
            {
                emit user_requested(item->data(Qt::UserRole).toLongLong(), item->text());
            }
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
    std::stable_sort(
        members_.begin(), members_.end(), [](auto const& a, auto const& b) { return static_cast<int>(a.role) > static_cast<int>(b.role); });
    list_->clear();
    overview_count_->setText(QStringLiteral("%1 位成员").arg(members_.size()));
    members_heading_->setText(overview_count_->text());
    for (auto const& member : members_)
    {
        if (avatars_)
        {
            avatars_->observe(member.id, member.avatar);
        }
        auto* item = new QListWidgetItem(member.display_name(), list_);
        item->setData(Qt::UserRole, member.id);
        item->setData(Qt::DecorationRole, avatars_ ? avatars_->image(member.id) : QPixmap{});
        if (member.id == selected)
        {
            list_->setCurrentItem(item);
        }
    }
    filter_members();
    update_title();
    available_ = true;
    status_->clear();
    refresh_speech();
    update_actions();
    auto const self = std::find_if(members_.begin(), members_.end(), [this](auto const& member) { return member.id == self_user_; });
    if (self != members_.end() && self->role != chat::member_role::member)
    {
        if (open_requests_)
        {
            show_page(requests_page_);
        }
        emit invite_link_requested(std::nullopt);
        refresh_requests();
    }
    open_requests_ = false;
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

void group_dialog::filter_members()
{
    auto const query = member_search_->text().trimmed();
    for (int i = 0; i < members_.size(); ++i)
    {
        auto const& m = members_[i];
        auto* row = list_->item(i);

        row->setHidden(!query.isEmpty() && !m.username.contains(query, Qt::CaseInsensitive) && !m.nickname.contains(query, Qt::CaseInsensitive) &&
                       !m.remark.contains(query, Qt::CaseInsensitive));
        if (row->isHidden() && row == list_->currentItem())
        {
            list_->setCurrentRow(-1);
        }
    }
    auto count = 0;
    for (int i = 0; i < list_->count(); ++i)
    {
        count += !list_->item(i)->isHidden();
    }
    const bool empty_search = !query.isEmpty() && !count;
    list_->setFixedHeight(empty_search ? 70 : count * chat_theme::user_row_height);
    members_empty_->set_message(empty_search ? QStringLiteral("未找到成员") : QString{});
    update_actions();
}

void group_dialog::set_contacts(QList<user_data> contacts, QString const& error)
{
    contacts_ready_ = error.isEmpty();
    contacts_ = std::move(contacts);
    if (contacts_ready_)
    {
        for (int i = 0; i < members_.size(); ++i)
        {
            auto& m = members_[i];
            auto friend_it = std::ranges::find(contacts_, m.id, &user_data::id);
            auto remark = friend_it == contacts_.end() ? QString{} : friend_it->remark;
            if (remark == m.remark)
            {
                continue;
            }
            m.remark = std::move(remark);
            list_->item(i)->setText(m.display_name());
            list_->item(i)->setStatusTip({});
        }
        filter_members();
        refresh_speech();
    }
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
    auto const found = std::find_if(conversations.begin(), conversations.end(), [this](auto const& value) { return value.id == conversation_; });
    if (found == conversations.end())
    {
        reject();
        return;
    }
    description_->setText(found->description);
    description_->setVisible(!found->description.isEmpty());
    description_heading_->setVisible(!found->description.isEmpty());
    refresh_notifications();
    if (avatars_)
    {
        avatars_->observe(-conversation_, found->avatar);
    }
    overview_avatar_->setPixmap(avatar_icon(found->username, 94, avatars_ ? avatars_->image(-conversation_) : QPixmap{}, conversation_).pixmap(94));
    if (title_ != found->username)
    {
        auto const modified = title_edit_->text() != title_;
        title_ = found->username;
        if (!modified)
        {
            title_edit_->setText(title_);
        }
        overview_title_->setText(title_);

        update_title();
    }
    if (announcement_ != found->announcement)
    {
        auto const modified = announcement_edit_->toPlainText() != announcement_;
        announcement_ = found->announcement;
        if (!modified)
        {
            announcement_edit_->setPlainText(announcement_);
        }
    }
    overview_announcement_->setText(announcement_);
    overview_announcement_->setVisible(!announcement_.isEmpty());
    announcement_heading_->setVisible(!announcement_.isEmpty());
    read_announcement_button_->setVisible(!announcement_.isEmpty());
    pin_id_ = found->pinned_message.id;
    const bool has_pin = pin_id_ > cleared_through_;
    const auto preview = found->pinned_message.deleted ? QStringLiteral("消息已撤回") : found->pinned_message.text.simplified();
    overview_pin_->setText(QStringLiteral("置顶消息 · %1").arg(preview.left(48) + (preview.size() > 48 ? QStringLiteral("…") : QString{})));
    overview_pin_->setToolTip(preview);
    overview_pin_->setVisible(has_pin);
    approval_->setChecked(found->join_approval);
    group_muted_ = found->group_muted;
    own_speech_until_ = found->speech_until;
    server_clock_delta_ = found->server_clock_delta;
    refresh_speech();
    all_mute_->setChecked(group_muted_);
    update_actions();
}

void group_dialog::discard_history(qint64 conversation, qint64 through)
{
    if (conversation != conversation_)
    {
        return;
    }
    cleared_through_ = std::max(cleared_through_, through);
    if (pin_id_ <= cleared_through_)
    {
        overview_pin_->hide();
    }
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
    if (conversation != conversation_)
    {
        return;
    }
    auto const self = std::find_if(members_.begin(), members_.end(), [this](auto const& member) { return member.id == self_user_; });
    if (self == members_.end() || self->role == chat::member_role::member)
    {
        return;
    }
    if (!error.isEmpty())
    {
        copy_after_create_ = false;
        status_->setText(error);
        status_->show();
        return;
    }
    invite_edit_->setText(token.isEmpty() ? QString{} : QString::fromStdString(chat::format_invite_token(token.toStdString())));
    invite_edit_->setCursorPosition(0);
    if (std::exchange(copy_after_create_, false) && !token.isEmpty())
    {
        QGuiApplication::clipboard()->setText(invite_edit_->text());
        status_->setText(QStringLiteral("邀请码已复制"));
    }
    update_actions();
}

void group_dialog::set_requests(qint64 conversation, QList<user_data> users, qint64 next, bool older, QString const& error)
{
    if (conversation != conversation_)
    {
        return;
    }
    if (older)
    {
        pending_ = false;
    }
    auto const self = std::find_if(members_.begin(), members_.end(), [this](auto const& member) { return member.id == self_user_; });
    if (!available_ || self == members_.end() || self->role == chat::member_role::member)
    {
        update_actions();
        return;
    }
    if (!error.isEmpty())
    {
        status_->setText(error);
        requests_status_->setText(requests_->count() == 0 ? QStringLiteral("无法加载申请") : QString{});
        update_actions();
        return;
    }
    auto const selected = requests_->currentItem() ? requests_->currentItem()->data(Qt::UserRole).toLongLong() : 0;
    auto const selected_visible =
        requests_->currentItem() && requests_->viewport()->rect().intersects(requests_->visualItemRect(requests_->currentItem()));
    auto const scroll_position = requests_->verticalScrollBar()->value();
    if (!older)
    {
        requests_->clear();
    }
    for (auto const& user : users)
    {
        if (avatars_)
        {
            avatars_->observe(user.id, user.avatar);
        }
        auto* item = new QListWidgetItem(user.display_name(), requests_);
        item->setData(Qt::UserRole, user.id);
        auto const status = QStringLiteral("待审批");
        item->setData(Qt::StatusTipRole, status);
        item->setData(Qt::AccessibleTextRole, user.username + QStringLiteral(" · ") + status);
        item->setData(Qt::DecorationRole, avatars_ ? avatars_->image(user.id) : QPixmap{});
        item->setToolTip(user.username);
        if (user.id == selected)
        {
            requests_->setCurrentItem(item);
        }
    }
    requests_->doItemsLayout();
    requests_->verticalScrollBar()->setValue(scroll_position);
    if (selected_visible && requests_->currentItem())
    {
        requests_->scrollToItem(requests_->currentItem());
    }
    next_request_ = next;
    requests_status_->setText(requests_->count() == 0 ? QStringLiteral("暂无待处理申请") : QString{});
    requests_button_->setText(requests_->count()
                                  ? QStringLiteral("入群申请 (%1%2)").arg(requests_->count()).arg(next ? QStringLiteral("+") : QString{})
                                  : QStringLiteral("入群申请"));
    update_actions();
}

void group_dialog::refresh_requests()
{
    next_request_ = 0;
    requests_status_->setText(QStringLiteral("正在加载申请…"));
    update_actions();
    emit requests_requested(0);
}

void group_dialog::refresh_member_status() { refresh_speech(); }

void group_dialog::refresh_speech()
{
    auto const now = QDateTime::currentMSecsSinceEpoch() + server_clock_delta_;
    auto next = std::numeric_limits<qint64>::max();
    if (own_speech_until_ > now)
    {
        next = own_speech_until_;
    }
    for (int row = 0; row < list_->count(); ++row)
    {
        auto const& member = members_[row];
        auto const role = member.role == chat::member_role::owner   ? QStringLiteral("群主")
                          : member.role == chat::member_role::admin ? QStringLiteral("管理员")
                                                                    : QString{};
        auto badge = role;
        if (member.id == self_user_)
        {
            badge += badge.isEmpty() ? QStringLiteral("你") : QStringLiteral(" · 你");
        }
        QString description;
        if (member.role == chat::member_role::member)
        {
            if (member.speech_until == -1)
            {
                description = QStringLiteral("永久禁言");
            }
            else if (member.speech_until > now)
            {
                next = std::min(next, member.speech_until);
                description =
                    QStringLiteral("禁言至 %1").arg(QDateTime::fromMSecsSinceEpoch(member.speech_until).toString(QStringLiteral("MM-dd HH:mm:ss")));
            }
        }
        auto const presence = chat_.member_presence(member.id);
        if (presence)
        {
            auto const label = presence_text(presence->online, presence->last_seen);
            if (!label.isEmpty())
            {
                description = description.isEmpty() ? label : label + QStringLiteral(" · ") + description;
            }
        }
        auto* item = list_->item(row);
        item->setData(user_model::online_role, presence && presence->online);
        item->setData(user_delegate::badge_role, badge);
        item->setStatusTip(description);
        const auto accessible = item->text() + (badge.isEmpty() ? QString{} : QStringLiteral(" · ") + badge) +
                                (description.isEmpty() ? QString{} : QStringLiteral(" · ") + description);
        item->setToolTip(accessible);
        item->setData(Qt::AccessibleTextRole, accessible);
    }
    if (next == std::numeric_limits<qint64>::max())
    {
        speech_expiry_->stop();
    }
    else
    {
        speech_expiry_->start(static_cast<int>(std::min<qint64>(next - now, std::numeric_limits<int>::max())));
    }
}

void group_dialog::update_actions()
{
    auto const self = std::find_if(members_.begin(), members_.end(), [this](auto const& member) { return member.id == self_user_; });
    auto const owner = self != members_.end() && self->role == chat::member_role::owner;
    auto const manager = self != members_.end() && self->role != chat::member_role::member;
    auto const delay = own_speech_until_ - (QDateTime::currentMSecsSinceEpoch() + server_clock_delta_);
    auto const blocked = !manager && (group_muted_ || own_speech_until_ == -1 || delay > 0);
    files_->set_access(available_ && self != members_.end(), manager, !blocked);
    auto const can_manage = available_ && manager;
    auto const enabled = available_ && !pending_;
    all_mute_->setEnabled(enabled && manager);
    dissolve_button_->setVisible(owner);
    dissolve_button_->setEnabled(enabled && owner);
    if (pending_)
    {
        status_->setText(QStringLiteral("正在处理…"));
    }
    status_->setVisible(!status_->text().isEmpty());
    invite_controls_->setVisible(manager);
    approval_->setEnabled(enabled && manager);
    management_button_->setVisible(can_manage);
    requests_button_->setVisible(can_manage);
    notification_->setEnabled(enabled && self != members_.end());
    overview_invite_->setVisible(manager && !invite_edit_->text().isEmpty());
    overview_invite_->setText(QStringLiteral("邀请码 · ") + invite_edit_->text());
    overview_copy_invite_->setVisible(manager);
    overview_copy_invite_->setEnabled(enabled && manager);
    if (!can_manage)
    {
        next_request_ = 0;
        if (requests_->count() > 0)
        {
            QSignalBlocker blocked(requests_);
            requests_->clear();
        }
        if (pages_->currentWidget() == management_scroll_ || pages_->currentWidget() == requests_page_)
        {
            show_page(overview_scroll_);
        }
    }
    accept_request_button_->setEnabled(enabled && manager && requests_->currentItem());
    reject_request_button_->setEnabled(enabled && manager && requests_->currentItem());
    more_requests_button_->setEnabled(enabled && manager && next_request_ > 0);
    accept_request_button_->setVisible(can_manage && requests_->count() > 0);
    reject_request_button_->setVisible(can_manage && requests_->count() > 0);
    more_requests_button_->setVisible(can_manage && next_request_ > 0);
    requests_status_->setVisible(can_manage && !requests_status_->text().isEmpty());
    if (!manager)
    {
        invite_edit_->clear();
        copy_after_create_ = false;
    }
    create_invite_button_->setEnabled(enabled && manager && invite_edit_->text().isEmpty());
    copy_invite_button_->setEnabled(enabled && manager && !invite_edit_->text().isEmpty());
    revoke_invite_button_->setEnabled(enabled && manager && !invite_edit_->text().isEmpty());
    edit_description_->setEnabled(enabled && manager);
    change_avatar_->setEnabled(enabled && manager);
    clear_avatar_->setEnabled(enabled && manager && avatars_ && avatars_->state(-conversation_).present);
    title_edit_->setEnabled(enabled && manager);
    if (!manager && announcement_edit_->toPlainText() != announcement_)
    {
        announcement_edit_->setPlainText(announcement_);
    }
    announcement_edit_->setReadOnly(!enabled || !manager);
    auto const announcement = announcement_edit_->toPlainText();
    auto const announcement_bytes = announcement.toUtf8();
    auto const announcement_value = chat::text_is_blank(announcement_bytes.toStdString()) ? QString{} : announcement;
    announcement_button_->setEnabled(enabled && manager && announcement_value != announcement_ && announcement_bytes.size() <= 4096 &&
                                     !announcement.contains(QChar{}));
    announcement_button_->setToolTip(announcement.toUtf8().size() > 4096 ? QStringLiteral("公告超过 4 KiB，请缩短后保存。") : QString{});
    clear_announcement_button_->setEnabled(enabled && manager && !announcement_.isEmpty());
    rename_button_->setEnabled(enabled && manager && chat::valid_group_title(title_edit_->text().toUtf8().toStdString()) &&
                               title_edit_->text() != title_);
    invite_button_->setEnabled(enabled && manager && contacts_ready_);
    leave_button_->setEnabled(enabled && self != members_.end() && !owner);
    leave_button_->setVisible(self != members_.end() && !owner);
    leave_button_->setToolTip(owner ? QStringLiteral("请先将群主转让给一位管理员，再退出群聊。") : QString{});
    auto const admins = std::count_if(members_.begin(), members_.end(), [](auto const& member) { return member.role == chat::member_role::admin; });
    auto const row = list_->currentRow();
    auto const selected = row >= 0 && row < members_.size();
    auto const admin = selected && members_[row].role == chat::member_role::admin;
    admin_button_->setText(admin ? QStringLiteral("取消管理员") : QStringLiteral("设为管理员"));
    admin_button_->setEnabled(available_ && !pending_ && owner && selected && members_[row].role != chat::member_role::owner &&
                              (admin || admins < 3));
    transfer_button_->setEnabled(enabled && owner && selected && admin);
    member_mute_button_->setEnabled(enabled && manager && selected && members_[row].role == chat::member_role::member);
    remove_button_->setEnabled(enabled && manager && selected && members_[row].id != self_user_ && members_[row].role != chat::member_role::owner &&
                               (owner || !admin));
}
