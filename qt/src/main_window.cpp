#include "main_window.hpp"

#include <algorithm>
#include <array>
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
#include <QTimer>
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

    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setSingleShot(true);
    reconnect_countdown_timer_ = new QTimer(this);
    reconnect_countdown_timer_->setInterval(1000);
    reconnect_notice_timer_ = new QTimer(this);
    reconnect_notice_timer_->setSingleShot(true);
    reconnect_recovered_timer_ = new QTimer(this);
    reconnect_recovered_timer_->setSingleShot(true);

    pages_->addWidget(login_page_);
    pages_->addWidget(chat_page_);

    connect(login_button_, &QPushButton::clicked, this, [this] { start_login(); });
    connect(register_button_, &QPushButton::clicked, this, [this] { show_registration_dialog(); });
    connect(password_edit_, &QLineEdit::returnPressed, this, [this] { start_login(); });
    connect(registration_submit_button_, &QPushButton::clicked, this, [this] { start_registration(); });
    connect(registration_password_confirm_edit_, &QLineEdit::returnPressed, this, [this] { start_registration(); });
    connect(registration_cancel_button_, &QPushButton::clicked, registration_dialog_, &QDialog::reject);
    connect(reconnect_timer_, &QTimer::timeout, this, [this] { reconnect_now(); });
    connect(reconnect_countdown_timer_, &QTimer::timeout, this, [this] {
        if (reconnect_seconds_left_ > 1)
        {
            --reconnect_seconds_left_;
            update_reconnect_status();
        }
    });
    connect(reconnect_notice_timer_, &QTimer::timeout, this, [this] {
        if (!reconnecting_)
        {
            return;
        }
        reconnect_notice_visible_ = true;
        if (reconnect_timer_->isActive())
        {
            update_reconnect_status();
        }
        else
        {
            chat_page_->set_connection_status(QStringLiteral("正在连接…"), false);
        }
    });
    connect(reconnect_recovered_timer_, &QTimer::timeout, this, [this] {
        if (!reconnecting_ && connected_)
        {
            chat_page_->set_connection_status({}, false);
        }
    });
    connect(chat_page_, &chat_widget::reconnect_requested, this, [this] {
        if (!reconnecting_ || connected_)
        {
            return;
        }
        reconnect_timer_->stop();
        reconnect_countdown_timer_->stop();
        reconnect_now();
    });

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
        else if (pending_action_ == pending_action::reconnect)
        {
            if (reconnect_notice_visible_)
            {
                chat_page_->set_connection_status(QStringLiteral("正在恢复会话…"), false);
            }
            client_->authenticate(session_username_, session_password_);
        }
        else if (session_username_.isEmpty() && pages_->currentWidget() == login_page_)
        {
            client_->close();
        }
    }, Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::disconnected, this, [this] {
        auto const action = pending_action_;
        connected_ = false;
        if (logout_pending_)
        {
            logout_pending_ = false;
            pending_action_ = pending_action::none;
            status_label_->clear();
            set_login_busy(false);
            server_edit_->setEnabled(true);
            return;
        }
        if (action == pending_action::registration && registration_dialog_->isVisible())
        {
            pending_action_ = pending_action::none;
            set_login_busy(false);
            set_registration_busy(false);
            server_edit_->setEnabled(true);
            registration_status_label_->setText(QStringLiteral("连接已断开"));
            return;
        }
        if (!session_username_.isEmpty() && pages_->currentWidget() == chat_page_)
        {
            if (reconnecting_)
            {
                schedule_reconnect();
            }
            else
            {
                begin_reconnect();
            }
            return;
        }

        pending_action_ = pending_action::none;
        set_login_busy(false);
        server_edit_->setEnabled(true);
        if (pages_->currentWidget() == chat_page_)
        {
            chat_page_->set_user({});
            pages_->setCurrentWidget(login_page_);
            password_edit_->clear();
            status_label_->setText(QStringLiteral("连接已断开"));
        }
    }, Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::error, this, [this](QString const& message) {
        if (reconnecting_)
        {
            if (!connected_)
            {
                schedule_reconnect();
            }
            return;
        }
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
            [this](bool authenticated, QString const& error_message, bool retryable_error) {
                auto const action = pending_action_;
                if (action == pending_action::reconnect)
                {
                    if (!error_message.isEmpty())
                    {
                        if (retryable_error)
                        {
                            if (connected_)
                            {
                                client_->close();
                            }
                            else
                            {
                                schedule_reconnect();
                            }
                            return;
                        }
                        return_to_login(QStringLiteral("重新登录失败：%1").arg(error_message));
                        return;
                    }
                    if (!authenticated)
                    {
                        return_to_login(QStringLiteral("登录状态已失效，请重新登录"));
                        return;
                    }
                    finish_reconnect();
                    return;
                }

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

                session_server_ = server_edit_->text().trimmed();
                session_username_ = pending_username_;
                session_password_ = pending_password_;
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
            [this](qint64 user, QList<message_data> messages, qint64 read_message, bool older, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_message_error(user, error_message);
                    return;
                }

                chat_page_->set_messages(user, std::move(messages), read_message, older);
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

    connect(client_.get(), &client_bridge::messages_read, this,
            [this](qint64 user, qint64 message) { chat_page_->set_read_message(user, message); },
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
    auto const should_close = connected_ || reconnecting_ || pending_action_ == pending_action::reconnect;
    stop_reconnect();
    pending_action_ = pending_action::none;
    pending_username_.clear();
    pending_password_.clear();
    session_server_.clear();
    session_username_.clear();
    session_password_.clear();
    password_edit_->clear();
    chat_page_->set_connection_available(false);
    chat_page_->set_user({});
    pages_->setCurrentWidget(login_page_);

    if (!should_close)
    {
        status_label_->clear();
        set_login_busy(false);
        server_edit_->setEnabled(true);
        return;
    }

    logout_pending_ = connected_;
    status_label_->setText(QStringLiteral("正在退出…"));
    set_login_busy(true);
    client_->close();
    if (!connected_)
    {
        status_label_->clear();
        set_login_busy(false);
        server_edit_->setEnabled(true);
    }
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
    chat_page_->set_connection_available(true);
    chat_page_->set_connection_status({}, false);
    chat_page_->set_loading();
    pages_->setCurrentWidget(chat_page_);
    client_->get_conversations();
    client_->get_contacts();
}

void main_window::begin_reconnect()
{
    if (reconnecting_ || session_server_.isEmpty() || session_username_.isEmpty())
    {
        return;
    }

    reconnecting_ = true;
    reconnect_notice_visible_ = false;
    reconnect_attempt_ = 0;
    reconnect_seconds_left_ = 0;
    reconnect_timer_->stop();
    reconnect_countdown_timer_->stop();
    reconnect_recovered_timer_->stop();
    reconnect_notice_timer_->start(1000);
    chat_page_->set_connection_available(false);
    reconnect_now();
}

void main_window::reconnect_now()
{
    if (!reconnecting_ || connected_)
    {
        return;
    }

    reconnect_timer_->stop();
    reconnect_countdown_timer_->stop();
    reconnect_seconds_left_ = 0;
    pending_action_ = pending_action::reconnect;
    if (reconnect_notice_visible_)
    {
        chat_page_->set_connection_status(QStringLiteral("正在连接…"), false);
    }
    client_->connect_to_server(session_server_);
}

void main_window::schedule_reconnect()
{
    if (!reconnecting_ || reconnect_timer_->isActive())
    {
        return;
    }

    constexpr std::array delays = {1, 2, 4, 8, 15};
    auto const index = std::min(reconnect_attempt_, static_cast<int>(delays.size()) - 1);
    reconnect_seconds_left_ = delays[static_cast<std::size_t>(index)];
    ++reconnect_attempt_;
    pending_action_ = pending_action::none;
    reconnect_notice_timer_->stop();
    reconnect_notice_visible_ = true;
    update_reconnect_status();
    reconnect_countdown_timer_->start();
    reconnect_timer_->start(reconnect_seconds_left_ * 1000);
}

void main_window::update_reconnect_status()
{
    if (!reconnecting_ || !reconnect_notice_visible_)
    {
        return;
    }

    chat_page_->set_connection_status(
        QStringLiteral("连接中断，%1 秒后重连 · 立即重试").arg(reconnect_seconds_left_), true);
}

void main_window::finish_reconnect()
{
    reconnect_timer_->stop();
    reconnect_countdown_timer_->stop();
    reconnect_notice_timer_->stop();
    pending_action_ = pending_action::none;
    reconnecting_ = false;
    reconnect_attempt_ = 0;
    reconnect_seconds_left_ = 0;
    chat_page_->set_connection_available(true);

    if (reconnect_notice_visible_)
    {
        chat_page_->set_connection_status(QStringLiteral("已重新连接"), false);
        reconnect_recovered_timer_->start(1200);
    }
    else
    {
        chat_page_->set_connection_status({}, false);
    }
    reconnect_notice_visible_ = false;

    client_->get_conversations();
    client_->get_contacts();
    auto const user = chat_page_->active_user();
    if (user > 0)
    {
        client_->get_messages(user);
    }
}

void main_window::stop_reconnect()
{
    reconnect_timer_->stop();
    reconnect_countdown_timer_->stop();
    reconnect_notice_timer_->stop();
    reconnect_recovered_timer_->stop();
    reconnecting_ = false;
    reconnect_notice_visible_ = false;
    reconnect_attempt_ = 0;
    reconnect_seconds_left_ = 0;
    chat_page_->set_connection_status({}, false);
}

void main_window::return_to_login(QString message)
{
    auto const username = session_username_;
    auto const should_close = connected_;
    stop_reconnect();
    pending_action_ = pending_action::none;
    pending_username_.clear();
    pending_password_.clear();
    session_server_.clear();
    session_username_.clear();
    session_password_.clear();
    chat_page_->set_connection_available(false);
    chat_page_->set_user({});
    pages_->setCurrentWidget(login_page_);
    username_edit_->setText(username);
    password_edit_->clear();
    status_label_->setText(std::move(message));
    set_login_busy(false);
    server_edit_->setEnabled(true);

    if (should_close)
    {
        client_->close();
    }
}
