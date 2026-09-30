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
#include <QModelIndex>
#include <QPushButton>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
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
    navigation_layout->addWidget(
        make_navigation_button(QStringLiteral("聊天"), QStringLiteral("chat"), navigation_panel, true, true),
        0, Qt::AlignHCenter);
    navigation_layout->addWidget(
        make_navigation_button(QStringLiteral("联系人"), QStringLiteral("contacts"), navigation_panel, false, false),
        0, Qt::AlignHCenter);
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
    auto* conversation_header_layout = new QVBoxLayout(conversation_header);
    conversation_header_layout->setContentsMargins(18, 20, 14, 10);
    conversation_header_layout->setSpacing(5);
    auto* conversations_title = new QLabel(QStringLiteral("消息"), conversation_header);
    conversations_title->setObjectName(QStringLiteral("sectionTitle"));
    conversation_header_layout->addWidget(conversations_title);
    conversations_status_ = new QLabel(conversation_header);
    conversations_status_->setObjectName(QStringLiteral("subtleText"));
    conversation_header_layout->addWidget(conversations_status_);
    conversation_layout->addWidget(conversation_header);

    conversations_ = new conversation_model(this);
    conversations_view_ = new QListView(conversation_panel);
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
    conversation_layout->addWidget(conversations_view_, 1);

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

    connect(conversations_view_, &QListView::clicked, this, [this](QModelIndex const& index) { select_conversation(index); });
    connect(conversations_delegate, &conversation_delegate::avatar_clicked, this, [this](QModelIndex const& index) {
        if (auto const* item = conversations_->conversation_at(index))
        {
            show_conversation_details(*item);
        }
    });
    connect(messages_delegate, &message_delegate::avatar_clicked, this, [this](QModelIndex const&) {
        show_conversation_details();
    });
    connect(chat_title_, &QPushButton::clicked, this, [this] { show_conversation_details(); });
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
    messages_->set_self_username(username);
    messages_->reset(0);
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

void chat_widget::set_conversations(QList<conversation_data> conversations)
{
    auto const previous_user = active_user_;
    conversations_->set_conversations(std::move(conversations));
    if (conversations_->rowCount() == 0)
    {
        conversations_status_->setText(QStringLiteral("暂无会话"));
        return;
    }

    conversations_status_->clear();
    auto index = previous_user > 0 ? conversations_->index_for_user(previous_user) : QModelIndex{};
    if (!index.isValid())
    {
        index = conversations_->index(0, 0);
        conversations_view_->setCurrentIndex(index);
        select_conversation(index);
        return;
    }

    conversations_view_->setCurrentIndex(index);
    auto const* item = conversations_->conversation_at(index);
    if (item)
    {
        update_conversation_details(*item);
    }
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

void chat_widget::add_sent_message(qint64 user, qint64 message, qint64 timestamp, QString text, bool realtime)
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
    if (!realtime)
    {
        set_message_status(QStringLiteral("消息已保存，对方当前未实时接收"));
    }
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

void chat_widget::select_conversation(QModelIndex const& index)
{
    auto const* item = conversations_->conversation_at(index);
    if (!item)
    {
        return;
    }

    update_conversation_details(*item);
    if (active_user_ == item->user)
    {
        if (!messages_loaded_ && !messages_loading_)
        {
            messages_loading_ = true;
            set_message_status(QStringLiteral("正在加载消息…"));
            emit conversation_selected(active_user_);
        }
        return;
    }

    active_user_ = item->user;
    messages_->reset(active_user_, item->username);
    messages_loaded_ = false;
    messages_loading_ = true;
    history_exhausted_ = false;
    set_message_status(QStringLiteral("正在加载消息…"));
    message_edit_->setEnabled(true);
    send_button_->setEnabled(true);
    emit conversation_selected(active_user_);
}

void chat_widget::request_older_messages()
{
    if (active_user_ <= 0 || !messages_loaded_ || messages_loading_ || history_exhausted_)
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
    if (active_user_ <= 0 || message_edit_->text().isEmpty())
    {
        return;
    }

    auto text = message_edit_->text();
    message_edit_->clear();
    set_message_status({});
    emit send_message_requested(active_user_, std::move(text));
}

void chat_widget::show_conversation_details()
{
    auto const index = conversations_->index_for_user(active_user_);
    auto const* item = conversations_->conversation_at(index);
    if (item)
    {
        show_conversation_details(*item);
    }
}

void chat_widget::show_conversation_details(conversation_data const& item)
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("profileDialog"));
    dialog.setWindowTitle(item.username);
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

    auto* avatar = new QLabel(avatar_initial(item.username), header);
    avatar->setObjectName(QStringLiteral("profileDialogAvatar"));
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(104, 104);
    avatar->setStyleSheet(QStringLiteral(
        "background: %1; color: #315A4B; border-radius: 52px; font-size: 34px; font-weight: 700;")
                              .arg(avatar_background(item.username).name()));
    header_layout->addWidget(avatar, 0, Qt::AlignHCenter);

    auto* name = new QLabel(item.username, header);
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

    auto* username_value = new QLabel(item.username, info);
    username_value->setObjectName(QStringLiteral("profileInfoValue"));
    username_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_layout->addWidget(username_value);
    auto* username_label = new QLabel(QStringLiteral("用户名"), info);
    username_label->setObjectName(QStringLiteral("profileInfoLabel"));
    info_layout->addWidget(username_label);
    layout->addWidget(info);

    connect(close_button, &QToolButton::clicked, &dialog, &QDialog::reject);
    connect(message_button, &QToolButton::clicked, &dialog, &QDialog::accept);
    connect(copy_username_button, &QToolButton::clicked, &dialog, [username = item.username] {
        QGuiApplication::clipboard()->setText(username);
    });
    dialog.exec();
}

void chat_widget::set_message_status(QString message)
{
    message_status_->setText(std::move(message));
    message_status_->setVisible(!message_status_->text().isEmpty());
}

void chat_widget::update_conversation_details(conversation_data const& item)
{
    chat_title_->setText(item.username);
    chat_title_->setIcon(avatar_icon(item.username, 38));
    chat_title_->setEnabled(true);
}
