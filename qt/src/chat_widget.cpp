#include "chat_widget.hpp"

#include <utility>

#include <QAbstractItemView>
#include <QFrame>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QModelIndex>
#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "conversation_delegate.hpp"
#include "conversation_model.hpp"
#include "message_delegate.hpp"
#include "message_model.hpp"

namespace
{

QPushButton* make_navigation_button(QString text, QWidget* parent, bool selected, bool enabled)
{
    auto* button = new QPushButton(std::move(text), parent);
    button->setObjectName(selected ? QStringLiteral("navigationSelected") : QStringLiteral("navigationButton"));
    button->setEnabled(enabled);
    button->setFlat(true);
    button->setFixedHeight(54);
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
    navigation_panel->setFixedWidth(88);
    auto* navigation_layout = new QVBoxLayout(navigation_panel);
    navigation_layout->setContentsMargins(12, 20, 12, 16);
    navigation_layout->setSpacing(7);

    profile_avatar_ = new QLabel(QStringLiteral("?"), navigation_panel);
    profile_avatar_->setObjectName(QStringLiteral("profileAvatar"));
    profile_avatar_->setAlignment(Qt::AlignCenter);
    profile_avatar_->setFixedSize(44, 44);
    navigation_layout->addWidget(profile_avatar_, 0, Qt::AlignHCenter);
    navigation_layout->addSpacing(16);
    navigation_layout->addWidget(make_navigation_button(QStringLiteral("聊天"), navigation_panel, true, true));
    navigation_layout->addWidget(make_navigation_button(QStringLiteral("联系人"), navigation_panel, false, false));
    navigation_layout->addWidget(make_navigation_button(QStringLiteral("群组"), navigation_panel, false, false));
    navigation_layout->addWidget(make_navigation_button(QStringLiteral("动态"), navigation_panel, false, false));
    navigation_layout->addWidget(make_navigation_button(QStringLiteral("收藏"), navigation_panel, false, false));
    navigation_layout->addStretch();
    profile_name_ = new QLabel(navigation_panel);
    profile_name_->setObjectName(QStringLiteral("profileName"));
    profile_name_->setAlignment(Qt::AlignCenter);
    profile_name_->setWordWrap(true);
    navigation_layout->addWidget(profile_name_);

    auto* conversation_panel = new QFrame(this);
    conversation_panel->setObjectName(QStringLiteral("conversationPanel"));
    conversation_panel->setFixedWidth(320);
    auto* conversation_layout = new QVBoxLayout(conversation_panel);
    conversation_layout->setContentsMargins(20, 22, 12, 12);
    conversation_layout->setSpacing(10);
    auto* conversations_title = new QLabel(QStringLiteral("消息"), conversation_panel);
    conversations_title->setObjectName(QStringLiteral("sectionTitle"));
    conversation_layout->addWidget(conversations_title);
    conversations_status_ = new QLabel(conversation_panel);
    conversations_status_->setObjectName(QStringLiteral("subtleText"));
    conversation_layout->addWidget(conversations_status_);

    conversations_ = new conversation_model(this);
    conversations_view_ = new QListView(conversation_panel);
    conversations_view_->setObjectName(QStringLiteral("conversationList"));
    conversations_view_->setModel(conversations_);
    conversations_view_->setItemDelegate(new conversation_delegate(conversations_view_));
    conversations_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    conversations_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    conversations_view_->setMouseTracking(true);
    conversations_view_->verticalScrollBar()->setSingleStep(24);
    conversation_layout->addWidget(conversations_view_, 1);

    auto* chat_panel = new QFrame(this);
    chat_panel->setObjectName(QStringLiteral("chatPanel"));
    chat_panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* chat_layout = new QVBoxLayout(chat_panel);
    chat_layout->setContentsMargins(26, 22, 26, 24);
    chat_layout->setSpacing(10);
    chat_title_ = new QLabel(QStringLiteral("聊天"), chat_panel);
    chat_title_->setObjectName(QStringLiteral("sectionTitle"));
    chat_layout->addWidget(chat_title_);
    auto* chat_line = new QFrame(chat_panel);
    chat_line->setFrameShape(QFrame::HLine);
    chat_line->setObjectName(QStringLiteral("horizontalSeparator"));
    chat_layout->addWidget(chat_line);

    message_status_ = new QLabel(QStringLiteral("选择一个会话开始聊天"), chat_panel);
    message_status_->setObjectName(QStringLiteral("subtleText"));
    message_status_->setAlignment(Qt::AlignCenter);
    chat_layout->addWidget(message_status_);

    messages_ = new message_model(this);
    messages_view_ = new QListView(chat_panel);
    messages_view_->setObjectName(QStringLiteral("messageList"));
    messages_view_->setModel(messages_);
    messages_view_->setItemDelegate(new message_delegate(messages_view_));
    messages_view_->setSelectionMode(QAbstractItemView::NoSelection);
    messages_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    messages_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messages_view_->setSpacing(1);
    messages_view_->verticalScrollBar()->setSingleStep(24);
    chat_layout->addWidget(messages_view_, 1);

    auto* input_layout = new QHBoxLayout;
    input_layout->setSpacing(10);
    message_edit_ = new QLineEdit(chat_panel);
    message_edit_->setObjectName(QStringLiteral("messageEdit"));
    message_edit_->setPlaceholderText(QStringLiteral("输入消息…"));
    message_edit_->setEnabled(false);
    input_layout->addWidget(message_edit_, 1);
    send_button_ = new QPushButton(QStringLiteral("发送"), chat_panel);
    send_button_->setObjectName(QStringLiteral("sendButton"));
    send_button_->setEnabled(false);
    input_layout->addWidget(send_button_);
    chat_layout->addLayout(input_layout);

    auto* detail_panel = new QFrame(this);
    detail_panel->setObjectName(QStringLiteral("detailPanel"));
    detail_panel->setFixedWidth(286);
    auto* detail_layout = new QVBoxLayout(detail_panel);
    detail_layout->setContentsMargins(20, 22, 20, 22);
    detail_layout->setSpacing(16);
    auto* detail_title = new QLabel(QStringLiteral("详情"), detail_panel);
    detail_title->setObjectName(QStringLiteral("sectionTitle"));
    detail_layout->addWidget(detail_title);

    detail_avatar_ = new QLabel(QStringLiteral("?"), detail_panel);
    detail_avatar_->setObjectName(QStringLiteral("detailAvatar"));
    detail_avatar_->setAlignment(Qt::AlignCenter);
    detail_avatar_->setFixedSize(76, 76);
    detail_layout->addWidget(detail_avatar_, 0, Qt::AlignHCenter);

    detail_name_ = new QLabel(QStringLiteral("选择会话"), detail_panel);
    detail_name_->setObjectName(QStringLiteral("detailName"));
    detail_name_->setAlignment(Qt::AlignCenter);
    detail_name_->setWordWrap(true);
    detail_layout->addWidget(detail_name_);

    detail_id_ = new QLabel(QStringLiteral("选择会话后显示对端信息"), detail_panel);
    detail_id_->setObjectName(QStringLiteral("subtleText"));
    detail_id_->setAlignment(Qt::AlignCenter);
    detail_layout->addWidget(detail_id_);

    auto* detail_card = new QFrame(detail_panel);
    detail_card->setObjectName(QStringLiteral("detailCard"));
    auto* detail_card_layout = new QGridLayout(detail_card);
    detail_card_layout->setContentsMargins(16, 14, 16, 14);
    detail_card_layout->setHorizontalSpacing(12);
    detail_card_layout->setVerticalSpacing(12);

    auto* username_label = new QLabel(QStringLiteral("用户名"), detail_card);
    username_label->setObjectName(QStringLiteral("detailFieldLabel"));
    detail_username_ = new QLabel(QStringLiteral("—"), detail_card);
    detail_username_->setObjectName(QStringLiteral("detailFieldValue"));
    detail_username_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* id_label = new QLabel(QStringLiteral("用户 ID"), detail_card);
    id_label->setObjectName(QStringLiteral("detailFieldLabel"));
    detail_user_id_ = new QLabel(QStringLiteral("—"), detail_card);
    detail_user_id_->setObjectName(QStringLiteral("detailFieldValue"));
    detail_user_id_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    detail_card_layout->addWidget(username_label, 0, 0);
    detail_card_layout->addWidget(detail_username_, 0, 1);
    detail_card_layout->addWidget(id_label, 1, 0);
    detail_card_layout->addWidget(detail_user_id_, 1, 1);
    detail_card_layout->setColumnStretch(1, 1);
    detail_layout->addWidget(detail_card);
    detail_layout->addStretch();

    layout->addWidget(navigation_panel);
    layout->addWidget(conversation_panel);
    layout->addWidget(make_separator(this));
    layout->addWidget(chat_panel, 1);
    layout->addWidget(make_separator(this));
    layout->addWidget(detail_panel);

    connect(conversations_view_, &QListView::clicked, this, [this](QModelIndex const& index) { select_conversation(index); });
    connect(send_button_, &QPushButton::clicked, this, [this] { send_current_message(); });
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
    profile_avatar_->setText(username.isEmpty() ? QStringLiteral("?") : username.left(1).toUpper());
    profile_name_->setText(username);
    active_user_ = 0;
    messages_->reset(0);
    chat_title_->setText(QStringLiteral("聊天"));
    message_status_->setText(QStringLiteral("选择一个会话开始聊天"));
    detail_avatar_->setText(QStringLiteral("?"));
    detail_name_->setText(QStringLiteral("选择会话"));
    detail_id_->setText(QStringLiteral("选择会话后显示对端信息"));
    detail_username_->setText(QStringLiteral("—"));
    detail_user_id_->setText(QStringLiteral("—"));
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
        message_status_->setText(messages_->rowCount() == 0 ? QStringLiteral("暂无消息") : QString{});
        QTimer::singleShot(0, messages_view_, [view = messages_view_] { view->scrollToBottom(); });
        return;
    }

