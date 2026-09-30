#include "main_window.hpp"

#include <memory>
#include <utility>

#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "chat_widget.hpp"
#include "client_bridge.hpp"

main_window::main_window(QString server_url, QWidget* parent)
    : QMainWindow(parent), client_(std::make_unique<client_bridge>())
{
    setWindowTitle(QStringLiteral("Chat"));
    resize(1180, 760);
    setMinimumSize(980, 640);
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget {
            background: #F7F5EF;
            color: #2B3832;
            font-size: 14px;
        }
        QFrame#loginCard {
            background: #FFFEFA;
            border: 1px solid #E7E3D9;
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
            border: 1px solid #DCD9D0;
            border-radius: 10px;
            selection-background-color: #315A4B;
        }
        QLineEdit:focus {
            border-color: #6F8D80;
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
            background: #284C3F;
        }
        QPushButton#loginButton:disabled {
            background: #AEBDB6;
        }
        QFrame#navigationPanel {
            background: #315A4B;
        }
        QLabel#profileAvatar {
            background: #E5EEE8;
            color: #315A4B;
            border-radius: 22px;
            font-size: 18px;
            font-weight: 700;
        }
        QLabel#profileName {
            color: #DCE8E1;
            font-size: 12px;
        }
        QPushButton#navigationSelected, QPushButton#navigationButton {
            border: 0;
            border-radius: 10px;
            color: #DCE8E1;
            background: transparent;
            font-weight: 600;
        }
        QPushButton#navigationSelected {
            background: rgba(255, 255, 255, 0.15);
            color: #FFFFFF;
        }
        QPushButton#navigationButton:disabled {
            color: rgba(220, 232, 225, 0.42);
        }
        QFrame#conversationPanel, QFrame#detailPanel {
            background: #FBFAF6;
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
        QLabel#detailAvatar {
            background: #E5EEE8;
            color: #315A4B;
            border-radius: 38px;
            font-size: 26px;
            font-weight: 700;
        }
        QLabel#detailName {
            color: #27362F;
            font-size: 18px;
            font-weight: 700;
        }
        QFrame#detailCard {
            background: #FFFEFA;
            border: 1px solid #E7E3D9;
            border-radius: 14px;
        }
        QLabel#detailFieldLabel {
            color: #8B918D;
            font-size: 12px;
        }
        QLabel#detailFieldValue {
            color: #2B3832;
            font-weight: 600;
        }
        QFrame#separator {
            background: #E6E3DB;
            border: 0;
        }
        QListView#conversationList {
            border: 0;
            outline: 0;
            background: transparent;
            padding: 4px 0;
        }
        QListView#messageList {
            border: 0;
            outline: 0;
            background: transparent;
            padding: 6px 0;
        }
        QListView#conversationList QScrollBar:vertical,
        QListView#messageList QScrollBar:vertical {
            width: 7px;
            margin: 2px 0;
            background: transparent;
        }
        QListView#conversationList QScrollBar::handle:vertical,
        QListView#messageList QScrollBar::handle:vertical {
            min-height: 36px;
            background: #C4CAC6;
            border-radius: 3px;
        }
        QListView#conversationList QScrollBar::handle:vertical:hover,
        QListView#messageList QScrollBar::handle:vertical:hover {
            background: #AAB4AF;
        }
        QListView#conversationList QScrollBar::add-line:vertical,
        QListView#conversationList QScrollBar::sub-line:vertical,
        QListView#messageList QScrollBar::add-line:vertical,
        QListView#messageList QScrollBar::sub-line:vertical {
            width: 0;
            height: 0;
            background: transparent;
        }
        QListView#conversationList QScrollBar::add-page:vertical,
        QListView#conversationList QScrollBar::sub-page:vertical,
        QListView#messageList QScrollBar::add-page:vertical,
        QListView#messageList QScrollBar::sub-page:vertical {
            background: transparent;
        }
        QFrame#horizontalSeparator {
            background: #E6E3DB;
            border: 0;
            max-height: 1px;
        }
        QLineEdit#messageEdit {
            min-height: 42px;
            background: #FFFEFA;
            border-color: #DDD9D0;
            border-radius: 12px;
        }
        QPushButton#sendButton {
            min-width: 76px;
            min-height: 42px;
            border: 0;
            border-radius: 12px;
            background: #315A4B;
            color: #FFFFFF;
            font-weight: 600;
        }
        QPushButton#sendButton:hover {
            background: #284C3F;
        }
        QPushButton#sendButton:disabled {
            background: #AEBDB6;
        }
    )"));

    pages_ = new QStackedWidget(this);
    setCentralWidget(pages_);

    login_page_ = new QWidget(pages_);
    auto* login_outer = new QVBoxLayout(login_page_);
    login_outer->setContentsMargins(32, 32, 32, 32);
    login_outer->addStretch();

    auto* login_card = new QFrame(login_page_);
    login_card->setObjectName(QStringLiteral("loginCard"));
    login_card->setFixedWidth(420);
    auto* login_layout = new QVBoxLayout(login_card);
    login_layout->setContentsMargins(34, 30, 34, 30);
    login_layout->setSpacing(18);

    auto* title = new QLabel(QStringLiteral("登录 Chat"), login_card);
    title->setObjectName(QStringLiteral("loginTitle"));
    login_layout->addWidget(title);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(14);
    server_edit_ = new QLineEdit(std::move(server_url), login_card);
    username_edit_ = new QLineEdit(login_card);
    password_edit_ = new QLineEdit(login_card);
    password_edit_->setEchoMode(QLineEdit::Password);
    username_edit_->setPlaceholderText(QStringLiteral("用户名"));
    password_edit_->setPlaceholderText(QStringLiteral("密码"));

    form->addRow(QStringLiteral("服务器"), server_edit_);
    form->addRow(QStringLiteral("用户名"), username_edit_);
    form->addRow(QStringLiteral("密码"), password_edit_);
    login_layout->addLayout(form);

    login_button_ = new QPushButton(QStringLiteral("登录"), login_card);
    login_button_->setObjectName(QStringLiteral("loginButton"));
    login_button_->setDefault(true);
    login_layout->addWidget(login_button_);

    status_label_ = new QLabel(login_card);
    status_label_->setObjectName(QStringLiteral("subtleText"));
    status_label_->setWordWrap(true);
    login_layout->addWidget(status_label_);

    login_outer->addWidget(login_card, 0, Qt::AlignHCenter);
    login_outer->addStretch();

    chat_page_ = new chat_widget(pages_);

    pages_->addWidget(login_page_);
    pages_->addWidget(chat_page_);

    connect(login_button_, &QPushButton::clicked, this, [this] { start_login(); });
    connect(password_edit_, &QLineEdit::returnPressed, this, [this] { start_login(); });

    connect(client_.get(), &client_bridge::connected, this, [this] {
        connected_ = true;
        server_edit_->setEnabled(false);
        if (login_pending_)
        {
            authenticate();
        }
    }, Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::disconnected, this, [this] {
        connected_ = false;
        login_pending_ = false;
        set_login_busy(false);
        server_edit_->setEnabled(true);
        if (pages_->currentWidget() == chat_page_)
        {
            pages_->setCurrentWidget(login_page_);
            status_label_->setText(QStringLiteral("连接已断开"));
        }
    }, Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::error, this, [this](QString const& message) {
        if (pages_->currentWidget() == login_page_)
        {
            show_login_error(message);
        }
        else
        {
            chat_page_->set_error(message);
        }
    }, Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::authentication_finished, this,
            [this](bool authenticated, QString const& error_message) {
                login_pending_ = false;
                if (!error_message.isEmpty())
                {
                    show_login_error(error_message);
                    return;
                }
                if (!authenticated)
                {
                    show_login_error(QStringLiteral("用户名或密码错误"));
                    return;
                }

                pending_password_.clear();
                status_label_->clear();
                show_authenticated_page();
            },
            Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::conversations_received, this,
            [this](QList<conversation_data> conversations, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_error(error_message);
                    return;
                }
                chat_page_->set_conversations(std::move(conversations));
            },
            Qt::QueuedConnection);

    connect(chat_page_, &chat_widget::conversation_selected, this,
            [this](qint64 user) { client_->get_messages(user); });

    connect(chat_page_, &chat_widget::older_messages_requested, this,
            [this](qint64 user, qint64 before) { client_->get_messages(user, before); });

    connect(chat_page_, &chat_widget::send_message_requested, this,
            [this](qint64 user, QString text) { client_->send_message(user, std::move(text)); });

    connect(client_.get(), &client_bridge::messages_received, this,
            [this](qint64 user, QList<message_data> messages, bool older, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_message_error(user, error_message);
                    return;
                }

                chat_page_->set_messages(user, std::move(messages), older);
                if (!older && chat_page_->active_user() == user)
                {
                    auto const message = chat_page_->latest_message_id();
                    if (message > 0)
                    {
                        client_->mark_read(user, message);
                    }
                }
            },
            Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::message_received, this,
            [this](message_data message) {
                auto const user = message.from;
                auto const message_id = message.id;
                chat_page_->add_message(user, std::move(message));
                if (chat_page_->active_user() == user)
                {
                    client_->mark_read(user, message_id);
                }
                else
                {
                    client_->get_conversations();
                }
            },
            Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::message_sent, this,
            [this](qint64 user, QString text, qint64 message, bool realtime, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_message_error(user, error_message);
                    return;
                }

                chat_page_->add_sent_message(user, message, std::move(text), realtime);
                client_->get_conversations();
            },
            Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::read_marked, this,
            [this](qint64 user, qint64 message, QString const& error_message) {
                (void)message;
                if (!error_message.isEmpty())
                {
                    chat_page_->set_message_error(user, error_message);
                    return;
                }
                client_->get_conversations();
            },
            Qt::QueuedConnection);
}

