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
            background: #FBFAF6;
            border: 1px solid #E1DED5;
            border-radius: 18px;
        }
        QToolButton#profileCloseButton {
            border: 0;
            border-radius: 18px;
            background: transparent;
        }
        QToolButton#profileCloseButton:hover {
            background: #ECEAE3;
        }
        QLabel#profileDialogAvatar {
            background: #E7EFEA;
            color: #315A4B;
            border-radius: 44px;
            font-size: 30px;
            font-weight: 700;
        }
        QLabel#profileDialogName {
            color: #27362F;
            font-size: 21px;
            font-weight: 700;
        }
        QLabel#profileDialogSecondary {
            color: #8B918D;
        }
        QToolButton#profileActionButton {
            min-height: 68px;
            padding: 8px 8px 6px 8px;
            background: #FFFEFA;
            color: #315A4B;
            border: 1px solid #E5E1D8;
            border-radius: 14px;
            font-size: 12px;
            font-weight: 600;
        }
        QToolButton#profileActionButton:hover {
            background: #F0F3EE;
            border-color: #D6DFD9;
        }
        QFrame#profileInfoCard {
            background: #FFFEFA;
            border: 1px solid #E8E4DA;
            border-radius: 14px;
        }
        QLabel#profileInfoValue {
            color: #315A4B;
            font-size: 15px;
            font-weight: 600;
        }
        QLabel#profileInfoLabel {
            color: #8B918D;
            font-size: 12px;
        }
        QFrame#profileInfoSeparator {
            background: #E9E5DC;
            border: 0;
            max-height: 1px;
        }
        QFrame#separator {
            background: #E8E4DB;
            border: 0;
        }
        QListView#conversationList, QListView#messageList {
            border: 0;
            outline: 0;
            background: transparent;
        }
        QListView#conversationList {
            padding: 3px 0;
        }
        QListView#messageList {
            padding: 2px 2px 6px 2px;
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
            border: 1px solid #E3DFD6;
            border-radius: 16px;
        }
        QLineEdit#messageEdit {
            min-height: 38px;
            padding: 0 6px;
            background: transparent;
            border: 0;
            border-radius: 0;
        }
        QToolButton#sendButton {
            border: 0;
            border-radius: 18px;
            background: #315A4B;
        }
        QToolButton#sendButton:hover {
            background: #294D40;
        }
        QToolButton#sendButton:disabled {
            background: #B7C3BD;
        }
    )");
}
