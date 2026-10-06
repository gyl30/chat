#include "theme.hpp"
#include <QAbstractButton>
#include <QAccessibleObject>
#include <QApplication>
#include <QLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <utility>

class accessible_feedback_notification final : public QAccessibleObject
{
   public:
    explicit accessible_feedback_notification(QObject* notification)
        : QAccessibleObject(notification)
    {
    }

    QAccessibleInterface* parent() const override
    {
        return QAccessible::queryAccessibleInterface(qApp);
    }

    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(QAccessibleInterface const*) const override { return -1; }
    QAccessible::Role role() const override { return QAccessible::Notification; }

    QAccessible::State state() const override
    {
        QAccessible::State result;
        result.invisible = !static_cast<QLabel*>(object()->parent())->isVisible();
        result.readOnly = true;
        return result;
    }

    QString text(QAccessible::Text type) const override
    {
        if (type == QAccessible::Name)
        {
            return static_cast<QLabel*>(object()->parent())->text();
        }
        return {};
    }
};

feedback_label::feedback_label(QWidget* parent) : QLabel(parent), notification_(this)
{
    notification_.setObjectName(QStringLiteral("feedbackNotification"));
    QAccessible::registerAccessibleInterface(new accessible_feedback_notification(&notification_));
}

void feedback_label::show_error(QString message)
{
    setText(std::move(message));
    if (isVisible() && !text().isEmpty())
    {
        QAccessibleEvent event(&notification_, QAccessible::ObjectShow);
        QAccessible::updateAccessibility(&event);
    }
}

bool confirm_action(QWidget* parent, QString const& title, QString const& text, QString const& action)
{
    QMessageBox dialog(QMessageBox::NoIcon, title, text, QMessageBox::Yes | QMessageBox::No, parent);
    dialog.setObjectName(QStringLiteral("confirmationDialog"));
    dialog.setTextFormat(Qt::PlainText);
    dialog.button(QMessageBox::Yes)->setText(action);
    dialog.button(QMessageBox::Yes)->setObjectName(QStringLiteral("confirmationActionButton"));
    dialog.button(QMessageBox::No)->setText(QStringLiteral("取消"));
    dialog.button(QMessageBox::No)->setObjectName(QStringLiteral("confirmationCancelButton"));
    for (auto* button : dialog.buttons())
    {
        button->setIcon({});
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
    dialog.setDefaultButton(QMessageBox::No);
    dialog.setEscapeButton(QMessageBox::No);
    dialog.layout()->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                                       chat_theme::dialog_padding, chat_theme::dialog_padding);
    dialog.layout()->setSpacing(chat_theme::dialog_spacing);
    return dialog.exec() == QMessageBox::Yes;
}

void show_notice(QWidget* parent, QString const& title, QString const& text, QString const& action)
{
    QMessageBox dialog(QMessageBox::NoIcon, title, text, QMessageBox::Ok, parent);
    dialog.setObjectName(QStringLiteral("noticeDialog"));
    dialog.setTextFormat(Qt::PlainText);
    auto* button = dialog.button(QMessageBox::Ok);
    button->setText(action);
    button->setIcon({});
    dialog.layout()->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                                       chat_theme::dialog_padding, chat_theme::dialog_padding);
    dialog.layout()->setSpacing(chat_theme::dialog_spacing);
    dialog.exec();
}

