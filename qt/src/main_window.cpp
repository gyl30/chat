#include "main_window.hpp"

#include <memory>
#include <utility>

#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include "theme.hpp"

main_window::main_window(QString server_url, QWidget* parent)
    : QMainWindow(parent), client_(std::make_unique<client_bridge>())
{
    setWindowTitle(QStringLiteral("Chat"));
    resize(1180, 760);
    setMinimumSize(980, 640);
    setStyleSheet(chat_style_sheet());

    pages_ = new QStackedWidget(this);
    setCentralWidget(pages_);

    login_page_ = new QWidget(pages_);
    login_page_->setObjectName(QStringLiteral("loginPage"));
    auto* login_outer = new QVBoxLayout(login_page_);
    login_outer->setContentsMargins(32, 32, 32, 32);
    login_outer->addStretch();

    auto* login_card = new QFrame(login_page_);
    login_card->setObjectName(QStringLiteral("loginCard"));
    login_card->setFixedWidth(420);
    auto* login_layout = new QVBoxLayout(login_card);
    login_layout->setContentsMargins(34, 30, 34, 30);
    login_layout->setSpacing(18);

    auto* title = new QLabel(QStringLiteral("Chat"), login_card);
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

    auto* auth_buttons = new QHBoxLayout;
    auth_buttons->setSpacing(10);
    login_button_ = new QPushButton(QStringLiteral("登录"), login_card);
    login_button_->setObjectName(QStringLiteral("loginButton"));
    login_button_->setDefault(true);
    auth_buttons->addWidget(login_button_, 1);
    register_button_ = new QPushButton(QStringLiteral("注册"), login_card);
    register_button_->setObjectName(QStringLiteral("registerButton"));
    auth_buttons->addWidget(register_button_, 1);
    login_layout->addLayout(auth_buttons);

    status_label_ = new QLabel(login_card);
    status_label_->setObjectName(QStringLiteral("subtleText"));
    status_label_->setWordWrap(true);
    login_layout->addWidget(status_label_);

    login_outer->addWidget(login_card, 0, Qt::AlignHCenter);
    login_outer->addStretch();

    registration_dialog_ = new QDialog(this);
    registration_dialog_->setObjectName(QStringLiteral("registrationDialog"));
    registration_dialog_->setWindowTitle(QStringLiteral("注册"));
    registration_dialog_->setModal(true);
    registration_dialog_->setFixedWidth(420);
    auto* registration_layout = new QVBoxLayout(registration_dialog_);
    registration_layout->setContentsMargins(32, 28, 32, 28);
    registration_layout->setSpacing(18);

    auto* registration_title = new QLabel(QStringLiteral("注册新账号"), registration_dialog_);
    registration_title->setObjectName(QStringLiteral("registrationTitle"));
    registration_layout->addWidget(registration_title);

    auto* registration_form = new QFormLayout;
    registration_form->setHorizontalSpacing(16);
    registration_form->setVerticalSpacing(14);
    registration_username_edit_ = new QLineEdit(registration_dialog_);
    registration_password_edit_ = new QLineEdit(registration_dialog_);
    registration_password_confirm_edit_ = new QLineEdit(registration_dialog_);
    registration_password_edit_->setEchoMode(QLineEdit::Password);
    registration_password_confirm_edit_->setEchoMode(QLineEdit::Password);
    registration_username_edit_->setPlaceholderText(QStringLiteral("用户名"));
    registration_password_edit_->setPlaceholderText(QStringLiteral("密码"));
    registration_password_confirm_edit_->setPlaceholderText(QStringLiteral("再次输入密码"));
    registration_form->addRow(QStringLiteral("用户名"), registration_username_edit_);
    registration_form->addRow(QStringLiteral("密码"), registration_password_edit_);
    registration_form->addRow(QStringLiteral("确认密码"), registration_password_confirm_edit_);
    registration_layout->addLayout(registration_form);

    auto* registration_buttons = new QHBoxLayout;
    registration_buttons->setSpacing(10);
    registration_cancel_button_ = new QPushButton(QStringLiteral("取消"), registration_dialog_);
    registration_cancel_button_->setObjectName(QStringLiteral("registrationCancelButton"));
    registration_buttons->addWidget(registration_cancel_button_, 1);
    registration_submit_button_ = new QPushButton(QStringLiteral("注册"), registration_dialog_);
    registration_submit_button_->setObjectName(QStringLiteral("registrationSubmitButton"));
    registration_submit_button_->setDefault(true);
    registration_buttons->addWidget(registration_submit_button_, 1);
    registration_layout->addLayout(registration_buttons);

    registration_status_label_ = new QLabel(registration_dialog_);
    registration_status_label_->setObjectName(QStringLiteral("subtleText"));
    registration_status_label_->setWordWrap(true);
    registration_layout->addWidget(registration_status_label_);

    chat_page_ = new chat_widget(pages_);

    pages_->addWidget(login_page_);
    pages_->addWidget(chat_page_);

    connect(login_button_, &QPushButton::clicked, this, [this] { start_login(); });
    connect(register_button_, &QPushButton::clicked, this, [this] { show_registration_dialog(); });
    connect(password_edit_, &QLineEdit::returnPressed, this, [this] { start_login(); });
    connect(registration_submit_button_, &QPushButton::clicked, this, [this] { start_registration(); });
    connect(registration_password_confirm_edit_, &QLineEdit::returnPressed, this, [this] { start_registration(); });
    connect(registration_cancel_button_, &QPushButton::clicked, registration_dialog_, &QDialog::reject);

    connect(client_.get(), &client_bridge::connected, this, [this] {
        connected_ = true;
        server_edit_->setEnabled(false);
        if (pending_action_ == pending_action::login)
        {
            authenticate();
        }
        else if (pending_action_ == pending_action::registration)
        {
            register_user();
        }
    }, Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::disconnected, this, [this] {
        auto const action = pending_action_;
        connected_ = false;
        pending_action_ = pending_action::none;
        set_login_busy(false);
        server_edit_->setEnabled(true);
        if (logout_pending_)
        {
            logout_pending_ = false;
            status_label_->clear();
            return;
        }
        if (action == pending_action::registration && registration_dialog_->isVisible())
        {
            set_registration_busy(false);
            registration_status_label_->setText(QStringLiteral("连接已断开"));
            return;
        }
        if (pages_->currentWidget() == chat_page_)
        {
            chat_page_->set_user({});
            pages_->setCurrentWidget(login_page_);
            password_edit_->clear();
            status_label_->setText(QStringLiteral("连接已断开"));
        }
    }, Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::error, this, [this](QString const& message) {
        if (pending_action_ == pending_action::registration && registration_dialog_->isVisible())
        {
            show_registration_error(message);
        }
        else if (pages_->currentWidget() == login_page_)
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
                pending_action_ = pending_action::none;
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

    connect(client_.get(), &client_bridge::registration_finished, this,
            [this](qint64 user, QString const& error_message) {
                (void)user;
                if (!error_message.isEmpty())
                {
                    show_registration_error(error_message);
                    return;
                }

                auto const username = pending_username_;
                pending_action_ = pending_action::none;
                pending_password_.clear();
                set_login_busy(false);
                set_registration_busy(false);
                username_edit_->setText(username);
                password_edit_->clear();
                registration_status_label_->clear();
                QMessageBox::information(registration_dialog_, QStringLiteral("注册成功"),
                                         QStringLiteral("账号 %1 注册成功，请返回登录。").arg(username));
                registration_dialog_->accept();
                registration_username_edit_->clear();
                registration_password_edit_->clear();
                registration_password_confirm_edit_->clear();
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

    connect(client_.get(), &client_bridge::contacts_received, this,
            [this](QList<user_data> contacts, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_contacts_error(error_message);
                    return;
                }
                chat_page_->set_contacts(std::move(contacts));
            },
            Qt::QueuedConnection);

    connect(chat_page_, &chat_widget::logout_requested, this, [this] { logout(); });

    connect(chat_page_, &chat_widget::add_contact_search_requested, this,
            [this](QString query) { client_->search_users(std::move(query)); });

    connect(client_.get(), &client_bridge::users_received, this,
            [this](QList<user_data> users, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_add_contact_search_error(error_message);
                    return;
                }
                chat_page_->set_add_contact_search_results(std::move(users));
            },
            Qt::QueuedConnection);

    connect(chat_page_, &chat_widget::contact_add_requested, this,
            [this](qint64 user) { client_->add_contact(user); });

    connect(client_.get(), &client_bridge::contact_added, this,
            [this](user_data, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_add_contact_search_error(error_message);
                    return;
                }
                chat_page_->finish_add_contact();
                client_->get_contacts();
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
            [this](qint64 user, QString text, qint64 message, qint64 timestamp, bool realtime, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_message_error(user, error_message);
                    return;
                }

                (void)realtime;
                chat_page_->add_sent_message(user, message, timestamp, std::move(text));
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
    pending_action_ = pending_action::login;
    set_login_busy(true);

    if (connected_)
    {
        authenticate();
        return;
    }

    status_label_->setText(QStringLiteral("正在连接…"));
    client_->connect_to_server(server);
}

void main_window::show_registration_dialog()
{
    registration_username_edit_->clear();
    registration_password_edit_->clear();
    registration_password_confirm_edit_->clear();
    registration_status_label_->clear();
    set_registration_busy(false);
    registration_dialog_->open();
    registration_username_edit_->setFocus();
}

void main_window::start_registration()
{
    auto const server = server_edit_->text().trimmed();
    auto const username = registration_username_edit_->text();
    auto const password = registration_password_edit_->text();
    auto const password_confirm = registration_password_confirm_edit_->text();

    if (server.isEmpty())
    {
        registration_status_label_->setText(QStringLiteral("服务器不能为空"));
        return;
    }
    if (username.isEmpty() || password.isEmpty() || password_confirm.isEmpty())
    {
        registration_status_label_->setText(QStringLiteral("用户名和密码不能为空"));
        return;
    }
    if (password != password_confirm)
    {
        registration_status_label_->setText(QStringLiteral("两次输入的密码不一致"));
        return;
    }

    pending_username_ = username;
    pending_password_ = password;
    pending_action_ = pending_action::registration;
    registration_status_label_->clear();
    set_login_busy(true);
    set_registration_busy(true);

    if (connected_)
    {
        register_user();
        return;
    }

    registration_status_label_->setText(QStringLiteral("正在连接…"));
    client_->connect_to_server(server);
}

void main_window::authenticate()
{
    status_label_->setText(QStringLiteral("正在登录…"));
    client_->authenticate(pending_username_, pending_password_);
}

void main_window::register_user()
{
    registration_status_label_->setText(QStringLiteral("正在注册…"));
    client_->register_user(pending_username_, pending_password_);
}

void main_window::logout()
{
    pending_action_ = pending_action::none;
    pending_username_.clear();
    pending_password_.clear();
    password_edit_->clear();
    chat_page_->set_user({});
    pages_->setCurrentWidget(login_page_);

    if (!connected_)
    {
        status_label_->clear();
        set_login_busy(false);
        server_edit_->setEnabled(true);
        return;
    }

    logout_pending_ = true;
    status_label_->setText(QStringLiteral("正在退出…"));
    set_login_busy(true);
    client_->close();
}

void main_window::set_login_busy(bool busy)
{
    username_edit_->setEnabled(!busy);
    password_edit_->setEnabled(!busy);
    login_button_->setEnabled(!busy);
    register_button_->setEnabled(!busy);
    server_edit_->setEnabled(!busy && !connected_);
}

void main_window::set_registration_busy(bool busy)
{
    registration_username_edit_->setEnabled(!busy);
    registration_password_edit_->setEnabled(!busy);
    registration_password_confirm_edit_->setEnabled(!busy);
    registration_submit_button_->setEnabled(!busy);
    registration_cancel_button_->setEnabled(!busy);
}

void main_window::show_login_error(QString message)
{
    pending_action_ = pending_action::none;
    pending_password_.clear();
    status_label_->setText(std::move(message));
    set_login_busy(false);
}

void main_window::show_registration_error(QString message)
{
    pending_action_ = pending_action::none;
    pending_password_.clear();
    registration_status_label_->setText(std::move(message));
    set_registration_busy(false);
    set_login_busy(false);
}

void main_window::show_authenticated_page()
{
    chat_page_->set_user(pending_username_);
    chat_page_->set_loading();
    pages_->setCurrentWidget(chat_page_);
    client_->get_conversations();
    client_->get_contacts();
}
