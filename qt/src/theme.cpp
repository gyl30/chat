#include "theme.hpp"

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
        QPushButton#loginButton {
            min-height: 40px;
            background: #315A4B;
            color: #FFFFFF;
            border: 0;
            border-radius: 10px;
            font-weight: 600;
        }
        QPushButton#loginButton:hover {
            background: #294D40;
        }
        QPushButton#loginButton:disabled {
            background: #AEBDB6;
        }
        QPushButton#registerButton {
            min-height: 40px;
            background: #FFFFFF;
            color: #315A4B;
            border: 1px solid #AFC0B8;
            border-radius: 10px;
            font-weight: 600;
        }
        QPushButton#registerButton:hover {
            background: #F0F4F1;
            border-color: #789487;
        }
        QPushButton#registerButton:disabled {
            color: #96A29C;
            border-color: #D7DDD9;
        }
        QDialog#registrationDialog {
            background: #FFFEFA;
        }
        QLabel#registrationTitle {
            font-size: 22px;
            font-weight: 700;
            color: #294B3E;
        }
        QPushButton#registrationSubmitButton {
            min-height: 40px;
            background: #315A4B;
            color: #FFFFFF;
            border: 0;
            border-radius: 10px;
            font-weight: 600;
        }
        QPushButton#registrationSubmitButton:hover {
            background: #294D40;
        }
        QPushButton#registrationSubmitButton:disabled {
            background: #AEBDB6;
        }
        QPushButton#registrationCancelButton {
            min-height: 40px;
            background: #FFFFFF;
            color: #315A4B;
            border: 1px solid #AFC0B8;
            border-radius: 10px;
            font-weight: 600;
        }
        QPushButton#registrationCancelButton:hover {
            background: #F0F4F1;
            border-color: #789487;
        }
        QPushButton#registrationCancelButton:disabled {
            color: #96A29C;
            border-color: #D7DDD9;
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
        QToolButton#sidebarHeaderButton, QToolButton#sidebarTextButton {
            border: 0;
            background: transparent;
            color: #315A4B;
            font-weight: 600;
        }
        QToolButton#sidebarHeaderButton {
            font-size: 26px;
        }
        QToolButton#sidebarTextButton {
            padding: 6px 4px;
            font-size: 13px;
        }
        QToolButton#sidebarHeaderButton:hover, QToolButton#sidebarTextButton:hover {
            background: #EEF1ED;
            border-radius: 8px;
        }
        QLabel#subtleText {
            color: #8B918D;
        }
        QFrame#chatHeader {
            background: transparent;
        }
        QPushButton#chatHeaderButton {
            min-height: 44px;
            padding: 2px 10px 2px 2px;
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
        QToolButton#headerActionButton {
            border: 0;
            border-radius: 18px;
            background: transparent;
        }
        QToolButton#headerActionButton:hover:enabled {
            background: #ECEBE5;
        }
        QDialog#profileDialog {
            background: #FFFEFA;
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
        QToolButton#profileActionButton {
            padding: 9px 8px 7px 8px;
            background: #FFFEFA;
            color: #315A4B;
            border: 1px solid #DFDDD7;
            border-radius: 12px;
            font-size: 13px;
            font-weight: 500;
        }
        QToolButton#profileActionButton:hover {
            background: #F4F5F2;
            border-color: #D5D9D4;
        }
        QLabel#profileInfoValue {
            color: #315A4B;
            font-size: 16px;
            font-weight: 500;
        }
        QLabel#profileInfoLabel {
            color: #8B918D;
            font-size: 13px;
        }
        QFrame#separator {
            background: #E8E4DB;
            border: 0;
        }
        QListView#conversationList, QListView#userList, QListView#messageList {
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
        QLineEdit#messageEdit {
            min-height: 36px;
            padding: 0 4px;
            background: transparent;
            border: 0;
            border-radius: 0;
        }
        QLineEdit#messageEdit:focus {
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
    )");
}