QString chat_style_sheet()
{
    return QStringLiteral(R"(
        QMainWindow {
            background: #F7F5EF;
        }
        QWidget {
            background: transparent;
            color: #27332E;
            font-size: 14px;
        }
        QDialog {
            background: #FFFEFA;
        }
        QFileDialog QComboBox {
            background: #FFFFFF;
            color: #27332E;
            border: 1px solid #AFC0B8;
            border-radius: 6px;
            min-height: 32px;
            padding: 0 8px;
        }
        QFileDialog QComboBox:focus {
            border-color: #547C68;
        }
        QFileDialog QComboBox QAbstractItemView {
            background: #FFFFFF;
            color: #27332E;
            border: 1px solid #AFC0B8;
            selection-background-color: #315A4B;
            selection-color: #FFFFFF;
        }
        QFileDialog QHeaderView::section {
            background: #F0F4F1;
            color: #27332E;
            border: 0;
            border-bottom: 1px solid #D7DDD9;
            padding: 4px 8px;
        }
        QFileDialog QToolButton {
            background: transparent;
            color: #27332E;
            border: 2px solid transparent;
            border-radius: 6px;
            padding: 2px;
        }
        QFileDialog QToolButton:hover {
            background: #F0F4F1;
        }
        QFileDialog QToolButton:pressed {
            background: #E7EEE9;
        }
        QFileDialog QToolButton:focus {
            border-color: #547C68;
        }
        QMenu {
            background: #FFFEFA;
            color: #27332E;
            border: 1px solid #D7DDD9;
            border-radius: 8px;
            padding: 6px;
        }
        QMenu::item {
            padding: 8px 28px 8px 12px;
            border-radius: 4px;
        }
        QMenu::item:selected {
            background: #315A4B;
            color: #FFFFFF;
        }
        QMenu::item:disabled {
            color: #96A29C;
        }
        QMenu::separator {
            height: 1px;
            background: #D7DDD9;
            margin: 4px 8px;
        }
        QDialog#groupDialog QListWidget, QTabWidget#groupTabs::pane {
            background: transparent;
            border: 0;
        }
        QTabWidget#groupTabs QTabBar::tab {
            background: transparent;
            color: #5D6C64;
            border: 0;
            border-bottom: 2px solid transparent;
            padding: 10px 12px;
        }
        QTabWidget#groupTabs QTabBar::tab:selected {
            color: #315A4B;
            border-bottom-color: #315A4B;
        }
        QTabWidget#groupTabs QTabBar::tab:hover {
            background: #F0F4F1;
        }
        QLabel#groupOverviewTitle {
            font-size: 22px;
            font-weight: 600;
            color: #294B3E;
        }
        QLabel#groupOverviewCount, QLabel#groupOverviewPinned, QLabel#groupOverviewInvite,
        QLabel#groupDetailHint, QLabel#groupJoinRequestsStatus, QLabel#groupStatus,
        QLabel#messageSearchStatus, QLabel#messageSearchHelp, QLabel#messageSearchCount, QLabel#attachmentStatus {
            font-size: 13px;
            color: #5D6C64;
        }
        QLabel#groupSectionHeading {
            font-size: 13px;
            font-weight: 600;
            color: #5D6C64;
        }
        QPlainTextEdit#groupAnnouncementEdit, QPlainTextEdit#editMessageText {
            background: #FFFFFF;
            border: 1px solid #DDD9D0;
            border-radius: 10px;
            padding: 8px;
        }
        QPlainTextEdit#groupAnnouncementEdit:focus, QPlainTextEdit#editMessageText:focus {
            border-color: #547C68;
        }
        QDialog QPushButton, QPushButton#newFriendsButton, QToolButton#messageSearchButton, QToolButton#sendAttachmentButton {
            min-height: 32px;
            padding: 0 12px;
            background: #FFFFFF;
            color: #315A4B;
            border: 1px solid #AFC0B8;
            border-radius: 8px;
        }
        QDialog QPushButton:disabled, QPushButton#newFriendsButton:disabled, QToolButton#messageSearchButton:disabled, QToolButton#sendAttachmentButton:disabled {
            color: #96A29C;
            border-color: #D7DDD9;
        }
        QDialog#createGroupDialog QPushButton:hover, QDialog#joinGroupDialog QPushButton:hover,
        QDialog#groupInviteDialog QPushButton:hover, QDialog#confirmationDialog QPushButton:hover,
        QDialog#editMessageDialog QPushButton:hover {
            background: #F0F4F1;
        }
        QDialog#createGroupDialog QPushButton:focus, QDialog#joinGroupDialog QPushButton:focus,
        QDialog#groupInviteDialog QPushButton:focus, QDialog#confirmationDialog QPushButton:focus,
        QDialog#editMessageDialog QPushButton:focus {
            border-color: #547C68;
        }
        QDialog#createGroupDialog QPushButton#groupNextButton, QDialog#joinGroupDialog QPushButton#joinGroupButton,
        QDialog#groupInviteDialog QPushButton#groupInviteSubmitButton,
        QDialog#editMessageDialog QPushButton#editMessageButton {
            background: #315A4B;
            color: #FFFFFF;
            border: 2px solid transparent;
        }
        QDialog#createGroupDialog QPushButton#groupNextButton:hover, QDialog#joinGroupDialog QPushButton#joinGroupButton:hover,
        QDialog#groupInviteDialog QPushButton#groupInviteSubmitButton:hover,
        QDialog#editMessageDialog QPushButton#editMessageButton:hover {
            background: #294D40;
        }
        QDialog#createGroupDialog QPushButton#groupNextButton:focus, QDialog#joinGroupDialog QPushButton#joinGroupButton:focus,
        QDialog#groupInviteDialog QPushButton#groupInviteSubmitButton:focus,
        QDialog#editMessageDialog QPushButton#editMessageButton:focus {
            border-color: #88A697;
        }
        QDialog#createGroupDialog QPushButton#groupNextButton:disabled, QDialog#joinGroupDialog QPushButton#joinGroupButton:disabled,
        QDialog#groupInviteDialog QPushButton#groupInviteSubmitButton:disabled,
        QDialog#editMessageDialog QPushButton#editMessageButton:disabled {
            background: #E7EEE9;
            color: #78897F;
        }
        QListWidget#groupContactPicker, QListWidget#groupNamingMembers, QListWidget#groupSelectedContacts {
            background: transparent;
            border: 0;
            outline: 0;
        }
        QListWidget#groupContactPicker::item, QListWidget#groupNamingMembers::item {
            padding: 0 8px;
            border-radius: 8px;
        }
        QListWidget#groupContactPicker::item:hover { background: #F0F4F1; }
        QListWidget#groupNamingMembers:focus { border: 1px solid #789487; border-radius: 8px; }
        QListWidget#groupContactPicker::item:selected { background: #E7EEE9; color: #27332E; }
        QListWidget#groupContactPicker::indicator, QDialog#groupInviteDialog QListWidget::indicator {
            width: 16px;
            height: 16px;
            border: 1px solid #AFC0B8;
            border-radius: 4px;
            background: #FFFFFF;
        }
        QListWidget#groupContactPicker::indicator:checked, QDialog#groupInviteDialog QListWidget::indicator:checked {
            background: #315A4B;
            border-color: #315A4B;
            image: url(:/icons/check.svg);
        }
        QListWidget#groupSelectedContacts::item {
            background: #E7EEE9;
            color: #315A4B;
            border-radius: 8px;
            padding: 0 8px;
        }
        QLabel#groupSelectedCount, QLabel#groupNamingSummary { color: #5D6C64; font-size: 13px; }
        QDialog#confirmationDialog QLabel#qt_msgbox_label, QDialog#noticeDialog QLabel#qt_msgbox_label {
            min-width: 340px;
            max-width: 430px;
        }
        QDialog#confirmationDialog QPushButton#confirmationActionButton {
            background: #A64C48;
            color: #FFFFFF;
            border: 2px solid transparent;
        }
        QDialog#confirmationDialog QPushButton#confirmationActionButton:hover { background: #913F3B; }
        QDialog#confirmationDialog QPushButton#confirmationActionButton:focus { border-color: #DDAEAA; }
        QDialog#groupDialog QPushButton:hover,
        QDialog#messageSearchDialog QPushButton:hover, QDialog#attachmentDialog QPushButton:hover {
            background: #F0F4F1;
        }
        QDialog#groupDialog QPushButton:pressed,
        QDialog#messageSearchDialog QPushButton:pressed, QDialog#attachmentDialog QPushButton:pressed {
            background: #E7EEE9;
        }
        QDialog#groupDialog QPushButton:focus,
        QDialog#messageSearchDialog QPushButton:focus, QDialog#attachmentDialog QPushButton:focus {
            border-color: #547C68;
        }
        QDialog#groupDialog QPushButton#groupReadAnnouncementButton,
        QDialog#groupDialog QPushButton#groupAllMembersButton,
        QDialog#groupDialog QPushButton#groupManageButton,
        QDialog#groupDialog QPushButton#groupLeaveButton {
            background: transparent;
            border: 2px solid transparent;
            padding: 0 8px;
        }
        QDialog#groupDialog QPushButton#groupReadAnnouncementButton:hover,
        QDialog#groupDialog QPushButton#groupAllMembersButton:hover,
        QDialog#groupDialog QPushButton#groupManageButton:hover {
            background: #F0F4F1;
        }
        QDialog#groupDialog QPushButton#groupReadAnnouncementButton:focus,
        QDialog#groupDialog QPushButton#groupAllMembersButton:focus,
        QDialog#groupDialog QPushButton#groupManageButton:focus,
        QDialog#groupDialog QPushButton#groupLeaveButton:focus {
            border-color: #547C68;
        }
        QDialog#groupDialog QPushButton#groupRenameButton,
        QDialog#groupDialog QPushButton#groupAnnouncementButton,
        QDialog#groupDialog QPushButton#groupAcceptRequestButton,
        QDialog#messageSearchDialog QPushButton#searchMessagesButton,
        QDialog#attachmentDialog QPushButton#saveAttachmentButton {
            background: #315A4B;
            color: #FFFFFF;
            border: 2px solid transparent;
        }
        QDialog#groupDialog QPushButton#groupRenameButton:hover,
        QDialog#groupDialog QPushButton#groupAnnouncementButton:hover,
        QDialog#groupDialog QPushButton#groupAcceptRequestButton:hover,
        QDialog#messageSearchDialog QPushButton#searchMessagesButton:hover,
        QDialog#attachmentDialog QPushButton#saveAttachmentButton:hover {
            background: #294D40;
        }
        QDialog#groupDialog QPushButton#groupRenameButton:focus,
        QDialog#groupDialog QPushButton#groupAnnouncementButton:focus,
        QDialog#groupDialog QPushButton#groupAcceptRequestButton:focus,
        QDialog#messageSearchDialog QPushButton#searchMessagesButton:focus,
        QDialog#attachmentDialog QPushButton#saveAttachmentButton:focus {
            border-color: #88A697;
        }
        QDialog#groupDialog QPushButton#groupRenameButton:disabled,
        QDialog#groupDialog QPushButton#groupAnnouncementButton:disabled,
        QDialog#groupDialog QPushButton#groupAcceptRequestButton:disabled,
        QDialog#messageSearchDialog QPushButton#searchMessagesButton:disabled,
        QDialog#attachmentDialog QPushButton#saveAttachmentButton:disabled {
            background: #E7EEE9;
            color: #78897F;
        }
        QDialog#groupDialog QPushButton#groupLeaveButton,
        QDialog#groupDialog QPushButton#groupRevokeInviteButton,
        QDialog#groupDialog QPushButton#groupClearAnnouncementButton {
            color: #A64C48;
        }
        QDialog#groupDialog QPushButton#groupRevokeInviteButton:disabled,
        QDialog#groupDialog QPushButton#groupClearAnnouncementButton:disabled {
            color: #96A29C;
        }
        QWidget#loginPage {
            background: #F7F5EF;
        }
        QFrame#loginCard {
            background: #FFFEFA;
            border: 1px solid #E8E4DA;
            border-radius: 18px;
        }
        QLabel#loginTitle {
            font-size: 26px;
            font-weight: 700;
            color: #294B3E;
        }
        QLabel#authMark {
            background: #315A4B;
            border-radius: 12px;
        }
        QLabel#authSubtitle {
            color: #5D6C64;
            font-size: 13px;
        }
        QFrame#loginCard QLabel#subtleText, QDialog#registrationDialog QLabel#subtleText {
            color: #5D6C64;
            font-size: 13px;
        }
        QLineEdit {
            min-height: 38px;
            padding: 0 12px;
            background: #FFFFFF;
            border: 1px solid #DDD9D0;
            border-radius: 10px;
            selection-background-color: #315A4B;
        }
        QLineEdit:focus {
            border-color: #789487;
        }
        QFrame#loginCard QLineEdit:focus, QDialog#registrationDialog QLineEdit:focus {
            border-color: #547C68;
        }
        QPushButton#loginButton, QPushButton#registrationSubmitButton {
            min-height: 44px;
            background: #315A4B;
            color: #FFFFFF;
            border: 2px solid transparent;
            border-radius: 10px;
            font-weight: 600;
        }
        QPushButton#loginButton:hover, QPushButton#registrationSubmitButton:hover {
            background: #294D40;
        }
        QPushButton#loginButton:pressed, QPushButton#registrationSubmitButton:pressed {
            background: #234536;
        }
        QPushButton#loginButton:focus, QPushButton#registrationSubmitButton:focus {
            border-color: #88A697;
        }
        QPushButton#loginButton:disabled, QPushButton#registrationSubmitButton:disabled {
            background: #AEBDB6;
        }
        QPushButton#registerButton, QPushButton#registrationCancelButton {
            min-height: 28px;
            background: transparent;
            color: #315A4B;
            border: 2px solid transparent;
            border-radius: 8px;
        }
        QPushButton#registerButton:hover, QPushButton#registrationCancelButton:hover {
            background: #F0F4F1;
        }
        QPushButton#registerButton:focus, QPushButton#registrationCancelButton:focus {
            border-color: #547C68;
        }
        QPushButton#registerButton:disabled, QPushButton#registrationCancelButton:disabled {
            color: #96A29C;
        }
        QLabel#registrationTitle {
            font-size: 22px;
            font-weight: 700;
            color: #294B3E;
        }
        QToolButton#serverSettingsButton {
            min-height: 28px;
            padding: 0 8px;
            background: transparent;
            color: #5D6C64;
            border: 2px solid transparent;
            border-radius: 8px;
            font-size: 13px;
        }
        QToolButton#serverSettingsButton:hover {
            background: #F0F4F1;
        }
        QToolButton#serverSettingsButton:focus {
            border-color: #547C68;
        }
        QFrame#navigationPanel {
            background: #294F40;
        }
        QLabel#brandLabel {
            background: transparent;
            color: #F4F7F5;
            font-size: 19px;
            font-weight: 700;
        }
        QLabel#profileAvatar {
            background: #E7EFEA;
            color: #315A4B;
            border-radius: 22px;
            font-size: 18px;
            font-weight: 700;
        }
        QToolButton#navigationSelected, QToolButton#navigationButton {
            border: 0;
            border-radius: 12px;
            padding: 6px 4px 5px 4px;
            color: #D8E4DE;
            background: transparent;
            font-size: 12px;
            font-weight: 500;
        }
        QToolButton#navigationSelected {
            background: rgba(255, 255, 255, 0.13);
            color: #FFFFFF;
        }
        QToolButton#navigationButton:disabled {
            color: rgba(216, 228, 222, 0.43);
        }
        QFrame#conversationPanel {
            background: #FCFBF7;
        }
        QFrame#chatPanel {
            background: #F7F5EF;
        }
        QLabel#sectionTitle {
            font-size: 20px;
            font-weight: 700;
            color: #27362F;
        }
        QToolButton#sidebarHeaderButton, QToolButton#sidebarTextButton, QToolButton#chatsActionsButton {
            border: 0;
            background: transparent;
            color: #315A4B;
            font-weight: 600;
        }
        QToolButton#sidebarHeaderButton, QToolButton#chatsActionsButton {
            font-size: 26px;
        }
        QToolButton#sidebarTextButton {
            padding: 6px 4px;
            font-size: 13px;
        }
        QToolButton#sidebarHeaderButton:hover, QToolButton#sidebarTextButton:hover, QToolButton#chatsActionsButton:hover {
            background: #EEF1ED;
            border-radius: 8px;
        }
        QMenu#chatsActionsMenu {
            background: #FFFEFA;
            color: #27332E;
            border: 1px solid #D7DDD9;
            padding: 4px;
        }
        QMenu#chatsActionsMenu::item {
            padding: 8px 16px;
        }
        QMenu#chatsActionsMenu::item:selected {
            background: #EEF1ED;
        }
        QLabel#subtleText {
            color: #8B918D;
        }
        QLabel#friendRequestsStatus, QLabel#friendRequestHeading {
            color: #5D6C64;
            font-size: 13px;
        }
        QLabel#friendRequestHeading {
            font-weight: 600;
        }
        QFrame#chatHeader {
            background: transparent;
        }
        QPushButton#chatHeaderButton {
            min-height: 38px;
            padding: 0 10px 0 2px;
            border: 0;
            border-radius: 12px;
            background: transparent;
            color: #27362F;
            text-align: left;
            font-size: 17px;
            font-weight: 700;
        }
        QPushButton#chatHeaderButton:hover:enabled {
            background: #EEEDE7;
        }
        QPushButton#chatHeaderButton:disabled {
            color: #27362F;
        }
        QLabel#chatPresence, QLabel#chatPresenceOnline {
            background: transparent;
            font-size: 12px;
        }
        QLabel#chatPresence {
            color: #8B918D;
        }
        QLabel#chatPresenceOnline {
            color: #4F8A70;
        }
        QToolButton#connectionStatusButton {
            min-height: 26px;
            padding: 0 10px;
            border: 0;
            border-radius: 13px;
            background: #ECEBE6;
            color: #747C78;
            font-size: 12px;
            font-weight: 500;
        }
        QToolButton#connectionStatusButton:hover:enabled {
            background: #E2E8E3;
            color: #315A4B;
        }
        QToolButton#connectionStatusButton:disabled {
            color: #747C78;
        }
        QToolButton#headerActionButton {
            border: 0;
            border-radius: 18px;
            background: transparent;
        }
        QToolButton#headerActionButton:hover:enabled {
            background: #ECEBE5;
        }
        QDialog#profileDialog {
            border: 1px solid #D8D6D0;
            border-radius: 12px;
        }
        QFrame#profileHeaderSection, QFrame#profileInfoSection {
            background: #FFFEFA;
            border: 0;
        }
        QFrame#profileSectionSeparator {
            background: #F1F0ED;
            border: 0;
        }
        QToolButton#profileCloseButton {
            border: 0;
            border-radius: 18px;
            background: transparent;
        }
        QToolButton#profileCloseButton:hover {
            background: #F0EFEC;
        }
        QLabel#profileDialogAvatar {
            background: #E7EFEA;
            color: #315A4B;
            border-radius: 52px;
            font-size: 34px;
            font-weight: 700;
        }
        QLabel#profileDialogName {
            color: #1F2623;
            font-size: 22px;
            font-weight: 600;
        }
        QLabel#profileRelationship {
            color: #5D6C64;
            font-size: 13px;
        }
        QToolButton#profileActionButton {
            padding: 0 12px;
            background: transparent;
            color: #315A4B;
            border: 2px solid transparent;
            border-radius: 8px;
            font-size: 13px;
            font-weight: 500;
        }
        QToolButton#profileActionButton:hover {
            background: #F0F4F1;
        }
        QToolButton#profileActionButton:pressed {
            background: #E7EEE9;
        }
        QToolButton#profileActionButton:focus {
            border-color: #547C68;
        }
        QDialog#profileDialog QPushButton {
            background: transparent;
            border: 2px solid transparent;
        }
        QToolButton#profileActionButton[primary="true"], QDialog#profileDialog QPushButton#changeAvatarButton {
            background: #315A4B;
            color: #FFFFFF;
            border: 2px solid transparent;
        }
        QToolButton#profileActionButton[primary="true"]:hover, QDialog#profileDialog QPushButton#changeAvatarButton:hover {
            background: #294D40;
        }
        QToolButton#profileActionButton[primary="true"]:focus, QDialog#profileDialog QPushButton#changeAvatarButton:focus {
            border-color: #88A697;
        }
        QToolButton#profileActionButton[primary="true"]:disabled, QDialog#profileDialog QPushButton#changeAvatarButton:disabled {
            background: #E7EEE9;
            color: #78897F;
        }
        QDialog#profileDialog QPushButton#profileLogoutButton, QDialog#profileDialog QPushButton#removeContactButton,
        QDialog#profileDialog QPushButton#rejectFriendRequestButton, QDialog#profileDialog QPushButton#cancelFriendRequestButton {
            color: #A64C48;
        }
        QDialog#profileDialog QPushButton:hover {
            background: #F0F4F1;
        }
        QDialog#profileDialog QPushButton:focus {
            border-color: #547C68;
        }
        QDialog#profileDialog QPushButton:disabled,
        QDialog#profileDialog QPushButton#profileLogoutButton:disabled, QDialog#profileDialog QPushButton#removeContactButton:disabled,
        QDialog#profileDialog QPushButton#rejectFriendRequestButton:disabled, QDialog#profileDialog QPushButton#cancelFriendRequestButton:disabled {
            color: #96A29C;
        }
        QFrame#separator {
            background: #E8E4DB;
            border: 0;
        }
        QListView#conversationList, QListView#userList, QListView#messageList, QListView#messageSearchResults,
        QListWidget#incomingFriendRequests, QListWidget#outgoingFriendRequests {
            border: 0;
            outline: 0;
            background: transparent;
        }
        QListView#conversationList, QListView#userList {
            padding: 0;
        }
        QLineEdit#userSearchEdit {
            margin: 0 12px 0 12px;
            min-height: 36px;
            background: #F1F2EF;
            border: 0;
            border-radius: 18px;
            padding: 0 14px;
        }
        QLineEdit#userSearchEdit:focus {
            border: 1px solid #A9B8B1;
        }
        QListView#messageList {
            padding: 0 2px 6px 2px;
        }
        QScrollBar:vertical {
            width: 6px;
            margin: 6px 1px;
            background: transparent;
        }
        QScrollBar::handle:vertical {
            min-height: 32px;
            background: rgba(70, 91, 82, 0.14);
            border-radius: 3px;
        }
        QScrollBar::handle:vertical:hover {
            background: rgba(70, 91, 82, 0.34);
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            width: 0;
            height: 0;
            background: transparent;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: transparent;
        }
        QScrollBar:horizontal {
            height: 0;
        }
        QFrame#horizontalSeparator {
            background: #E8E4DB;
            border: 0;
            max-height: 1px;
        }
        QFrame#inputBar {
            background: #FFFEFA;
            border: 0;
            border-radius: 0;
        }
        QPlainTextEdit#messageEdit {
            min-height: 36px;
            padding: 0 4px;
            background: transparent;
            border: 0;
            border-radius: 0;
        }
        QPlainTextEdit#messageEdit:focus {
            border: 0;
        }
        QToolButton#sendButton {
            border: 0;
            border-radius: 20px;
            background: transparent;
        }
        QToolButton#sendButton:hover:enabled {
            background: #E9EEE9;
        }
        QToolButton#sendButton:disabled {
            background: transparent;
        }
        QToolButton#cancelReplyButton {
            border: 1px solid transparent;
            border-radius: 18px;
            background: transparent;
        }
        QToolButton#cancelReplyButton:hover {
            background: #E9EEE9;
        }
        QToolButton#cancelReplyButton:focus {
            border-color: #547C68;
        }
    )");
}
