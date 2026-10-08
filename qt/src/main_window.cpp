#include "main_window.hpp"
#include "emoji_segments.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <chat/text.hpp>

#include <QDialog>
#include <QStyle>
#include <QAction>
#include <QCoreApplication>
#include <QSettings>
#include <QMenu>
#include <QApplication>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "avatar.hpp"
#include "chat_widget.hpp"
#include "attachment_dialog.hpp"
#include "group_dialog.hpp"
#include "client_bridge.hpp"
#include "message_search_dialog.hpp"
#include "theme.hpp"
#include "icons.hpp"

main_window::main_window(QString server_url, QWidget* parent)
    : QMainWindow(parent), client_(std::make_unique<client_bridge>())
{
    setWindowTitle(QStringLiteral("Chat"));
    resize(chat_theme::login_window_size);
    setMinimumSize(chat_theme::login_window_size);
    setStyleSheet(chat_style_sheet());

    pages_ = new QStackedWidget(this);
    setCentralWidget(pages_);

    login_page_ = new QWidget(pages_);
    login_page_->setObjectName(QStringLiteral("loginPage"));
    auto* login_outer = new QVBoxLayout(login_page_);
    login_outer->setContentsMargins(0, 0, 0, 0);

    // Modeled on the QQ desktop login: identity first (avatar, account, password), one primary
    // action, and secondary entries (registration, server settings) kept out of the main path.
    auto* login_card = new QFrame(login_page_);
    login_card->setObjectName(QStringLiteral("loginCard"));
    login_card->setFixedWidth(chat_theme::auth_card_width);
    auto* login_layout = new QVBoxLayout(login_card);
    login_layout->setContentsMargins(chat_theme::auth_padding, 8, chat_theme::auth_padding, 20);
    login_layout->setSpacing(chat_theme::auth_spacing);

    auto* server_settings = new QToolButton(login_card);
    server_settings->setObjectName(QStringLiteral("serverSettingsButton"));
    server_settings->setIcon(svg_icon(u"settings", QColor(QStringLiteral("#5D6C64"))));
    server_settings->setToolTip(QStringLiteral("服务器设置"));
    server_settings->setAccessibleName(QStringLiteral("服务器设置"));
    server_settings->setToolButtonStyle(Qt::ToolButtonIconOnly);
    server_settings->setFixedSize(36, 36);
    server_settings->setCheckable(true);
    auto* settings_bar = new QHBoxLayout;
    settings_bar->addStretch();
    settings_bar->addWidget(server_settings);
    login_layout->addLayout(settings_bar);
    login_layout->addStretch(2);

    login_avatar_ = new QLabel(login_card);
    login_avatar_->setObjectName(QStringLiteral("loginAvatar"));
    login_avatar_->setFixedSize(chat_theme::login_avatar_size, chat_theme::login_avatar_size);
    login_avatar_->setAlignment(Qt::AlignCenter);
    login_layout->addWidget(login_avatar_, 0, Qt::AlignHCenter);
    login_layout->addSpacing(12);

    server_edit_ = new QLineEdit(std::move(server_url), login_card);
    server_edit_->setObjectName(QStringLiteral("serverUrlEdit"));
    server_edit_->setAccessibleName(QStringLiteral("服务器地址"));
    server_edit_->setPlaceholderText(QStringLiteral("ws://服务器地址:端口/ws"));
    username_edit_ = new QLineEdit(login_card);
    username_edit_->setObjectName(QStringLiteral("loginUsernameEdit"));
    username_edit_->setAccessibleName(QStringLiteral("用户名"));
    username_edit_->setPlaceholderText(QStringLiteral("用户名"));
    username_edit_->setAlignment(Qt::AlignCenter);
    password_edit_ = new QLineEdit(login_card);
    password_edit_->setObjectName(QStringLiteral("loginPasswordEdit"));
    password_edit_->setAccessibleName(QStringLiteral("密码"));
    password_edit_->setPlaceholderText(QStringLiteral("密码"));
    password_edit_->setEchoMode(QLineEdit::Password);
    password_edit_->setAlignment(Qt::AlignCenter);
    recent_accounts_action_ = username_edit_->addAction(
        svg_icon(u"chevron-down", QColor(QStringLiteral("#5D6C64")), QSize(18, 18)), QLineEdit::TrailingPosition);
    recent_accounts_action_->setToolTip(QStringLiteral("选择已登录过的账号"));
    connect(recent_accounts_action_, &QAction::triggered, this, [this] {
        QMenu menu(username_edit_);
        menu.setObjectName(QStringLiteral("recentAccountsMenu"));
        for (auto const& account : recent_accounts())
        {
            menu.addAction(avatar_icon(account, 24), account, this, [this, account] {
                username_edit_->setText(account);
                password_edit_->clear();
                password_edit_->setFocus();
            });
        }
        menu.setMinimumWidth(username_edit_->width());
        menu.exec(username_edit_->mapToGlobal(QPoint(0, username_edit_->height())));
    });
    login_layout->addWidget(username_edit_);
    login_layout->addWidget(password_edit_);

    status_label_ = new feedback_label(login_card);
    status_label_->setObjectName(QStringLiteral("subtleText"));
    status_label_->setAlignment(Qt::AlignCenter);
    status_label_->setWordWrap(true);
    login_layout->addWidget(status_label_);

    login_button_ = new QPushButton(QStringLiteral("登录"), login_card);
    login_button_->setObjectName(QStringLiteral("loginButton"));
    login_button_->setDefault(true);
    login_layout->addWidget(login_button_);
    register_button_ = new QPushButton(QStringLiteral("注册账号"), login_card);
    register_button_->setObjectName(QStringLiteral("registerButton"));
    login_layout->addWidget(register_button_, 0, Qt::AlignHCenter);

    login_layout->addWidget(server_edit_);
    login_layout->addStretch(3);
    server_edit_->hide();
    connect(server_settings, &QToolButton::toggled, this, [this](bool expanded) {
        server_edit_->setVisible(expanded);
        if (expanded) { server_edit_->setFocus(); }
        else { username_edit_->setFocus(); }
    });
    connect(username_edit_, &QLineEdit::textChanged, this, [this] { update_login_identity(); });
    setTabOrder(username_edit_, password_edit_);
    setTabOrder(password_edit_, login_button_);
    setTabOrder(login_button_, register_button_);
    setTabOrder(register_button_, server_settings);
    setTabOrder(server_settings, server_edit_);

    login_outer->addWidget(login_card, 1, Qt::AlignHCenter);

    if (auto const accounts = recent_accounts(); !accounts.isEmpty())
    {
        // Like QQ, the last account is ready and only its password is asked for.
        username_edit_->setText(accounts.front());
    }
    update_login_identity();

    registration_dialog_ = new QDialog(this);
    registration_dialog_->setObjectName(QStringLiteral("registrationDialog"));
    registration_dialog_->setWindowTitle(QStringLiteral("注册"));
    registration_dialog_->setModal(true);
    registration_dialog_->setFixedWidth(chat_theme::auth_card_width);
    auto* registration_layout = new QVBoxLayout(registration_dialog_);
    registration_layout->setContentsMargins(chat_theme::auth_padding, chat_theme::auth_padding,
                                            chat_theme::auth_padding, chat_theme::auth_padding);
    registration_layout->setSpacing(chat_theme::auth_spacing);

    auto* registration_title = new QLabel(QStringLiteral("创建账号"), registration_dialog_);
    registration_title->setObjectName(QStringLiteral("registrationTitle"));
    registration_layout->addWidget(registration_title);
    auto* registration_subtitle = new QLabel(QStringLiteral("用用户名找到朋友，也让朋友找到你。"), registration_dialog_);
    registration_subtitle->setObjectName(QStringLiteral("authSubtitle"));
    registration_subtitle->setWordWrap(true);
    registration_layout->addWidget(registration_subtitle);

    auto* registration_form = new QFormLayout;
    registration_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    registration_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    registration_form->setVerticalSpacing(8);
    registration_username_edit_ = new QLineEdit(registration_dialog_);
    registration_password_edit_ = new QLineEdit(registration_dialog_);
    registration_password_confirm_edit_ = new QLineEdit(registration_dialog_);
    registration_username_edit_->setObjectName(QStringLiteral("registrationUsernameEdit"));
    registration_username_edit_->setAccessibleName(QStringLiteral("用户名"));
    registration_password_edit_->setAccessibleName(QStringLiteral("密码"));
    registration_password_confirm_edit_->setAccessibleName(QStringLiteral("确认密码"));
    registration_password_edit_->setEchoMode(QLineEdit::Password);
    registration_password_confirm_edit_->setEchoMode(QLineEdit::Password);
    registration_username_edit_->setPlaceholderText(QStringLiteral("用户名"));
    registration_password_edit_->setPlaceholderText(QStringLiteral("密码"));
    registration_password_confirm_edit_->setPlaceholderText(QStringLiteral("再次输入密码"));
    registration_form->addRow(QStringLiteral("用户名"), registration_username_edit_);
    registration_form->addRow(QStringLiteral("密码"), registration_password_edit_);
    registration_form->addRow(QStringLiteral("确认密码"), registration_password_confirm_edit_);
    registration_layout->addLayout(registration_form);

    registration_status_label_ = new feedback_label(registration_dialog_);
    registration_status_label_->setObjectName(QStringLiteral("subtleText"));
    registration_status_label_->setWordWrap(true);
    registration_layout->addWidget(registration_status_label_);

    registration_submit_button_ = new QPushButton(QStringLiteral("创建账号"), registration_dialog_);
    registration_submit_button_->setObjectName(QStringLiteral("registrationSubmitButton"));
    registration_submit_button_->setDefault(true);
    registration_layout->addWidget(registration_submit_button_);
    registration_cancel_button_ = new QPushButton(QStringLiteral("返回登录"), registration_dialog_);
    registration_cancel_button_->setObjectName(QStringLiteral("registrationCancelButton"));
    registration_layout->addWidget(registration_cancel_button_);
    setTabOrder(registration_username_edit_, registration_password_edit_);
    setTabOrder(registration_password_edit_, registration_password_confirm_edit_);
    setTabOrder(registration_password_confirm_edit_, registration_submit_button_);
    setTabOrder(registration_submit_button_, registration_cancel_button_);

    chat_page_ = new chat_widget(pages_);
    chat_page_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

    tray_ = new QSystemTrayIcon(svg_icon(u"chat", QColor(QStringLiteral("#365E4B")), QSize(32, 32)), this);
    tray_->setObjectName(QStringLiteral("notificationTray"));
    tray_->setToolTip(QStringLiteral("Chat"));
    auto restore_window = [this] { showNormal(); raise(); activateWindow(); };
    connect(tray_, &QSystemTrayIcon::messageClicked, this, restore_window);
    connect(tray_, &QSystemTrayIcon::activated, this, [restore_window](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) { restore_window(); }
    });
    connect(this, &main_window::notification_requested, tray_, [this](qint64, QString const& title, QString const& summary) {
        if (QSystemTrayIcon::isSystemTrayAvailable() && QSystemTrayIcon::supportsMessages())
        {
            tray_->showMessage(title, summary, QSystemTrayIcon::Information);
        }
    });
    connect(chat_page_, &chat_widget::read_requested, client_.get(), &client_bridge::mark_read);
    connect(chat_page_, &chat_widget::mute_requested, client_.get(), &client_bridge::set_conversation_muted);
    connect(client_.get(), &client_bridge::mute_finished, this, [this](qint64 conversation, bool muted, QString error) {
        if (!error.isEmpty())
        {
            chat_page_->set_error(std::move(error));
            return;
        }
        chat_page_->set_conversation_muted(conversation, muted);
        client_->get_conversations();
    });

    connect(&chat_page_->avatars(), &avatar_cache::requested, client_.get(), &client_bridge::get_avatar);
    connect(chat_page_, &chat_widget::pin_requested, client_.get(), &client_bridge::set_conversation_pinned);
    connect(client_.get(), &client_bridge::pin_finished, this, [this](qint64 conversation, bool pinned, QString error) {
        if (!error.isEmpty())
        {
            chat_page_->set_error(std::move(error));
            return;
        }
        chat_page_->set_conversation_pinned(conversation, pinned);
        client_->get_conversations();
    });
    connect(client_.get(), &client_bridge::avatar_changed, &chat_page_->avatars(), &avatar_cache::observe);
    connect(client_.get(), &client_bridge::avatar_received, &chat_page_->avatars(), &avatar_cache::receive);
    connect(chat_page_, &chat_widget::avatar_set_requested, client_.get(), &client_bridge::set_avatar);
    connect(chat_page_, &chat_widget::avatar_clear_requested, client_.get(), &client_bridge::clear_avatar);
    connect(client_.get(), &client_bridge::avatar_update_finished, chat_page_, &chat_widget::finish_avatar_update);

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
    connect(pages_, &QStackedWidget::currentChanged, this, [this] {
        auto const chatting = pages_->currentWidget() == chat_page_;
        auto const policy = chatting ? QSizePolicy::Preferred : QSizePolicy::Ignored;
        chat_page_->setSizePolicy(policy, policy);
        setMinimumSize(chatting ? QSize(980, 640) : chat_theme::login_window_size);
        if (!chatting && isMaximized()) { showNormal(); }
        resize(chatting ? QSize(1180, 760) : chat_theme::login_window_size);
    });

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
    }, Qt::AutoConnection);

    connect(client_.get(), &client_bridge::disconnected, this, [this] {
        pending_notifications_.clear();
        auto const action = pending_action_;
        connected_ = false;
        if (logout_pending_)
        {
            logout_pending_ = false;
            pending_action_ = pending_action::none;
            status_label_->clear();
            set_login_busy(false);
            server_edit_->setEnabled(true);
            username_edit_->setFocus();
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
    }, Qt::AutoConnection);

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
    }, Qt::AutoConnection);

    connect(
        client_.get(), &client_bridge::authentication_finished, this,
        [this](bool authenticated, qint64 user, QString const& error_message, bool retryable_error, chat::avatar_state avatar)
        {
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
                    chat_page_->avatars().observe(user, avatar);
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
                remember_account(session_username_);
                session_password_ = pending_password_;
                pending_password_.clear();
                status_label_->clear();
            show_authenticated_page(user);
            chat_page_->avatars().observe(user, avatar);
            },
            Qt::AutoConnection);

    connect(client_.get(), &client_bridge::registration_finished, this,
            [this](qint64 user, QString const& error_message) {
                (void)user;
                if (!error_message.isEmpty())
                {
                    show_registration_error(error_message);
                    return;
                }

                // A new account signs in directly with the credentials just chosen, like other IMs.
                set_registration_busy(false);
                registration_status_label_->clear();
                registration_dialog_->accept();
                registration_username_edit_->clear();
                registration_password_edit_->clear();
                registration_password_confirm_edit_->clear();
                username_edit_->setText(pending_username_);
                password_edit_->clear();
                pending_action_ = pending_action::login;
                authenticate();
            },
            Qt::AutoConnection);

    connect(client_.get(), &client_bridge::conversations_received, this,
            [this](QList<conversation_data> conversations, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    pending_notifications_.clear();
                    chat_page_->set_conversations_error(error_message);
                    return;
                }
                chat_page_->set_conversations(std::move(conversations));
                chat_page_->set_connection_available(true);
                auto pending = std::move(pending_notifications_);
                pending_notifications_.clear();
                for (auto const& message : pending)
                {
                    if (chat_page_->conversation(message.conversation)) { notify_message(message); }
                }
            },
            Qt::AutoConnection);

    connect(client_.get(), &client_bridge::contacts_received, this,
            [this](QList<user_data> contacts, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_contacts_error(error_message);
                    return;
                }
                chat_page_->set_contacts(std::move(contacts));
                client_->get_presence();
            },
            Qt::AutoConnection);

    connect(client_.get(), &client_bridge::presences_received, this,
            [this](QList<presence_data> users, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_presences({});
                    chat_page_->set_error(error_message);
                    return;
                }
                chat_page_->set_presences(std::move(users));
            },
            Qt::AutoConnection);

    connect(client_.get(), &client_bridge::presence_changed, this,
            [this](presence_data user) { chat_page_->set_presence(std::move(user)); },
            Qt::AutoConnection);

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
            Qt::AutoConnection);

    connect(client_.get(), &client_bridge::friend_requests_received, chat_page_, &chat_widget::set_friend_requests);
    connect(client_.get(), &client_bridge::friendship_changed, this, [this](qint64) {
        client_->get_contacts();
        client_->get_friend_requests();
        client_->get_conversations();
    });
    connect(chat_page_, &chat_widget::friend_request_respond_requested, client_.get(), &client_bridge::respond_friend_request);
    connect(chat_page_, &chat_widget::friend_request_cancel_requested, client_.get(), &client_bridge::cancel_friend_request);
    connect(chat_page_, &chat_widget::contact_add_requested, this,
            [this](qint64 user) { client_->add_contact(user); });

    connect(client_.get(), &client_bridge::contact_added, this,
            [this](user_data user, QString const& error_message) {
                chat_page_->finish_add_contact(user.id, error_message);
                if (!error_message.isEmpty())
                {
                    return;
                }
                client_->get_contacts();
                client_->get_friend_requests();
                client_->get_conversations();
            },
            Qt::AutoConnection);

    connect(chat_page_, &chat_widget::contact_remove_requested, this,
            [this](qint64 user) { client_->remove_contact(user); });
    connect(client_.get(), &client_bridge::contact_removed, this, [this](QString const& error_message) {
        if (!error_message.isEmpty())
        {
            show_notice(this, QStringLiteral("移除联系人失败"), error_message, QStringLiteral("关闭"));
            return;
        }
        client_->get_contacts();
        client_->get_friend_requests();
        client_->get_conversations();
    }, Qt::AutoConnection);
    connect(chat_page_, &chat_widget::direct_conversation_requested, this,
            [this](qint64 user, QString username) { client_->open_direct_conversation(user, std::move(username)); });
    connect(chat_page_, &chat_widget::group_create_requested, this, [this](QString title, QList<qint64> members)
            { client_->create_group(std::move(title), std::move(members)); });
    connect(chat_page_, &chat_widget::group_join_requested, this, [this](QString token) { client_->join_group(std::move(token)); });
    connect(chat_page_, &chat_widget::members_requested, this,
            [this](qint64 conversation, qint64 self_user, QString title) {
                auto const snapshot = chat_page_->conversation(conversation);
                if (!snapshot || !snapshot->group) { return; }
                group_dialog dialog(conversation, self_user, title, snapshot->announcement, snapshot->join_approval, this, &chat_page_->avatars());
                connect(&dialog, &group_dialog::user_requested, chat_page_, &chat_widget::show_user_details);
                connect(client_.get(), &client_bridge::members_received, &dialog, &group_dialog::set_members);
                connect(client_.get(), &client_bridge::group_action_finished, &dialog, &group_dialog::finish_action);
                connect(client_.get(), &client_bridge::group_invite_received, &dialog, &group_dialog::set_invite);
                connect(client_.get(), &client_bridge::group_join_requests_received, &dialog, &group_dialog::set_requests);
                connect(&dialog, &group_dialog::approval_requested, &dialog, [this, conversation](bool required) {
                    client_->set_group_join_approval(conversation, required);
                });
                connect(&dialog, &group_dialog::requests_requested, &dialog, [this, conversation](qint64 before) {
                    client_->get_group_join_requests(conversation, before);
                });
                connect(&dialog, &group_dialog::request_response_requested, &dialog, [this, conversation](qint64 user, bool accept) {
                    client_->respond_group_join_request(conversation, user, accept);
                });
                connect(client_.get(), &client_bridge::group_join_request_changed, &dialog,
                    [&dialog, conversation](qint64 id, qint64, chat::group_join_request_state) {
                        if (id == conversation) { dialog.refresh_requests(); }
                    });
                connect(&dialog, &group_dialog::invite_link_requested, &dialog, [this, conversation](std::optional<bool> create) {
                    client_->group_invite(conversation, create);
                });
                connect(client_.get(), &client_bridge::disconnected, &dialog, [&dialog] {
                    dialog.set_error(QStringLiteral("连接已断开，请重连后重新打开群成员。"));
                }, Qt::AutoConnection);
                connect(&dialog, &group_dialog::admin_requested, this, [this, conversation](qint64 user, bool admin) {
                    client_->set_group_admin(conversation, user, admin);
                });
                connect(&dialog, &group_dialog::transfer_requested, this, [this, conversation](qint64 user) {
                    client_->transfer_group_owner(conversation, user);
                });
                connect(&dialog, &group_dialog::remove_requested, this, [this, conversation](qint64 user) {
                    client_->remove_group_member(conversation, user);
                });
                connect(client_.get(), &client_bridge::conversation_changed, &dialog,
                        [&dialog, conversation](qint64 id, bool removed) {
                    if (removed && id == conversation)
                    {
                        for (auto* child : dialog.findChildren<QDialog*>()) { child->reject(); }
                        dialog.reject();
                    }
                });
                connect(client_.get(), &client_bridge::contacts_received, &dialog, &group_dialog::set_contacts,
                        Qt::AutoConnection);
                connect(client_.get(), &client_bridge::conversations_received, &dialog, &group_dialog::set_conversations,
                        Qt::AutoConnection);
                connect(&dialog, &group_dialog::rename_requested, this, [this, conversation](QString title) {
                    client_->rename_group(conversation, std::move(title));
                });
                connect(&dialog, &group_dialog::announcement_requested, this, [this, conversation](QString text) {
                    client_->set_group_announcement(conversation, std::move(text));
                });
                connect(&dialog, &group_dialog::invite_requested, this, [this, conversation](QList<qint64> members) {
                    client_->invite_group_members(conversation, std::move(members));
                });
                connect(&dialog, &group_dialog::leave_requested, this, [this, conversation] {
                    client_->leave_group(conversation);
                });
                client_->get_contacts();
                client_->get_friend_requests();
                client_->get_members(conversation);
                dialog.exec();
            });
    connect(chat_page_, &chat_widget::message_search_requested, this,
            [this](qint64 conversation, qint64 self_user, bool group, QString const& title, QString const& query) {
        message_search_dialog dialog(conversation, self_user, group, title, query, this, &chat_page_->avatars());
        connect(&dialog, &message_search_dialog::search_requested, &dialog,
                [this, conversation](QString query, qint64 before) {
                    client_->search_messages(conversation, std::move(query), before);
                });
        connect(client_.get(), &client_bridge::message_search_received, &dialog,
                &message_search_dialog::set_results, Qt::AutoConnection);
        connect(client_.get(), &client_bridge::reaction_changed, &dialog,
                &message_search_dialog::set_reactions, Qt::AutoConnection);
        connect(client_.get(), &client_bridge::message_updated, &dialog,
                &message_search_dialog::update_message, Qt::AutoConnection);
        connect(client_.get(), &client_bridge::conversation_changed, &dialog,
                [&dialog, conversation](qint64 id, bool removed) {
            if (removed && id == conversation) { dialog.reject(); }
        });
        qint64 located = 0;
        connect(&dialog, &message_search_dialog::message_activated, &dialog, [&dialog, &located](qint64 message) {
            located = message;
            dialog.accept();
        });
        if (dialog.exec() == QDialog::Accepted && located > 0) { chat_page_->locate_message(conversation, located); }
    });
    connect(
        client_.get(), &client_bridge::conversation_opened, this,
        [this](conversation_data conversation, QString error)
        {
            if (!error.isEmpty())
            {
                chat_page_->set_error(std::move(error));
                return;
            }
            chat_page_->open_conversation(std::move(conversation));
            client_->get_conversations();
        });
    connect(client_.get(), &client_bridge::group_join_pending, this, [this](QString const& title) {
        chat_page_->set_error(QStringLiteral("已申请加入 %1，等待管理员处理。").arg(title));
    });
    connect(client_.get(), &client_bridge::group_join_request_changed, this,
        [this](qint64, qint64 user, chat::group_join_request_state state) {
            if (user == chat_page_->self_user())
            {
                if (state == chat::group_join_request_state::accepted) { client_->get_conversations(); }
                chat_page_->set_error(state == chat::group_join_request_state::accepted ? QStringLiteral("入群申请已通过。") :
                    state == chat::group_join_request_state::rejected ? QStringLiteral("入群申请被拒绝。") : QStringLiteral("入群申请已提交。"));
            }
            else if (state == chat::group_join_request_state::pending)
            { chat_page_->set_error(QStringLiteral("收到新的入群申请，可在群资料中处理。")); }
        });
    connect(client_.get(), &client_bridge::group_action_finished, this,
            [this](qint64 conversation, bool left, QString const& error) {
                if (error.isEmpty())
                {
                    if (left)
                    {
                        chat_page_->close_conversation(conversation);
                    }
                    else
                    {
                        client_->get_members(conversation);
                    }
                    client_->get_conversations();
                }
            });

    connect(
        client_.get(), &client_bridge::conversation_changed, this, [this](qint64 conversation, bool removed) {
            if (removed)
            {
                pending_notifications_.removeIf([conversation](auto const& message) { return message.conversation == conversation; });
                chat_page_->close_conversation(conversation);
            }
            client_->get_conversations();
            if (!removed) { client_->get_members(conversation); }
            if (conversation == chat_page_->active_conversation())
            {
                chat_page_->reset_read_positions(conversation);
                client_->get_messages(conversation, {}, chat_page_->recovery_cursor());
            }
        }, Qt::AutoConnection);
    connect(client_.get(), &client_bridge::members_received, chat_page_, &chat_widget::set_members);

    connect(chat_page_, &chat_widget::conversation_selected, this,
            [this](qint64 user, bool group) {
                client_->get_messages(user);
                if (group) { client_->get_members(user); }
            });

    connect(chat_page_, &chat_widget::older_messages_requested, this,
            [this](qint64 user, qint64 before) { client_->get_messages(user, before); });

    connect(chat_page_, &chat_widget::send_message_requested, this,
            [this](qint64 user, QString text, qint64 reply) { client_->send_message(user, std::move(text), reply); });
    connect(chat_page_, &chat_widget::typing_requested, client_.get(), &client_bridge::set_typing);
    connect(client_.get(), &client_bridge::typing_changed, chat_page_, &chat_widget::set_typing, Qt::AutoConnection);
    connect(chat_page_, &chat_widget::attachment_send_requested, this,
            [this](qint64 conversation, QString filename, QByteArray data, qint64 reply) {
                client_->send_attachment(conversation, std::move(filename), std::move(data), reply);
            });
    connect(client_.get(), &client_bridge::attachment_sent, this,
            [this](qint64 conversation, message_data message, QString const& error) {
        chat_page_->finish_attachment_send(conversation, error);
        if (error.isEmpty())
        {
            chat_page_->add_message(conversation, std::move(message));
            client_->get_conversations();
        }
    }, Qt::AutoConnection);
    connect(chat_page_, &chat_widget::attachment_open_requested, this,
            [this](qint64 conversation, qint64 message, QString filename, bool preview) {
        attachment_dialog dialog(conversation, message, std::move(filename), preview, this,
                                  preview ? chat_page_->images().image(message) : QPixmap{});
        connect(client_.get(), &client_bridge::attachment_received, &dialog, &attachment_dialog::set_data,
                Qt::AutoConnection);
        connect(client_.get(), &client_bridge::conversation_changed, &dialog,
                [&dialog, conversation](qint64 id, bool removed) {
            if (removed && id == conversation)
            {
                for (auto* child : dialog.findChildren<QDialog*>()) { child->reject(); }
                dialog.reject();
            }
        });
        connect(client_.get(), &client_bridge::disconnected, &dialog, [&dialog, conversation, message] {
            dialog.set_data(conversation, message, {}, QStringLiteral("连接已断开，请关闭后重新下载。"));
        }, Qt::AutoConnection);
        connect(client_.get(), &client_bridge::message_updated, &dialog,
                [&dialog, message](qint64, message_data value, QString const&) {
            if (value.id == message && value.deleted) { dialog.reject(); }
        }, Qt::AutoConnection);
        auto const bytes = chat_page_->images().bytes(message);
        if (!bytes.isEmpty()) { dialog.set_data(conversation, message, bytes, {}); }
        else { client_->get_attachment(conversation, message); }
        dialog.exec();
    });
    connect(&chat_page_->images(), &message_images::requested, client_.get(), &client_bridge::get_message_image);
    connect(client_.get(), &client_bridge::message_image_received, &chat_page_->images(), &message_images::receive,
            Qt::AutoConnection);

    connect(
        client_.get(), &client_bridge::messages_received, this,
        [this](qint64 user, QList<message_data> messages, read_positions positions, bool older, bool recovering,
               bool has_more, QString const& error_message)
        {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_message_error(user, error_message);
                    return;
                }

            chat_page_->set_messages(user, std::move(messages), std::move(positions), older, recovering, has_more);
            },
            Qt::AutoConnection);

    connect(
        client_.get(), &client_bridge::message_received, this,
        [this](message_data message)
        {
            auto const user = message.conversation;
            notify_message(message);
            chat_page_->add_message(user, std::move(message));
            if (!reading_conversation(user))
            {
                client_->get_conversations();
                client_->get_presence();
            }
        },
        Qt::AutoConnection);

    connect(chat_page_, &chat_widget::delete_message_requested, this,
            [this](qint64 conversation, qint64 message) { client_->delete_message(conversation, message); });
    connect(chat_page_, &chat_widget::reaction_requested, client_.get(), &client_bridge::set_message_reaction);
    connect(client_.get(), &client_bridge::reaction_changed, this,
            [this](qint64 conversation, qint64 message, qint64 revision, QList<reaction_data> reactions, QString error) {
        if (!error.isEmpty())
        {
            chat_page_->set_message_error(conversation, std::move(error));
            return;
        }
        if (!chat_page_->set_reactions(conversation, message, revision, std::move(reactions)) &&
            message > chat_page_->latest_message_id())
        {
            client_->get_messages(conversation, {}, chat_page_->recovery_cursor());
        }
    }, Qt::AutoConnection);
    connect(chat_page_, &chat_widget::edit_message_requested, this,
            [this](qint64 conversation, qint64 message, QString text)
            { client_->edit_message(conversation, message, std::move(text)); });
    connect(chat_page_, &chat_widget::group_message_pin_requested, client_.get(), &client_bridge::set_group_pinned_message);
    connect(client_.get(), &client_bridge::group_pin_finished, this, [this](qint64 conversation, QString error) {
        if (!error.isEmpty()) { chat_page_->set_message_error(conversation, std::move(error)); }
        else { client_->get_conversations(); }
    });
    connect(
        client_.get(), &client_bridge::message_updated, this,
        [this](qint64 conversation, message_data message, QString error)
        {
            if (!error.isEmpty())
            {
                chat_page_->set_message_error(conversation, std::move(error));
                return;
            }
            pending_notifications_.removeIf([&message](auto const& value) { return message.deleted && value.id == message.id; });
            for (auto& value : pending_notifications_)
            {
                if (value.id == message.id) { value = message; }
            }
            chat_page_->update_message(std::move(message));
            client_->get_conversations();
        },
        Qt::AutoConnection);

    connect(
        client_.get(), &client_bridge::messages_read, this, [this](qint64 conversation, qint64 user, qint64 message)
        { chat_page_->set_read_message(conversation, user, message); }, Qt::AutoConnection);

    connect(
        client_.get(), &client_bridge::message_sent, this,
        [this](qint64 user, QString text, qint64 message, qint64 timestamp, bool realtime, quoted_message_data reply,
               QList<mention_data> mentions, QString const& error_message)
        {
            chat_page_->finish_message_send(user, message, timestamp, std::move(text), std::move(reply), std::move(mentions),
                                            error_message);
            if (!error_message.isEmpty())
            {
                return;
            }

            (void)realtime;
            client_->get_conversations();
            client_->get_presence();
        },
        Qt::AutoConnection);

    connect(client_.get(), &client_bridge::read_marked, this,
            [this](qint64 user, qint64 message, QString const& error_message) {
                if (!error_message.isEmpty())
                {
                    chat_page_->set_message_error(user, error_message);
                    return;
                }
                chat_page_->set_read_message(user, chat_page_->self_user(), message);
                client_->get_conversations();
            },
            Qt::AutoConnection);
    (username_edit_->text().isEmpty() ? username_edit_ : password_edit_)->setFocus();
}

