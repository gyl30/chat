#include "chat_widget.hpp"

#include <utility>

#include <QAbstractItemView>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QModelIndex>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "conversation_delegate.hpp"
#include "conversation_model.hpp"

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
    conversation_layout->addWidget(conversations_view_, 1);

    auto* chat_panel = new QFrame(this);
    chat_panel->setObjectName(QStringLiteral("chatPanel"));
    chat_panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* chat_layout = new QVBoxLayout(chat_panel);
    chat_layout->setContentsMargins(26, 22, 26, 24);
    chat_layout->setSpacing(0);
    chat_title_ = new QLabel(QStringLiteral("聊天"), chat_panel);
    chat_title_->setObjectName(QStringLiteral("sectionTitle"));
    chat_layout->addWidget(chat_title_);
    chat_layout->addSpacing(12);
    auto* chat_line = new QFrame(chat_panel);
    chat_line->setFrameShape(QFrame::HLine);
    chat_line->setStyleSheet(QStringLiteral("background:#E6E3DB; border:0; max-height:1px;"));
    chat_layout->addWidget(chat_line);
    chat_placeholder_ = new QLabel(QStringLiteral("选择一个会话开始聊天"), chat_panel);
    chat_placeholder_->setObjectName(QStringLiteral("subtleText"));
    chat_placeholder_->setAlignment(Qt::AlignCenter);
    chat_layout->addWidget(chat_placeholder_, 1);

    auto* detail_panel = new QFrame(this);
    detail_panel->setObjectName(QStringLiteral("detailPanel"));
    detail_panel->setFixedWidth(240);
    auto* detail_layout = new QVBoxLayout(detail_panel);
    detail_layout->setContentsMargins(22, 22, 22, 22);
    detail_layout->setSpacing(12);
    auto* detail_title = new QLabel(QStringLiteral("详情"), detail_panel);
    detail_title->setObjectName(QStringLiteral("sectionTitle"));
    detail_layout->addWidget(detail_title);
    detail_name_ = new QLabel(QStringLiteral("选择会话后显示对端信息"), detail_panel);
    detail_name_->setObjectName(QStringLiteral("subtleText"));
    detail_name_->setWordWrap(true);
    detail_layout->addWidget(detail_name_);
    detail_layout->addStretch();

    layout->addWidget(navigation_panel);
    layout->addWidget(conversation_panel);
    layout->addWidget(make_separator(this));
    layout->addWidget(chat_panel, 1);
    layout->addWidget(make_separator(this));
    layout->addWidget(detail_panel);

    connect(conversations_view_, &QListView::clicked, this, [this](QModelIndex const& index) { select_conversation(index); });
}

void chat_widget::set_user(QString const& username)
{
    profile_avatar_->setText(username.isEmpty() ? QStringLiteral("?") : username.left(1).toUpper());
    profile_name_->setText(username);
    chat_title_->setText(QStringLiteral("聊天"));
    chat_placeholder_->setText(QStringLiteral("选择一个会话开始聊天"));
    detail_name_->setText(QStringLiteral("选择会话后显示对端信息"));
}

void chat_widget::set_loading() { conversations_status_->setText(QStringLiteral("正在加载…")); }

void chat_widget::set_error(QString message) { conversations_status_->setText(std::move(message)); }

void chat_widget::set_conversations(QList<conversation_data> conversations)
{
    conversations_->set_conversations(std::move(conversations));
    if (conversations_->rowCount() == 0)
    {
        conversations_status_->setText(QStringLiteral("暂无会话"));
        return;
    }

    conversations_status_->clear();
    auto first = conversations_->index(0, 0);
    conversations_view_->setCurrentIndex(first);
    select_conversation(first);
}

void chat_widget::select_conversation(QModelIndex const& index)
{
    auto const* item = conversations_->conversation_at(index);
    if (!item)
    {
        return;
    }

    chat_title_->setText(item->username);
    chat_placeholder_->setText(QStringLiteral("消息将在下一阶段接入"));
    detail_name_->setText(QStringLiteral("%1\n用户 ID：%2").arg(item->username).arg(item->user));
}