main_window::~main_window() { client_.reset(); }

void main_window::start_login()
{
    auto const server = server_edit_->text().trimmed();
    auto const username = username_edit_->text();
    auto const password = password_edit_->text();

    if (server.isEmpty() || username.isEmpty() || password.isEmpty())
    {
        status_label_->setText(QStringLiteral("服务器、用户名和密码不能为空"));
        return;
    }

    pending_username_ = username;
    pending_password_ = password;
    login_pending_ = true;
    set_login_busy(true);

    if (connected_)
    {
        authenticate();
        return;
    }

    status_label_->setText(QStringLiteral("正在连接…"));
    client_->connect_to_server(server);
}

void main_window::authenticate()
{
    status_label_->setText(QStringLiteral("正在认证…"));
    client_->authenticate(pending_username_, pending_password_);
}

void main_window::set_login_busy(bool busy)
{
    username_edit_->setEnabled(!busy);
    password_edit_->setEnabled(!busy);
    login_button_->setEnabled(!busy);
    server_edit_->setEnabled(!busy && !connected_);
}

void main_window::show_login_error(QString message)
{
    login_pending_ = false;
    pending_password_.clear();
    status_label_->setText(std::move(message));
    set_login_busy(false);
}

void main_window::show_authenticated_page()
{
    chat_page_->set_user(pending_username_);
    chat_page_->set_loading();
    pages_->setCurrentWidget(chat_page_);
    client_->get_conversations();
}