main_window::~main_window() { client_.reset(); }

QStringList main_window::recent_accounts() const
{
    // Only the installed client has an identity to store settings under; tests and tools never persist.
    if (QCoreApplication::organizationName().isEmpty()) { return {}; }
    return QSettings().value(QStringLiteral("login/recent_accounts")).toStringList();
}

void main_window::remember_account(QString const& username)
{
    if (QCoreApplication::organizationName().isEmpty() || username.isEmpty()) { return; }
    auto accounts = recent_accounts();
    accounts.removeAll(username);
    accounts.prepend(username);
    constexpr int max_recent_accounts = 5;
    QSettings().setValue(QStringLiteral("login/recent_accounts"), accounts.mid(0, max_recent_accounts));
    update_login_identity();
}

void main_window::update_login_identity()
{
    auto const username = username_edit_->text();
    auto const size = chat_theme::login_avatar_size;
    login_avatar_->setPixmap(username.isEmpty()
        ? svg_icon(u"chat", Qt::white, QSize(size / 2, size / 2)).pixmap(size / 2, size / 2)
        : avatar_icon(username, size).pixmap(size, size));
    login_avatar_->setProperty("empty", username.isEmpty());
    login_avatar_->style()->unpolish(login_avatar_);
    login_avatar_->style()->polish(login_avatar_);
    recent_accounts_action_->setVisible(!recent_accounts().isEmpty());
}

