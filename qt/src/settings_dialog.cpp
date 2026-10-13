#include "settings_dialog.hpp"

#include "icons.hpp"
#include "theme_manager.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QToolButton>
#include <QTransform>
#include <QVBoxLayout>

#include <array>
#include <utility>

namespace
{
QString page_title(int page)
{
    static std::array<QString, 5> const titles = {
        QStringLiteral("设置"), QStringLiteral("通知"), QStringLiteral("外观与聊天"),
        QStringLiteral("桌面与窗口"), QStringLiteral("关于Chat")};
    return titles.at(static_cast<std::size_t>(page));
}
}

settings_dialog::settings_dialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(QStringLiteral("设置"));
    setAccessibleName(QStringLiteral("设置"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowModality(Qt::WindowModal);
    resize(520, 580);
    setMinimumSize(420, 400);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* header = new QHBoxLayout;
    header->setContentsMargins(12, 12, 12, 8);
    header->setSpacing(8);
    back_ = new QToolButton(this);
    back_->setObjectName(QStringLiteral("settingsBackButton"));
    back_->setAccessibleName(QStringLiteral("返回设置首页"));
    back_->setToolTip(back_->accessibleName());
    back_->setFixedSize(40, 40);
    back_->setCursor(Qt::PointingHandCursor);
    header->addWidget(back_);
    title_ = new QLabel(QStringLiteral("设置"), this);
    title_->setObjectName(QStringLiteral("settingsTitle"));
    header->addWidget(title_, 1);
    close_ = new QToolButton(this);
    close_->setObjectName(QStringLiteral("settingsCloseButton"));
    close_->setAccessibleName(QStringLiteral("关闭设置"));
    close_->setToolTip(close_->accessibleName());
    close_->setFixedSize(40, 40);
    close_->setCursor(Qt::PointingHandCursor);
    header->addWidget(close_);
    outer->addLayout(header);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("settingsPages"));
    outer->addWidget(pages_, 1);
    auto make_page = [this](QString name) {
        auto* scroll = new QScrollArea(pages_);
        scroll->setObjectName(name + QStringLiteral("Scroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* body = new QWidget(scroll);
        body->setObjectName(name + QStringLiteral("Page"));
        auto* layout = new QVBoxLayout(body);
        layout->setContentsMargins(20, 12, 20, 20);
        layout->setSpacing(16);
        scroll->setWidget(body);
        pages_->addWidget(scroll);
        return layout;
    };
    auto hint = [](QVBoxLayout* layout, QString text, QString name = QStringLiteral("settingsHint")) {
        auto* label = new QLabel(std::move(text), layout->parentWidget());
        label->setObjectName(std::move(name));
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        layout->addWidget(label);
        return label;
    };
    auto checkbox = [](QVBoxLayout* layout, QString text, QString name) {
        auto* option = new QCheckBox(std::move(text), layout->parentWidget());
        option->setObjectName(std::move(name));
        option->setAccessibleName(option->text());
        layout->addWidget(option);
        return option;
    };

    auto* home = make_page(QStringLiteral("settingsHome"));
    account_ = new QToolButton(home->parentWidget());
    account_->setObjectName(QStringLiteral("settingsAccountButton"));
    account_->setAccessibleName(QStringLiteral("我的账号"));
    account_->setToolTip(QStringLiteral("查看我的账号"));
    account_->setCursor(Qt::PointingHandCursor);
    account_->setMinimumHeight(80);
    account_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto* identity = new QHBoxLayout(account_);
    identity->setContentsMargins(12, 12, 12, 12);
    identity->setSpacing(16);
    account_avatar_ = new QLabel(account_);
    account_avatar_->setObjectName(QStringLiteral("settingsAccountAvatar"));
    account_avatar_->setFixedSize(56, 56);
    account_avatar_->setAttribute(Qt::WA_TransparentForMouseEvents);
    identity->addWidget(account_avatar_);
    auto* identity_text = new QVBoxLayout;
    identity_text->setSpacing(4);
    auto* account_caption = new QLabel(QStringLiteral("我的账号"), account_);
    account_caption->setObjectName(QStringLiteral("settingsAccountCaption"));
    account_caption->setAttribute(Qt::WA_TransparentForMouseEvents);
    identity_text->addWidget(account_caption);
    account_name_ = new QLabel(account_);
    account_name_->setObjectName(QStringLiteral("settingsAccountName"));
    account_name_->setTextFormat(Qt::PlainText);
    account_name_->setWordWrap(true);
    account_name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    account_name_->setAttribute(Qt::WA_TransparentForMouseEvents);
    identity_text->addWidget(account_name_);
    identity->addLayout(identity_text, 1);
    home->addWidget(account_);
    static std::array<QString, 4> const section_names = {
        QStringLiteral("settingsNotificationsButton"), QStringLiteral("settingsAppearanceChatButton"),
        QStringLiteral("settingsDesktopWindowButton"), QStringLiteral("settingsAboutButton")};
    for (int page = 1; page <= 4; ++page)
    {
        auto* button = new QPushButton(home->parentWidget());
        button->setObjectName(section_names.at(static_cast<std::size_t>(page - 1)));
        button->setProperty("settingsSection", true);
        button->setAccessibleName(page_title(page));
        button->setAutoDefault(false);
        button->setMinimumHeight(48);
        button->setCursor(Qt::PointingHandCursor);
        auto* row = new QHBoxLayout(button);
        row->setContentsMargins(16, 0, 16, 0);
        row->setSpacing(12);
        auto* label = new QLabel(page_title(page), button);
        label->setObjectName(QStringLiteral("settingsSectionLabel"));
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(label, 1);
        auto* arrow = new QLabel(button);
        arrow->setObjectName(QStringLiteral("settingsSectionArrow"));
        arrow->setFixedSize(20, 20);
        arrow->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(arrow);
        home->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, page] { show_page(page); });
    }
    home->addStretch();
    auto* logout = new QPushButton(QStringLiteral("退出登录"), home->parentWidget());
    logout->setObjectName(QStringLiteral("settingsLogoutButton"));
    logout->setAccessibleName(logout->text());
    logout->setAutoDefault(false);
    logout->setMinimumHeight(40);
    home->addWidget(logout);
    connect(account_, &QToolButton::clicked, this, &settings_dialog::account_requested);
    connect(logout, &QPushButton::clicked, this, &settings_dialog::logout_requested);

    auto* notifications = make_page(QStringLiteral("settingsNotifications"));
    desktop_ = checkbox(notifications, QStringLiteral("启用桌面通知"), QStringLiteral("settingsDesktopNotifications"));
    preview_ = checkbox(notifications, QStringLiteral("显示消息预览"), QStringLiteral("settingsMessagePreview"));
    hint(notifications, QStringLiteral("关闭消息预览后，通知仅显示 Chat 和“收到新消息”，不显示发送者、会话名称或消息内容。系统是否显示通知取决于桌面通知支持。"));
    notifications->addStretch();
    connect(desktop_, &QCheckBox::toggled, this, [this](bool enabled) {
        preview_->setEnabled(enabled);
        emit desktop_notifications_changed(enabled);
    });
    connect(preview_, &QCheckBox::toggled, this, &settings_dialog::message_preview_changed);

    auto* chat = make_page(QStringLiteral("settingsAppearanceChat"));
    auto* appearance_label = hint(chat, QStringLiteral("外观主题"), QStringLiteral("settingsSectionHeading"));
    appearance_ = new QComboBox(chat->parentWidget());
    appearance_->setObjectName(QStringLiteral("settingsAppearanceMode"));
    appearance_->setAccessibleName(QStringLiteral("外观主题"));
    appearance_->addItem(QStringLiteral("跟随系统"), static_cast<int>(chat_appearance::system));
    appearance_->addItem(QStringLiteral("浅色 · Daylight"), static_cast<int>(chat_appearance::light));
    appearance_->addItem(QStringLiteral("深色 · Midnight"), static_cast<int>(chat_appearance::dark));
    appearance_label->setBuddy(appearance_);
    chat->addWidget(appearance_);
    auto& themes = theme_manager::instance();
    appearance_->setCurrentIndex(appearance_->findData(static_cast<int>(themes.appearance())));
    connect(appearance_, &QComboBox::currentIndexChanged, this, [this](int) {
        theme_manager::instance().set_appearance(static_cast<chat_appearance>(appearance_->currentData().toInt()));
    });
    control_enter_ = checkbox(chat, QStringLiteral("按 Ctrl+Enter 发送消息"), QStringLiteral("settingsControlEnter"));
    hint(chat, QStringLiteral("默认按 Enter 发送，Shift+Enter 换行。启用后按 Ctrl+Enter 发送，Enter 换行。"));
    chat->addStretch();
    connect(control_enter_, &QCheckBox::toggled, this, &settings_dialog::control_enter_changed);

    auto* desktop = make_page(QStringLiteral("settingsDesktopWindow"));
    close_to_tray_ = checkbox(desktop, QStringLiteral("关闭窗口时最小化到托盘"), QStringLiteral("settingsCloseToTray"));
    hint(desktop, QStringLiteral("启用后可从托盘重新打开窗口，或从托盘菜单退出应用。"));
    tray_unavailable_ = hint(desktop, QStringLiteral("当前桌面没有可用的系统托盘，关闭窗口将退出应用。"),
                             QStringLiteral("settingsTrayUnavailableHint"));
    desktop->addStretch();
    connect(close_to_tray_, &QCheckBox::toggled, this, &settings_dialog::close_to_tray_changed);

    auto* about = make_page(QStringLiteral("settingsAbout"));
    hint(about, QStringLiteral("Chat\n开发版本\nQt %1\n项目尚未声明许可证。").arg(QString::fromLatin1(qVersion())),
         QStringLiteral("settingsAboutText"));
    repository_ = new QLabel(about->parentWidget());
    repository_->setObjectName(QStringLiteral("settingsRepositoryLink"));
    repository_->setAccessibleName(QStringLiteral("打开 Chat 项目仓库"));
    repository_->setWordWrap(true);
    repository_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    repository_->setOpenExternalLinks(true);
    about->addWidget(repository_);
    about->addStretch();

    connect(back_, &QToolButton::clicked, this, [this] { show_page(0); });
    connect(close_, &QToolButton::clicked, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this] { show_page(0); });
    connect(&themes, &theme_manager::changed, this, [this] {
        QSignalBlocker blocker(appearance_);
        appearance_->setCurrentIndex(appearance_->findData(static_cast<int>(theme_manager::instance().appearance())));
        refresh_icons();
    });
    refresh_icons();
    show_page(0);
}

