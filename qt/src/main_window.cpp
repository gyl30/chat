#include "main_window.hpp"

#include <memory>
#include <utility>

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "client_bridge.hpp"

main_window::main_window(QString server_url, QWidget* parent)
    : QMainWindow(parent), client_(std::make_unique<client_bridge>())
{
    setWindowTitle(QStringLiteral("Chat"));
    resize(420, 300);

    pages_ = new QStackedWidget(this);
    setCentralWidget(pages_);

    login_page_ = new QWidget(pages_);
    auto* login_layout = new QVBoxLayout(login_page_);
    login_layout->setContentsMargins(48, 36, 48, 36);
    login_layout->setSpacing(16);

    auto* title = new QLabel(QStringLiteral("登录"), login_page_);
    auto title_font = title->font();
    title_font.setPointSize(title_font.pointSize() + 6);
    title_font.setBold(true);
    title->setFont(title_font);
    login_layout->addWidget(title);

    auto* form = new QFormLayout;
    server_edit_ = new QLineEdit(std::move(server_url), login_page_);
    username_edit_ = new QLineEdit(login_page_);
    password_edit_ = new QLineEdit(login_page_);
    password_edit_->setEchoMode(QLineEdit::Password);

    form->addRow(QStringLiteral("服务器"), server_edit_);
    form->addRow(QStringLiteral("用户名"), username_edit_);
    form->addRow(QStringLiteral("密码"), password_edit_);
    login_layout->addLayout(form);

    login_button_ = new QPushButton(QStringLiteral("登录"), login_page_);
    login_button_->setDefault(true);
    login_layout->addWidget(login_button_);

    status_label_ = new QLabel(login_page_);
    status_label_->setWordWrap(true);
    login_layout->addWidget(status_label_);
    login_layout->addStretch();

    authenticated_page_ = new QWidget(pages_);
    auto* authenticated_layout = new QVBoxLayout(authenticated_page_);
    authenticated_label_ = new QLabel(QStringLiteral("登录成功"), authenticated_page_);
    authenticated_label_->setAlignment(Qt::AlignCenter);
    authenticated_layout->addWidget(authenticated_label_);

    pages_->addWidget(login_page_);
    pages_->addWidget(authenticated_page_);

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
        if (pages_->currentWidget() == authenticated_page_)
        {
            pages_->setCurrentWidget(login_page_);
            status_label_->setText(QStringLiteral("连接已断开"));
        }
    }, Qt::QueuedConnection);

    connect(client_.get(), &client_bridge::error, this, [this](QString const& message) {
        show_login_error(message);
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
                authenticated_label_->setText(QStringLiteral("%1 已登录").arg(pending_username_));
                pages_->setCurrentWidget(authenticated_page_);
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
