#include "chat_widget.hpp"

#include <utility>

#include <QAbstractItemView>
#include <QClipboard>
#include <QColor>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "avatar.hpp"
#include "conversation_delegate.hpp"
#include "conversation_model.hpp"
#include "icons.hpp"
#include "message_delegate.hpp"
#include "message_model.hpp"
#include "theme.hpp"
#include "user_delegate.hpp"
#include "user_model.hpp"

namespace
{

QToolButton* make_navigation_button(
    QString text, QStringView icon, QWidget* parent, bool selected, bool enabled)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(selected ? QStringLiteral("navigationSelected") : QStringLiteral("navigationButton"));
    button->setText(std::move(text));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setIcon(svg_icon(
        icon,
        selected ? QColor(QStringLiteral("#FFFFFF")) : QColor(QStringLiteral("#C4D2CB")),
        QSize(23, 23)));
    button->setIconSize(QSize(23, 23));
    button->setEnabled(enabled);
    button->setFixedSize(64, 64);
    button->setCursor(enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
    return button;
}

void set_navigation_button(QToolButton* button, QStringView icon, bool selected)
{
    button->setObjectName(selected ? QStringLiteral("navigationSelected") : QStringLiteral("navigationButton"));
    button->setIcon(svg_icon(
        icon, selected ? QColor(QStringLiteral("#FFFFFF")) : QColor(QStringLiteral("#C4D2CB")), QSize(23, 23)));
    button->style()->unpolish(button);
    button->style()->polish(button);
}

QFrame* make_separator(QWidget* parent)
{
    auto* separator = new QFrame(parent);
    separator->setObjectName(QStringLiteral("separator"));
    separator->setFrameShape(QFrame::VLine);
    separator->setFixedWidth(1);
    return separator;
}

}    // namespace