void settings_dialog::set_account(QString username, QIcon avatar)
{
    account_->setEnabled(!username.isEmpty());
    account_name_->setText(username.isEmpty() ? QStringLiteral("未登录") : username);
    account_->setToolTip(username.isEmpty() ? QStringLiteral("我的账号") : username);
    account_avatar_->setPixmap(avatar.pixmap(QSize(56, 56)));
}

void settings_dialog::set_options(bool desktop, bool preview, bool control_enter, bool close_to_tray, bool tray_available)
{
    QSignalBlocker desktop_blocker(desktop_);
    QSignalBlocker preview_blocker(preview_);
    QSignalBlocker control_blocker(control_enter_);
    QSignalBlocker tray_blocker(close_to_tray_);
    desktop_->setChecked(desktop);
    preview_->setChecked(preview);
    preview_->setEnabled(desktop);
    control_enter_->setChecked(control_enter);
    close_to_tray_->setChecked(close_to_tray);
    close_to_tray_->setEnabled(tray_available);
    tray_unavailable_->setVisible(!tray_available);
}

void settings_dialog::show_page(int page)
{
    pages_->setCurrentIndex(page);
    title_->setText(page_title(page));
    back_->setVisible(page != 0);
    if (page == 0) { account_->setFocus(); }
    else { back_->setFocus(); }
}

void settings_dialog::refresh_icons()
{
    repository_->setText(QStringLiteral("<a href=\"https://github.com/gyl30/chat\" style=\"color:%1\">项目仓库：github.com/gyl30/chat</a>")
                         .arg(themed("link").name()));
    close_->setIcon(svg_icon(u"close", themed("text_secondary"), QSize(20, 20)));
    auto const down = svg_icon(u"chevron-down", themed("text_secondary"), QSize(20, 20));
    back_->setIcon(QIcon(down.pixmap(QSize(20, 20)).transformed(QTransform().rotate(90))));
    auto const right = down.pixmap(QSize(20, 20)).transformed(QTransform().rotate(-90));
    for (auto* arrow : findChildren<QLabel*>(QStringLiteral("settingsSectionArrow")))
    {
        arrow->setPixmap(right);
    }
}

void settings_dialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && pages_->currentIndex() != 0)
    {
        show_page(0);
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}