    message_status_->clear();
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
        message_status_->clear();
        QTimer::singleShot(0, messages_view_, [view = messages_view_] { view->scrollToBottom(); });
    }
}

void chat_widget::add_sent_message(qint64 user, qint64 message, QString text, bool realtime)
{
    if (user != active_user_)
    {
        return;
    }

    message_data value;
    value.id = message;
    value.from = 0;
    value.text = std::move(text);
    add_message(user, std::move(value));
    if (!realtime)
    {
        message_status_->setText(QStringLiteral("消息已保存，对方当前未实时接收"));
    }
}

void chat_widget::set_message_error(qint64 user, QString message)
{
    if (user == active_user_)
    {
        messages_loading_ = false;
        message_status_->setText(std::move(message));
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
            message_status_->setText(QStringLiteral("正在加载消息…"));
            emit conversation_selected(active_user_);
        }
        return;
    }

    active_user_ = item->user;
    messages_->reset(active_user_);
    messages_loaded_ = false;
    messages_loading_ = true;
    history_exhausted_ = false;
    message_status_->setText(QStringLiteral("正在加载消息…"));
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
    message_status_->clear();
    emit send_message_requested(active_user_, std::move(text));
}

void chat_widget::update_conversation_details(conversation_data const& item)
{
    chat_title_->setText(item.username);
    detail_avatar_->setText(item.username.isEmpty() ? QStringLiteral("?") : item.username.left(1).toUpper());
    detail_name_->setText(item.username);
    detail_id_->setText(QStringLiteral("用户 ID %1").arg(item.user));
    detail_username_->setText(item.username);
    detail_user_id_->setText(QString::number(item.user));
}