void main_window::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow())
    {
        QTimer::singleShot(0, this, [this] { chat_page_->mark_visible_messages(); });
    }
}

bool main_window::reading_conversation(qint64 conversation) const
{
    return connected_ && !reconnecting_ &&
        pages_->currentWidget() == chat_page_ && chat_page_->active_conversation() == conversation && chat_page_->viewing_latest();
}

void main_window::notify_message(message_data const& message)
{
    if (!connected_ || reconnecting_ || pages_->currentWidget() != chat_page_ ||
        message.deleted || message.from == chat_page_->self_user() || reading_conversation(message.conversation)) { return; }
    auto const conversation = chat_page_->conversation(message.conversation);
    if (!conversation)
    {
        pending_notifications_.push_back(message);
        return;
    }
    if (conversation->muted) { return; }
    auto const title = conversation->group
        ? QStringLiteral("%1 · %2").arg(grapheme_prefix(conversation->username, 80), grapheme_prefix(message.username, 80))
        : grapheme_prefix(message.username, 80);
    auto summary = message.text.simplified();
    if (message.attachment)
    {
        summary = (message.attachment->media_type.startsWith(QStringLiteral("image/"))
            ? QStringLiteral("图片：") : QStringLiteral("文件：")) + message.attachment->filename;
    }
    emit notification_requested(message.conversation, title, grapheme_prefix(summary, 120));
}