chat_widget::chat_widget(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* navigation_panel = new QFrame(this);
    navigation_panel->setObjectName(QStringLiteral("navigationPanel"));
    navigation_panel->setFixedWidth(82);
    auto* navigation_layout = new QVBoxLayout(navigation_panel);
    navigation_layout->setContentsMargins(9, 18, 9, 14);
    navigation_layout->setSpacing(6);

    auto* brand = new QLabel(QStringLiteral("轻聊"), navigation_panel);
    brand->setObjectName(QStringLiteral("brandLabel"));
    brand->setAlignment(Qt::AlignCenter);
    brand->setFixedWidth(64);
    navigation_layout->addWidget(brand, 0, Qt::AlignHCenter);
    navigation_layout->addSpacing(14);
    chats_navigation_ =
        make_navigation_button(QStringLiteral("聊天"), QStringLiteral("chat"), navigation_panel, true, true);
    navigation_layout->addWidget(chats_navigation_, 0, Qt::AlignHCenter);
    contacts_navigation_ =
        make_navigation_button(QStringLiteral("联系人"), QStringLiteral("contacts"), navigation_panel, false, true);
    navigation_layout->addWidget(contacts_navigation_, 0, Qt::AlignHCenter);
    navigation_layout->addWidget(
        make_navigation_button(QStringLiteral("群组"), QStringLiteral("groups"), navigation_panel, false, false),
        0, Qt::AlignHCenter);
    navigation_layout->addWidget(
        make_navigation_button(QStringLiteral("动态"), QStringLiteral("activity"), navigation_panel, false, false),
        0, Qt::AlignHCenter);
    navigation_layout->addWidget(
        make_navigation_button(QStringLiteral("收藏"), QStringLiteral("bookmark"), navigation_panel, false, false),
        0, Qt::AlignHCenter);
    navigation_layout->addStretch();

    logout_navigation_ =
        make_navigation_button(QStringLiteral("退出"), QStringLiteral("close"), navigation_panel, false, true);
    navigation_layout->addWidget(logout_navigation_, 0, Qt::AlignHCenter);

    profile_avatar_ = new QLabel(QStringLiteral("?"), navigation_panel);
    profile_avatar_->setObjectName(QStringLiteral("profileAvatar"));
    profile_avatar_->setAlignment(Qt::AlignCenter);
    profile_avatar_->setFixedSize(44, 44);
    navigation_layout->addWidget(profile_avatar_, 0, Qt::AlignHCenter);

    auto* conversation_panel = new QFrame(this);
    conversation_panel->setObjectName(QStringLiteral("conversationPanel"));
    conversation_panel->setFixedWidth(332);
    auto* conversation_layout = new QVBoxLayout(conversation_panel);
    conversation_layout->setContentsMargins(0, 0, 0, 0);
    conversation_layout->setSpacing(0);

    auto* conversation_header = new QFrame(conversation_panel);
    auto* conversation_header_layout = new QHBoxLayout(conversation_header);
    conversation_header_layout->setContentsMargins(12, 16, 12, 8);
    conversation_header_layout->setSpacing(6);
    sidebar_back_button_ = new QToolButton(conversation_header);
    sidebar_back_button_->setObjectName(QStringLiteral("sidebarHeaderButton"));
    sidebar_back_button_->setText(QStringLiteral("‹"));
    sidebar_back_button_->setFixedSize(32, 32);
    sidebar_back_button_->setCursor(Qt::PointingHandCursor);
    sidebar_back_button_->hide();
    conversation_header_layout->addWidget(sidebar_back_button_);
    section_title_ = new QLabel(QStringLiteral("消息"), conversation_header);
    section_title_->setObjectName(QStringLiteral("sectionTitle"));
    conversation_header_layout->addWidget(section_title_);
    conversation_header_layout->addStretch();
    add_contact_button_ = new QToolButton(conversation_header);
    add_contact_button_->setObjectName(QStringLiteral("sidebarTextButton"));
    add_contact_button_->setText(QStringLiteral("添加联系人"));
    add_contact_button_->setCursor(Qt::PointingHandCursor);
    add_contact_button_->hide();
    conversation_header_layout->addWidget(add_contact_button_);
    conversation_layout->addWidget(conversation_header);

    sidebar_pages_ = new QStackedWidget(conversation_panel);

    auto* conversations_page = new QWidget(sidebar_pages_);
    auto* conversations_layout = new QVBoxLayout(conversations_page);
    conversations_layout->setContentsMargins(0, 0, 0, 0);
    conversations_layout->setSpacing(0);
    conversations_status_ = new QLabel(conversations_page);
    conversations_status_->setObjectName(QStringLiteral("subtleText"));
    conversations_status_->setContentsMargins(18, 0, 14, 6);
    conversations_layout->addWidget(conversations_status_);

    conversations_ = new conversation_model(this);
    conversations_view_ = new QListView(conversations_page);
    conversations_view_->setObjectName(QStringLiteral("conversationList"));
    conversations_view_->setModel(conversations_);
    auto* conversations_delegate = new conversation_delegate(conversations_view_);
    conversations_view_->setItemDelegate(conversations_delegate);
    conversations_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    conversations_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    conversations_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    conversations_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    conversations_view_->setMouseTracking(true);
    conversations_view_->verticalScrollBar()->setSingleStep(24);
    conversations_layout->addWidget(conversations_view_, 1);
    sidebar_pages_->addWidget(conversations_page);

    auto* contacts_page = new QWidget(sidebar_pages_);
    auto* contacts_layout = new QVBoxLayout(contacts_page);
    contacts_layout->setContentsMargins(0, 0, 0, 0);
    contacts_layout->setSpacing(0);
    contact_search_ = new QLineEdit(contacts_page);
    contact_search_->setObjectName(QStringLiteral("userSearchEdit"));
    contact_search_->setPlaceholderText(QStringLiteral("搜索联系人"));
    contact_search_->setClearButtonEnabled(false);
    contacts_layout->addWidget(contact_search_);
    contacts_status_ = new QLabel(QStringLiteral("暂无联系人"), contacts_page);
    contacts_status_->setObjectName(QStringLiteral("subtleText"));
    contacts_status_->setContentsMargins(18, 8, 14, 8);
    contacts_layout->addWidget(contacts_status_);

    contacts_ = new user_model(this);
    contacts_filter_ = new QSortFilterProxyModel(this);
    contacts_filter_->setSourceModel(contacts_);
    contacts_filter_->setFilterRole(user_model::username_role);
    contacts_filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    contacts_view_ = new QListView(contacts_page);
    contacts_view_->setObjectName(QStringLiteral("userList"));
    contacts_view_->setModel(contacts_filter_);
    contacts_view_->setItemDelegate(new user_delegate(contacts_view_));
    contacts_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    contacts_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    contacts_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    contacts_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    contacts_view_->setMouseTracking(true);
    contacts_view_->verticalScrollBar()->setSingleStep(24);
    contacts_layout->addWidget(contacts_view_, 1);
    sidebar_pages_->addWidget(contacts_page);

    auto* add_contacts_page = new QWidget(sidebar_pages_);
    auto* add_contacts_layout = new QVBoxLayout(add_contacts_page);
    add_contacts_layout->setContentsMargins(0, 0, 0, 0);
    add_contacts_layout->setSpacing(0);
    add_user_search_ = new QLineEdit(add_contacts_page);
    add_user_search_->setObjectName(QStringLiteral("userSearchEdit"));
    add_user_search_->setPlaceholderText(QStringLiteral("搜索用户"));
    add_user_search_->setClearButtonEnabled(false);
    add_contacts_layout->addWidget(add_user_search_);
    add_users_status_ = new QLabel(QStringLiteral("输入用户名搜索"), add_contacts_page);
    add_users_status_->setObjectName(QStringLiteral("subtleText"));
    add_users_status_->setContentsMargins(18, 8, 14, 8);
    add_contacts_layout->addWidget(add_users_status_);

    add_users_ = new user_model(this);
    add_users_view_ = new QListView(add_contacts_page);
    add_users_view_->setObjectName(QStringLiteral("userList"));
    add_users_view_->setModel(add_users_);
    add_users_view_->setItemDelegate(new user_delegate(add_users_view_));
    add_users_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    add_users_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    add_users_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    add_users_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    add_users_view_->setMouseTracking(true);
    add_users_view_->verticalScrollBar()->setSingleStep(24);
    add_contacts_layout->addWidget(add_users_view_, 1);
    sidebar_pages_->addWidget(add_contacts_page);

    conversation_layout->addWidget(sidebar_pages_, 1);

    auto* chat_panel = new QFrame(this);
    chat_panel->setObjectName(QStringLiteral("chatPanel"));
    chat_panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* chat_layout = new QVBoxLayout(chat_panel);
    chat_layout->setContentsMargins(0, 0, 0, 0);
    chat_layout->setSpacing(0);

    auto* header = new QFrame(chat_panel);
    header->setObjectName(QStringLiteral("chatHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(16, 7, 12, 7);
    header_layout->setSpacing(8);
    chat_title_ = new QPushButton(QStringLiteral("聊天"), header);
    chat_title_->setObjectName(QStringLiteral("chatHeaderButton"));
    chat_title_->setFlat(true);
    chat_title_->setEnabled(false);
    chat_title_->setCursor(Qt::PointingHandCursor);
    chat_title_->setIconSize(QSize(38, 38));
    header_layout->addWidget(chat_title_);
    header_layout->addStretch();
    connection_status_ = new QToolButton(header);
    connection_status_->setObjectName(QStringLiteral("connectionStatusButton"));
    connection_status_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connection_status_->setCursor(Qt::PointingHandCursor);
    connection_status_->hide();
    header_layout->addWidget(connection_status_);
    chat_layout->addWidget(header);

    auto* chat_line = new QFrame(chat_panel);
    chat_line->setFrameShape(QFrame::HLine);
    chat_line->setObjectName(QStringLiteral("horizontalSeparator"));
    chat_layout->addWidget(chat_line);

    message_status_ = new QLabel(QStringLiteral("选择一个会话开始聊天"), chat_panel);
    message_status_->setObjectName(QStringLiteral("subtleText"));
    message_status_->setAlignment(Qt::AlignCenter);
    message_status_->setContentsMargins(0, 6, 0, 6);
    chat_layout->addWidget(message_status_);

    messages_ = new message_model(this);
    messages_view_ = new QListView(chat_panel);
    messages_view_->setObjectName(QStringLiteral("messageList"));
    messages_view_->setModel(messages_);
    auto* messages_delegate = new message_delegate(messages_view_);
    messages_view_->setItemDelegate(messages_delegate);
    messages_view_->setSelectionMode(QAbstractItemView::NoSelection);
    messages_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    messages_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    messages_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messages_view_->setSpacing(0);
    messages_view_->verticalScrollBar()->setSingleStep(24);
    chat_layout->addWidget(messages_view_, 1);

    auto* input_separator = new QFrame(chat_panel);
    input_separator->setFrameShape(QFrame::HLine);
    input_separator->setObjectName(QStringLiteral("horizontalSeparator"));
    chat_layout->addWidget(input_separator);

    auto* input_bar = new QFrame(chat_panel);
    input_bar->setObjectName(QStringLiteral("inputBar"));
    input_bar->setMinimumHeight(chat_theme::compose_height);
    auto* input_layout = new QHBoxLayout(input_bar);
    input_layout->setContentsMargins(12, 5, 8, 5);
    input_layout->setSpacing(4);
    message_edit_ = new QLineEdit(input_bar);
    message_edit_->setObjectName(QStringLiteral("messageEdit"));
    message_edit_->setPlaceholderText(QStringLiteral("输入消息…"));
    message_edit_->setMinimumHeight(chat_theme::compose_field_min_height);
    message_edit_->setEnabled(false);
    input_layout->addWidget(message_edit_, 1);
    send_button_ = new QToolButton(input_bar);
    send_button_->setObjectName(QStringLiteral("sendButton"));
    send_button_->setIcon(svg_icon(QStringLiteral("send"), QColor(QStringLiteral("#315A4B")), QSize(22, 22)));
    send_button_->setIconSize(QSize(22, 22));
    send_button_->setFixedSize(chat_theme::compose_button_width, chat_theme::compose_button_height);
    send_button_->setEnabled(false);
    send_button_->setCursor(Qt::PointingHandCursor);
    input_layout->addWidget(send_button_);
    chat_layout->addWidget(input_bar);

    layout->addWidget(navigation_panel);
    layout->addWidget(conversation_panel);
    layout->addWidget(make_separator(this));
    layout->addWidget(chat_panel, 1);

    connect(chats_navigation_, &QToolButton::clicked, this, [this] { show_conversations_section(); });
    connect(contacts_navigation_, &QToolButton::clicked, this, [this] { show_contacts_section(); });
    connect(logout_navigation_, &QToolButton::clicked, this, [this] { emit logout_requested(); });
    connect(sidebar_back_button_, &QToolButton::clicked, this, [this] { show_contacts_section(); });
    connect(add_contact_button_, &QToolButton::clicked, this, [this] { show_add_contact_section(); });
    connect(contact_search_, &QLineEdit::textChanged, this, [this](QString const& query) { filter_contacts(query); });
    connect(contacts_view_, &QListView::clicked, this, [this](QModelIndex const& index) { select_contact(index); });
    connect(add_user_search_, &QLineEdit::returnPressed, this, [this] { search_users(); });
    connect(add_users_view_, &QListView::clicked, this, [this](QModelIndex const& index) { select_add_user(index); });
    connect(conversations_view_, &QListView::clicked, this, [this](QModelIndex const& index) { select_conversation(index); });
    connect(conversations_delegate, &conversation_delegate::avatar_clicked, this, [this](QModelIndex const& index) {
        if (auto const* item = conversations_->conversation_at(index))
        {
            show_user_details(item->user, item->username);
        }
    });
    connect(messages_delegate, &message_delegate::avatar_clicked, this, [this](QModelIndex const&) {
        if (active_user_ > 0)
        {
            show_user_details(active_user_, active_username_);
        }
    });
    connect(chat_title_, &QPushButton::clicked, this, [this] {
        if (active_user_ > 0)
        {
            show_user_details(active_user_, active_username_);
        }
    });
    connect(connection_status_, &QToolButton::clicked, this, [this] {
        if (connection_status_->isEnabled())
        {
            emit reconnect_requested();
        }
    });
    connect(send_button_, &QToolButton::clicked, this, [this] { send_current_message(); });
    connect(message_edit_, &QLineEdit::returnPressed, this, [this] { send_current_message(); });
    connect(messages_view_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (value == messages_view_->verticalScrollBar()->minimum())
        {
            request_older_messages();
        }
    });
}

void chat_widget::set_user(QString const& username)
{
    profile_avatar_->setText(avatar_initial(username));
    profile_avatar_->setToolTip(username);
    profile_avatar_->setStyleSheet(QStringLiteral("background: %1;").arg(avatar_background(username).name()));
    active_user_ = 0;
    active_username_.clear();
    conversations_->set_conversations({});
    conversations_view_->setCurrentIndex({});
    conversations_status_->setText(QStringLiteral("暂无会话"));
    messages_->set_self_username(username);
    messages_->reset(0);
    contacts_->set_users({});
    contacts_filter_->setFilterRegularExpression(QRegularExpression{});
    contact_search_->clear();
    contacts_status_->setText(QStringLiteral("暂无联系人"));
    add_users_->set_users({});
    add_user_search_->clear();
    add_users_status_->setText(QStringLiteral("输入用户名搜索"));
    show_conversations_section();
    chat_title_->setText(QStringLiteral("聊天"));
    chat_title_->setIcon(QIcon{});
    chat_title_->setEnabled(false);
    set_message_status(QStringLiteral("选择一个会话开始聊天"));
    message_edit_->clear();
    message_edit_->setEnabled(false);
    send_button_->setEnabled(false);
}

void chat_widget::set_loading() { conversations_status_->setText(QStringLiteral("正在加载…")); }

void chat_widget::set_error(QString message) { conversations_status_->setText(std::move(message)); }

void chat_widget::set_connection_available(bool available)
{
    connection_available_ = available;
    message_edit_->setEnabled(available && active_user_ > 0);
    send_button_->setEnabled(available && active_user_ > 0);
    add_contact_button_->setEnabled(available);
    add_user_search_->setEnabled(available);
}

void chat_widget::set_connection_status(QString text, bool retry_enabled)
{
    if (text.isEmpty())
    {
        connection_status_->hide();
        return;
    }

    connection_status_->setText(std::move(text));
    connection_status_->setEnabled(retry_enabled);
    connection_status_->setCursor(retry_enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
    connection_status_->show();
}

void chat_widget::set_conversations(QList<conversation_data> conversations)
{
    auto const previous_user = active_user_;
    conversations_->set_conversations(std::move(conversations));
    if (conversations_->rowCount() == 0)
    {
        conversations_status_->setText(QStringLiteral("暂无会话"));
        conversations_view_->setCurrentIndex({});
        return;
    }

    conversations_status_->clear();
    if (previous_user > 0)
    {
        auto const index = conversations_->index_for_user(previous_user);
        if (!index.isValid())
        {
            conversations_view_->setCurrentIndex({});
            conversations_view_->clearSelection();
            return;
        }

        conversations_view_->setCurrentIndex(index);
        if (auto const* item = conversations_->conversation_at(index))
        {
            active_username_ = item->username;
            update_chat_header(item->username);
        }
        return;
    }

    auto const index = conversations_->index(0, 0);
    conversations_view_->setCurrentIndex(index);
    select_conversation(index);
}

void chat_widget::set_contacts(QList<user_data> contacts)
{
    contacts_->set_users(std::move(contacts));
    filter_contacts(contact_search_->text());
}

void chat_widget::set_contacts_error(QString message)
{
    contacts_->set_users({});
    contacts_status_->setText(std::move(message));
}

void chat_widget::set_add_contact_search_results(QList<user_data> users)
{
    add_users_->set_users(std::move(users));
    add_users_status_->setText(add_users_->rowCount() == 0 ? QStringLiteral("没有找到可添加的用户") : QString{});
}

void chat_widget::set_add_contact_search_error(QString message)
{
    add_users_->set_users({});
    add_users_status_->setText(std::move(message));
}

void chat_widget::finish_add_contact()
{
    add_user_search_->clear();
    add_users_->set_users({});
    add_users_status_->setText(QStringLiteral("输入用户名搜索"));
    show_contacts_section();
    contacts_status_->setText(QStringLiteral("正在加载…"));
}

void chat_widget::set_messages(qint64 user, QList<message_data> messages, bool older)
{
    if (user != active_user_)
    {
        return;
    }

    auto const batch_size = messages.size();
    auto* scroll = messages_view_->verticalScrollBar();
    auto const old_maximum = scroll->maximum();
    auto const old_value = scroll->value();
    messages_->merge_messages(std::move(messages));
    messages_loading_ = false;
    history_exhausted_ = history_exhausted_ || batch_size < 50;

    if (!older)
    {
        messages_loaded_ = true;
        set_message_status(messages_->rowCount() == 0 ? QStringLiteral("暂无消息") : QString{});
        QTimer::singleShot(0, messages_view_, [view = messages_view_] { view->scrollToBottom(); });
        return;
    }

    set_message_status({});
    QTimer::singleShot(0, messages_view_, [scroll, old_maximum, old_value] {
        scroll->setValue(old_value + scroll->maximum() - old_maximum);
    });
}

void chat_widget::add_message(qint64 user, message_data message)
{
    if (user != active_user_)
    {
        return;
    }

    if (messages_->add_message(std::move(message)))
    {
        set_message_status({});
        QTimer::singleShot(0, messages_view_, [view = messages_view_] { view->scrollToBottom(); });
    }
}

void chat_widget::add_sent_message(qint64 user, qint64 message, qint64 timestamp, QString text)
{
    if (user != active_user_)
    {
        return;
    }

    message_data value;
    value.id = message;
    value.from = 0;
    value.timestamp = timestamp;
    value.text = std::move(text);
    add_message(user, std::move(value));
}

void chat_widget::set_message_error(qint64 user, QString message)
{
    if (user == active_user_)
    {
        messages_loading_ = false;
        set_message_status(std::move(message));
    }
}

qint64 chat_widget::active_user() const noexcept { return active_user_; }

qint64 chat_widget::latest_message_id() const { return messages_->last_message_id(); }

void chat_widget::show_conversations_section()
{
    section_title_->setText(QStringLiteral("消息"));
    sidebar_pages_->setCurrentIndex(0);
    sidebar_back_button_->hide();
    add_contact_button_->hide();
    set_navigation_button(chats_navigation_, QStringLiteral("chat"), true);
    set_navigation_button(contacts_navigation_, QStringLiteral("contacts"), false);
}

void chat_widget::show_contacts_section()
{
    section_title_->setText(QStringLiteral("联系人"));
    sidebar_pages_->setCurrentIndex(1);
    sidebar_back_button_->hide();
    add_contact_button_->show();
    set_navigation_button(chats_navigation_, QStringLiteral("chat"), false);
    set_navigation_button(contacts_navigation_, QStringLiteral("contacts"), true);
    contact_search_->setFocus();
}

void chat_widget::show_add_contact_section()
{
    section_title_->setText(QStringLiteral("添加联系人"));
    sidebar_pages_->setCurrentIndex(2);
    sidebar_back_button_->show();
    add_contact_button_->hide();
    set_navigation_button(chats_navigation_, QStringLiteral("chat"), false);
    set_navigation_button(contacts_navigation_, QStringLiteral("contacts"), true);
    add_user_search_->setFocus();
}

void chat_widget::filter_contacts(QString const& query)
{
    auto const trimmed = query.trimmed();
    if (trimmed.isEmpty())
    {
        contacts_filter_->setFilterRegularExpression(QRegularExpression{});
    }
    else
    {
        auto const pattern = QStringLiteral("^") + QRegularExpression::escape(trimmed);
        QRegularExpression expression(pattern, QRegularExpression::CaseInsensitiveOption);
        contacts_filter_->setFilterRegularExpression(expression);
    }

    if (contacts_->rowCount() == 0)
    {
        contacts_status_->setText(QStringLiteral("暂无联系人"));
    }
    else if (contacts_filter_->rowCount() == 0)
    {
        contacts_status_->setText(QStringLiteral("没有匹配的联系人"));
    }
    else
    {
        contacts_status_->clear();
    }
}

void chat_widget::search_users()
{
    if (!connection_available_)
    {
        return;
    }

    auto const query = add_user_search_->text().trimmed();
    if (query.isEmpty())
    {
        add_users_->set_users({});
        add_users_status_->setText(QStringLiteral("输入用户名搜索"));
        return;
    }

    add_users_status_->setText(QStringLiteral("正在搜索…"));
    emit add_contact_search_requested(query);
}

void chat_widget::select_contact(QModelIndex const& index)
{
    auto const source_index = contacts_filter_->mapToSource(index);
    auto const* item = contacts_->user_at(source_index);
    if (item)
    {
        open_chat(item->id, item->username);
    }
}

void chat_widget::select_add_user(QModelIndex const& index)
{
    if (!connection_available_)
    {
        return;
    }

    auto const* item = add_users_->user_at(index);
    if (!item)
    {
        return;
    }

    auto const user = item->id;
    auto const username = item->username;
    QMessageBox confirm(QMessageBox::Question, QStringLiteral("添加联系人"),
                        QStringLiteral("确定添加 %1 为联系人吗？").arg(username), QMessageBox::NoButton, this);
    auto* add_button = confirm.addButton(QStringLiteral("添加"), QMessageBox::AcceptRole);
    confirm.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    confirm.exec();
    if (confirm.clickedButton() != add_button)
    {
        return;
    }

    add_users_status_->setText(QStringLiteral("正在添加…"));
    emit contact_add_requested(user);
}

void chat_widget::select_conversation(QModelIndex const& index)
{
    auto const* item = conversations_->conversation_at(index);
    if (item)
    {
        open_chat(item->user, item->username);
    }
}

void chat_widget::open_chat(qint64 user, QString username)
{
    if (user <= 0)
    {
        return;
    }

    active_username_ = std::move(username);
    update_chat_header(active_username_);

    auto const conversation_index = conversations_->index_for_user(user);
    if (conversation_index.isValid())
    {
        conversations_view_->setCurrentIndex(conversation_index);
    }
    else
    {
        conversations_view_->setCurrentIndex({});
        conversations_view_->clearSelection();
    }

    if (active_user_ == user)
    {
        if (!messages_loaded_ && !messages_loading_)
        {
            messages_loading_ = true;
            set_message_status(QStringLiteral("正在加载消息…"));
            if (connection_available_)
            {
                emit conversation_selected(active_user_);
            }
        }
        return;
    }

    active_user_ = user;
    messages_->reset(active_user_, active_username_);
    messages_loaded_ = false;
    messages_loading_ = true;
    history_exhausted_ = false;
    set_message_status(QStringLiteral("正在加载消息…"));
    message_edit_->setEnabled(connection_available_);
    send_button_->setEnabled(connection_available_);
    if (connection_available_)
    {
        emit conversation_selected(active_user_);
    }
}

void chat_widget::request_older_messages()
{
    if (!connection_available_ || active_user_ <= 0 || !messages_loaded_ || messages_loading_ || history_exhausted_)
    {
        return;
    }

    auto const before = messages_->first_message_id();
    if (before <= 0)
    {
        history_exhausted_ = true;
        return;
    }

    messages_loading_ = true;
    emit older_messages_requested(active_user_, before);
}

void chat_widget::send_current_message()
{
    if (!connection_available_ || active_user_ <= 0 || message_edit_->text().isEmpty())
    {
        return;
    }

    auto text = message_edit_->text();
    message_edit_->clear();
    set_message_status({});
    emit send_message_requested(active_user_, std::move(text));
}

void chat_widget::show_user_details(qint64 user, QString const& username)
{
    if (user <= 0 || username.isEmpty())
    {
        return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("profileDialog"));
    dialog.setWindowTitle(username);
    dialog.setWindowFlag(Qt::FramelessWindowHint);
    dialog.setModal(true);
    dialog.setFixedWidth(590);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QFrame(&dialog);
    header->setObjectName(QStringLiteral("profileHeaderSection"));
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(28, 18, 28, 24);
    header_layout->setSpacing(12);

    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->addStretch();
    auto* close_button = new QToolButton(header);
    close_button->setObjectName(QStringLiteral("profileCloseButton"));
    close_button->setIcon(svg_icon(QStringLiteral("close"), QColor(QStringLiteral("#3F4542")), QSize(22, 22)));
    close_button->setIconSize(QSize(22, 22));
    close_button->setFixedSize(36, 36);
    close_button->setCursor(Qt::PointingHandCursor);
    top->addWidget(close_button);
    header_layout->addLayout(top);

    auto* avatar = new QLabel(avatar_initial(username), header);
    avatar->setObjectName(QStringLiteral("profileDialogAvatar"));
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(104, 104);
    avatar->setStyleSheet(QStringLiteral(
        "background: %1; color: #315A4B; border-radius: 52px; font-size: 34px; font-weight: 700;")
                              .arg(avatar_background(username).name()));
    header_layout->addWidget(avatar, 0, Qt::AlignHCenter);

    auto* name = new QLabel(username, header);
    name->setObjectName(QStringLiteral("profileDialogName"));
    name->setAlignment(Qt::AlignCenter);
    name->setWordWrap(true);
    header_layout->addWidget(name);
    header_layout->addSpacing(4);

    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(20, 0, 20, 0);
    actions->setSpacing(14);
    actions->addStretch();
    auto make_action = [header](QString text, QStringView icon) {
        auto* button = new QToolButton(header);
        button->setObjectName(QStringLiteral("profileActionButton"));
        button->setText(std::move(text));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIcon(svg_icon(icon, QColor(QStringLiteral("#315A4B")), QSize(24, 24)));
        button->setIconSize(QSize(24, 24));
        button->setFixedSize(132, 78);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };
    auto* message_button = make_action(QStringLiteral("消息"), QStringLiteral("chat"));
    auto* copy_username_button = make_action(QStringLiteral("复制用户名"), QStringLiteral("copy"));
    actions->addWidget(message_button);
    actions->addWidget(copy_username_button);
    actions->addStretch();
    header_layout->addLayout(actions);
    layout->addWidget(header);

    auto* section_separator = new QFrame(&dialog);
    section_separator->setObjectName(QStringLiteral("profileSectionSeparator"));
    section_separator->setFixedHeight(10);
    layout->addWidget(section_separator);

    auto* info = new QFrame(&dialog);
    info->setObjectName(QStringLiteral("profileInfoSection"));
    auto* info_layout = new QVBoxLayout(info);
    info_layout->setContentsMargins(34, 20, 34, 22);
    info_layout->setSpacing(5);

    auto* username_value = new QLabel(username, info);
    username_value->setObjectName(QStringLiteral("profileInfoValue"));
    username_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_layout->addWidget(username_value);
    auto* username_label = new QLabel(QStringLiteral("用户名"), info);
    username_label->setObjectName(QStringLiteral("profileInfoLabel"));
    info_layout->addWidget(username_label);
    layout->addWidget(info);

    connect(close_button, &QToolButton::clicked, &dialog, &QDialog::reject);
    connect(message_button, &QToolButton::clicked, &dialog, [this, &dialog, user, username] {
        dialog.accept();
        open_chat(user, username);
    });
    connect(copy_username_button, &QToolButton::clicked, &dialog, [username] {
        QGuiApplication::clipboard()->setText(username);
    });
    dialog.exec();
}

void chat_widget::set_message_status(QString message)
{
    message_status_->setText(std::move(message));
    message_status_->setVisible(!message_status_->text().isEmpty());
}

void chat_widget::update_chat_header(QString const& username)
{
    chat_title_->setText(username);
    chat_title_->setIcon(avatar_icon(username, 38));
    chat_title_->setEnabled(true);
}