void main_window::start_login()
{
    auto const server = server_edit_->text().trimmed();
    auto const username = username_edit_->text();
    auto const password = password_edit_->text();

    if (server.isEmpty())
    {
        show_login_error(QStringLiteral("请在服务器设置中填写连接地址"));
        return;
    }
    if (username.isEmpty() || password.isEmpty())
    {
        show_login_error(QStringLiteral("请输入用户名和密码"));
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
        show_registration_error(QStringLiteral("请先在登录页的服务器设置中填写连接地址"));
        return;
    }
    if (username.isEmpty() || password.isEmpty() || password_confirm.isEmpty())
    {
        show_registration_error(QStringLiteral("用户名和密码不能为空"));
        return;
    }
    if (!chat::valid_username(username.toUtf8().toStdString()))
    {
        show_registration_error(QStringLiteral("用户名须为 1–64 UTF-8 字节，首尾不能有空白，且不能含 @ 或控制字符"));
        return;
    }
    if (password != password_confirm)
    {
        show_registration_error(QStringLiteral("两次输入的密码不一致"));
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
    pending_notifications_.clear();
    tray_->hide();
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
        username_edit_->setFocus();
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
        username_edit_->setFocus();
    }
}

void main_window::set_login_busy(bool busy)
{
    username_edit_->setEnabled(!busy);
    password_edit_->setEnabled(!busy);
    login_button_->setEnabled(!busy);
    login_button_->setText(busy ? QStringLiteral("请稍候…") : QStringLiteral("登录"));
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
    set_login_busy(false);
    status_label_->show_error(std::move(message));
}

void main_window::show_registration_error(QString message)
{
    pending_action_ = pending_action::none;
    pending_password_.clear();
    set_registration_busy(false);
    set_login_busy(false);
    registration_status_label_->show_error(std::move(message));
}

void main_window::show_authenticated_page(qint64 user)
{
    chat_page_->set_user(pending_username_, user);
    chat_page_->set_connection_available(true);
    chat_page_->set_connection_status({}, false);
    chat_page_->set_loading();
    pages_->setCurrentWidget(chat_page_);
    tray_->show();
    client_->get_conversations();
    client_->get_contacts();
    client_->get_friend_requests();
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
    chat_page_->avatars().retry();

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
    client_->get_friend_requests();
    auto const user = chat_page_->active_conversation();
    if (user > 0)
    {
        client_->get_messages(user, {}, chat_page_->recovery_cursor());
        client_->get_members(user);
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
    pending_notifications_.clear();
    tray_->hide();
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
