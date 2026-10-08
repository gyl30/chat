#include "chat_widget.hpp"
#include <chat/text.hpp>

#include <utility>
#include <iterator>

#include <QAbstractItemView>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QFrame>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMenu>
#include <QInputDialog>
#include <QInputMethod>
#include <QModelIndex>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOptionButton>
#include <QTimer>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QtMath>
#include <QToolButton>
#include <QVBoxLayout>
#include <chat/attachment.hpp>
#include <chat/reaction.hpp>

#include "avatar.hpp"
#include "conversation_delegate.hpp"
#include "conversation_model.hpp"
#include "emoji_text.hpp"
#include "icons.hpp"
#include "message_delegate.hpp"
#include "message_model.hpp"
#include "presence.hpp"
#include "theme.hpp"
#include "user_delegate.hpp"
#include "user_model.hpp"

namespace
{

class pinned_message_button final : public QPushButton
{
public:
    using QPushButton::QPushButton;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_PushButtonBevel, option);
        auto contents = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
        if (option.state & (QStyle::State_Sunken | QStyle::State_On))
        {
            contents.translate(style()->pixelMetric(QStyle::PM_ButtonShiftHorizontal, &option, this),
                               style()->pixelMetric(QStyle::PM_ButtonShiftVertical, &option, this));
        }
        auto displayed = text();
        displayed.replace(QStringLiteral("&&"), QStringLiteral("&"));
        paint_emoji_line(painter, contents, displayed, font(),
                         option.palette.color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                              QPalette::ButtonText));
        if (option.state & QStyle::State_HasFocus)
        {
            QStyleOptionFocusRect focus;
            focus.QStyleOption::operator=(option);
            focus.rect = style()->subElementRect(QStyle::SE_PushButtonFocusRect, &option, this);
            focus.backgroundColor = option.palette.color(QPalette::Button);
            painter.drawPrimitive(QStyle::PE_FrameFocusRect, focus);
        }
    }
};

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

chat_widget::chat_widget(QWidget* parent) : QWidget(parent), avatars_(this)
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
    navigation_layout->addStretch();

    profile_avatar_ = new QToolButton(navigation_panel);
    profile_avatar_->setCursor(Qt::PointingHandCursor);
    profile_avatar_->setIconSize(QSize(44, 44));
    profile_avatar_->setObjectName(QStringLiteral("profileAvatar"));
    profile_avatar_->setAccessibleName(QStringLiteral("我的资料"));
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
    add_contact_button_->setText(QStringLiteral("添加好友"));
    add_contact_button_->setCursor(Qt::PointingHandCursor);
    add_contact_button_->hide();
    conversation_header_layout->addWidget(add_contact_button_);
    chats_actions_ = new QToolButton(conversation_header);
    chats_actions_->setObjectName(QStringLiteral("chatsActionsButton"));
    chats_actions_->setText(QStringLiteral("+"));
    chats_actions_->setAccessibleName(QStringLiteral("聊天操作"));
    chats_actions_->setToolTip(QStringLiteral("添加好友、发起群聊或加入群聊"));
    chats_actions_->setFixedSize(32, 32);
    chats_actions_->setCursor(Qt::PointingHandCursor);
    chats_actions_->setPopupMode(QToolButton::InstantPopup);
    auto* actions = new QMenu(chats_actions_);
    actions->setObjectName(QStringLiteral("chatsActionsMenu"));
    auto* add_friend = actions->addAction(QStringLiteral("添加好友"));
    add_friend->setObjectName(QStringLiteral("addFriendAction"));
    auto* create_group = actions->addAction(QStringLiteral("发起群聊"));
    create_group->setObjectName(QStringLiteral("createGroupAction"));
    auto* join_group = actions->addAction(QStringLiteral("加入群聊"));
    join_group->setObjectName(QStringLiteral("joinGroupAction"));
    chats_actions_->setMenu(actions);
    conversation_header_layout->addWidget(chats_actions_);
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

    conversations_ = new conversation_model(this, &avatars_);
    conversations_view_ = new QListView(conversations_page);
    conversations_view_->setObjectName(QStringLiteral("conversationList"));
    conversations_view_->setAccessibleName(QStringLiteral("会话列表"));
    conversations_view_->setModel(conversations_);
    auto* conversations_delegate = new conversation_delegate(conversations_view_);
    conversations_view_->setItemDelegate(conversations_delegate);
    conversations_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    conversations_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    conversations_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    conversations_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    conversations_view_->setMouseTracking(true);
    conversations_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    conversations_view_->verticalScrollBar()->setSingleStep(24);
    conversations_layout->addWidget(conversations_view_, 1);
    sidebar_pages_->addWidget(conversations_page);

    auto* contacts_page = new QWidget(sidebar_pages_);
    auto* contacts_layout = new QVBoxLayout(contacts_page);
    contacts_layout->setContentsMargins(0, 0, 0, 0);
    contacts_layout->setSpacing(0);
    new_friends_button_ = new QPushButton(QStringLiteral("新的朋友"), contacts_page);
    new_friends_button_->setObjectName(QStringLiteral("newFriendsButton"));
    contacts_layout->addWidget(new_friends_button_);
    connect(new_friends_button_, &QPushButton::clicked, this, [this] { show_new_friends_section(); });
    contact_search_ = new QLineEdit(contacts_page);
    contact_search_->setObjectName(QStringLiteral("userSearchEdit"));
    contact_search_->setPlaceholderText(QStringLiteral("搜索联系人"));
    contact_search_->setAccessibleName(QStringLiteral("搜索联系人"));
    contact_search_->setClearButtonEnabled(false);
    contacts_layout->addWidget(contact_search_);
    contacts_status_ = new QLabel(QStringLiteral("暂无联系人"), contacts_page);
    contacts_status_->setObjectName(QStringLiteral("subtleText"));
    contacts_status_->setContentsMargins(18, 8, 14, 8);
    contacts_layout->addWidget(contacts_status_);

    contacts_ = new user_model(this, &avatars_);
    contacts_filter_ = new QSortFilterProxyModel(this);
    contacts_filter_->setSourceModel(contacts_);
    contacts_filter_->setFilterRole(user_model::username_role);
    contacts_filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    contacts_view_ = new QListView(contacts_page);
    contacts_view_->setObjectName(QStringLiteral("userList"));
    contacts_view_->setAccessibleName(QStringLiteral("联系人列表"));
    contacts_view_->setModel(contacts_filter_);
    auto* contacts_delegate = new user_delegate(contacts_view_);
    contacts_view_->setItemDelegate(contacts_delegate);
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
    add_user_search_->setAccessibleName(QStringLiteral("搜索用户"));
    add_user_search_->setClearButtonEnabled(false);
    add_contacts_layout->addWidget(add_user_search_);
    add_users_status_ = new QLabel(QStringLiteral("输入用户名搜索"), add_contacts_page);
    add_users_status_->setObjectName(QStringLiteral("subtleText"));
    add_users_status_->setContentsMargins(18, 8, 14, 8);
    add_contacts_layout->addWidget(add_users_status_);

    add_users_ = new user_model(this, &avatars_);
    add_users_view_ = new QListView(add_contacts_page);
    add_users_view_->setObjectName(QStringLiteral("userList"));
    add_users_view_->setAccessibleName(QStringLiteral("用户搜索结果"));
    add_users_view_->setModel(add_users_);
    auto* add_users_delegate = new user_delegate(add_users_view_);
    add_users_view_->setItemDelegate(add_users_delegate);
    add_users_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    add_users_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    add_users_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    add_users_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    add_users_view_->setMouseTracking(true);
    add_users_view_->verticalScrollBar()->setSingleStep(24);
    add_contacts_layout->addWidget(add_users_view_, 1);
    sidebar_pages_->addWidget(add_contacts_page);

    auto* requests_page = new QWidget(sidebar_pages_);
    auto* requests_layout = new QVBoxLayout(requests_page);
    requests_layout->setContentsMargins(0, 0, 0, 0);
    requests_layout->setSpacing(0);
    friend_requests_status_ = new QLabel(requests_page);
    friend_requests_status_->setObjectName(QStringLiteral("friendRequestsStatus"));
    friend_requests_status_->setWordWrap(true);
    friend_requests_status_->setContentsMargins(18, 16, 14, 8);
    requests_layout->addWidget(friend_requests_status_);
    incoming_friends_title_ = new QLabel(requests_page);
    incoming_friends_title_->setObjectName(QStringLiteral("friendRequestHeading"));
    incoming_friends_title_->setContentsMargins(18, 16, 14, 8);
    requests_layout->addWidget(incoming_friends_title_);
    incoming_friends_ = new QListWidget(requests_page);
    incoming_friends_->setObjectName(QStringLiteral("incomingFriendRequests"));
    incoming_friends_->setAccessibleName(QStringLiteral("收到的好友申请"));
    requests_layout->addWidget(incoming_friends_, 1);
    outgoing_friends_title_ = new QLabel(requests_page);
    outgoing_friends_title_->setObjectName(QStringLiteral("friendRequestHeading"));
    outgoing_friends_title_->setContentsMargins(18, 16, 14, 8);
    requests_layout->addWidget(outgoing_friends_title_);
    outgoing_friends_ = new QListWidget(requests_page);
    outgoing_friends_->setObjectName(QStringLiteral("outgoingFriendRequests"));
    outgoing_friends_->setAccessibleName(QStringLiteral("发出的好友申请"));
    requests_layout->addWidget(outgoing_friends_, 1);
    requests_layout->addStretch();
    sidebar_pages_->addWidget(requests_page);
    auto open_request = [this](QListWidgetItem* item) {
        show_user_details(item->data(Qt::UserRole).toLongLong(), item->data(Qt::UserRole + 1).toString());
    };
    connect(incoming_friends_, &QListWidget::itemClicked, this, open_request);
    connect(outgoing_friends_, &QListWidget::itemClicked, this, open_request);
    for (auto* list : {incoming_friends_, outgoing_friends_})
    {
        auto* delegate = new user_delegate(list);
        list->setItemDelegate(delegate);
        list->setUniformItemSizes(true);
        list->setFrameShape(QFrame::NoFrame);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setMouseTracking(true);
        list->installEventFilter(this);
        list->viewport()->installEventFilter(this);
    }
    notice_ = new feedback_label(conversation_panel);
    notice_->setObjectName(QStringLiteral("sidebarNotice"));
    notice_->setWordWrap(true);
    notice_->hide();
    conversation_layout->addWidget(notice_);
    notice_timer_ = new QTimer(this);
    notice_timer_->setSingleShot(true);
    notice_timer_->setInterval(6000);
    connect(notice_timer_, &QTimer::timeout, this, [this] { notice_->clear(); notice_->hide(); });
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
    header_layout->setContentsMargins(16, 5, 12, 5);
    header_layout->setSpacing(8);

    auto* chat_identity = new QWidget(header);
    auto* chat_identity_layout = new QVBoxLayout(chat_identity);
    chat_identity_layout->setContentsMargins(0, 0, 0, 0);
    chat_identity_layout->setSpacing(0);
    chat_title_ = new QPushButton(QStringLiteral("聊天"), chat_identity);
    chat_title_->setObjectName(QStringLiteral("chatHeaderButton"));
    chat_title_->setFlat(true);
    chat_title_->setEnabled(false);
    chat_title_->setCursor(Qt::PointingHandCursor);
    chat_title_->setIconSize(QSize(38, 38));
    chat_identity_layout->addWidget(chat_title_);
    chat_presence_ = new QLabel(chat_identity);
    chat_presence_->setObjectName(QStringLiteral("chatPresence"));
    chat_presence_->setContentsMargins(50, 0, 0, 0);
    chat_presence_->hide();
    chat_identity_layout->addWidget(chat_presence_);
    typing_label_ = new QLabel(chat_identity);
    typing_label_->setObjectName(QStringLiteral("typingStatusLabel"));
    typing_label_->setTextFormat(Qt::PlainText);
    typing_label_->setContentsMargins(50, 0, 0, 0);
    typing_label_->hide();
    chat_identity_layout->addWidget(typing_label_);
    header_layout->addWidget(chat_identity);
    header_layout->addStretch();
    message_search_button_ = new QToolButton(header);
    message_search_button_->setObjectName(QStringLiteral("messageSearchButton"));
    message_search_button_->setText(QStringLiteral("搜索消息"));
    message_search_button_->setEnabled(false);
    header_layout->addWidget(message_search_button_);
    connect(message_search_button_, &QToolButton::clicked, this, [this] {
        emit message_search_requested(active_conversation_, self_user_, active_group_, active_username_, {});
    });
    connection_status_ = new QToolButton(header);
    connection_status_->setObjectName(QStringLiteral("connectionStatusButton"));
    connection_status_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connection_status_->setCursor(Qt::PointingHandCursor);
    connection_status_->hide();
    header_layout->addWidget(connection_status_);
    chat_layout->addWidget(header);

    auto* pinned_row = new QWidget(chat_panel);
    auto* pinned_layout = new QHBoxLayout(pinned_row);
    pinned_layout->setContentsMargins(16, 0, 12, 0);
    pinned_message_button_ = new pinned_message_button(pinned_row);
    pinned_message_button_->setObjectName(QStringLiteral("pinnedMessageButton"));
    pinned_message_button_->setFlat(true);
    pinned_message_button_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pinned_message_button_->setCursor(Qt::PointingHandCursor);
    unpin_message_button_ = new QToolButton(pinned_row);
    unpin_message_button_->setObjectName(QStringLiteral("unpinMessageButton"));
    unpin_message_button_->setText(QStringLiteral("取消置顶消息"));
    pinned_layout->addWidget(pinned_message_button_, 1);
    pinned_layout->addWidget(unpin_message_button_);
    pinned_message_button_->hide();
    unpin_message_button_->hide();
    chat_layout->addWidget(pinned_row);
    connect(pinned_message_button_, &QPushButton::clicked, this, [this] {
        auto const item = conversation(active_conversation_);
        if (!item || item->pinned_message.id <= 0) { return; }
        for (int row = 0; row < messages_->rowCount(); ++row)
        {
            auto const index = messages_->index(row, 0);
            if (index.data(message_model::id_role).toLongLong() != item->pinned_message.id) { continue; }
            messages_view_->scrollTo(index, QAbstractItemView::PositionAtCenter);
            return;
        }
        QString query;
        for (auto const& line : item->pinned_message.text.split(QLatin1Char('\n')))
        {
            query = grapheme_prefix(line.trimmed(), 80);
            if (!query.isEmpty()) { break; }
        }
        emit message_search_requested(active_conversation_, self_user_, true, active_username_, std::move(query));
    });
    connect(unpin_message_button_, &QToolButton::clicked, this, [this] {
        if (connection_available_ && messages_->can_manage_group())
        {
            emit group_message_pin_requested(active_conversation_, std::nullopt);
        }
    });

    auto* chat_line = new QFrame(chat_panel);
    chat_line->setFrameShape(QFrame::HLine);
    chat_line->setObjectName(QStringLiteral("horizontalSeparator"));
    chat_layout->addWidget(chat_line);

    message_status_ = new QLabel(QStringLiteral("选择一个会话开始聊天"), chat_panel);
    message_status_->setObjectName(QStringLiteral("subtleText"));
    message_status_->setAlignment(Qt::AlignCenter);
    message_status_->setContentsMargins(0, 6, 0, 6);
    chat_layout->addWidget(message_status_);

    messages_ = new message_model(this, &avatars_, &images_);
    messages_view_ = new QListView(chat_panel);
    messages_view_->setObjectName(QStringLiteral("messageList"));
    messages_view_->setAccessibleName(QStringLiteral("消息记录"));
    connect(messages_, &QAbstractItemModel::modelReset, this, [this] {
        QTimer::singleShot(0, messages_view_, [this] { load_visible_images(); });
    });
    connect(messages_, &QAbstractItemModel::rowsInserted, this, [this] {
        QTimer::singleShot(0, messages_view_, [this] { load_visible_images(); });
    });
    messages_view_->viewport()->installEventFilter(this);
    messages_view_->setModel(messages_);
    auto* messages_delegate = new message_delegate(messages_view_);
    messages_view_->setItemDelegate(messages_delegate);
    messages_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    messages_view_->setSelectionMode(QAbstractItemView::NoSelection);
    messages_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    messages_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    messages_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messages_view_->setSpacing(0);
    messages_view_->verticalScrollBar()->setSingleStep(24);
    chat_layout->addWidget(messages_view_, 1);

    reply_bar_ = new QWidget(chat_panel);
    auto* reply_layout = new QHBoxLayout(reply_bar_);
    reply_preview_ = new emoji_label(reply_bar_);
    reply_preview_->setObjectName(QStringLiteral("replyPreview"));
    auto* cancel_reply = new QToolButton(reply_bar_);
    cancel_reply->setObjectName(QStringLiteral("cancelReplyButton"));
    cancel_reply->setAccessibleName(QStringLiteral("取消回复"));
    cancel_reply->setToolTip(QStringLiteral("取消回复"));
    cancel_reply->setIcon(svg_icon(QStringLiteral("close"), QColor(QStringLiteral("#3F4542")), QSize(20, 20)));
    cancel_reply->setIconSize(QSize(20, 20));
    cancel_reply->setFixedSize(36, 36);
    cancel_reply->setCursor(Qt::PointingHandCursor);
    reply_layout->addWidget(reply_preview_, 1);
    reply_layout->addWidget(cancel_reply);
    reply_bar_->hide();
    chat_layout->addWidget(reply_bar_);
    connect(cancel_reply, &QToolButton::clicked, this,
            [this]
            {
                reply_to_ = 0;
                reply_bar_->hide();
                message_edit_->setFocus();
            });
    connect(messages_view_, &QListView::customContextMenuRequested, this,
            [this](QPoint position)
            {
                auto const index = messages_view_->indexAt(position);
                if (!index.isValid() || !connection_available_ || index.data(message_model::deleted_role).toBool())
                {
                    return;
                }
                QMenu menu(this);
                auto* reply = can_send() ? menu.addAction(QStringLiteral("回复")) : nullptr;
                auto const message_id = index.data(message_model::id_role).toLongLong();
                auto const message_text = index.data(message_model::text_role).toString();
                auto const sender_name = index.data(message_model::sender_name_role).toString();
                auto const current = conversation(active_conversation_);
                auto const unpin = current && current->pinned_message.id == message_id;
                auto* group_pin = messages_->can_manage_group()
                    ? menu.addAction(unpin ? QStringLiteral("取消置顶消息") : QStringLiteral("置顶消息")) : nullptr;
                auto* picker = can_send() ? menu.addMenu(QStringLiteral("表情回应")) : nullptr;
                auto const own_reaction = index.data(message_model::own_reaction_role).toString();
                auto const reaction_index = QPersistentModelIndex(index);
                auto const conversation = active_conversation_;
                for (auto emoji : chat::reaction_choices)
                {
                    if (!picker) { break; }
                    auto const text = QString::fromUtf8(emoji.data(), static_cast<qsizetype>(emoji.size()));
                    auto* action = picker->addAction(text);
                    action->setCheckable(true);
                    action->setChecked(text == own_reaction);
                    connect(action, &QAction::triggered, &menu, [this, reaction_index, text, conversation] {
                        if (can_send() && conversation == active_conversation_ && reaction_index.isValid() &&
                            !reaction_index.data(message_model::deleted_role).toBool())
                        {
                            emit reaction_requested(conversation, reaction_index.data(message_model::id_role).toLongLong(),
                                text == reaction_index.data(message_model::own_reaction_role).toString() ? QString{} : text);
                        }
                    });
                }
                auto* readers = index.data(message_model::read_count_role).isValid()
                    ? menu.addAction(QStringLiteral("已读详情")) : nullptr;
                auto const filename = index.data(message_model::attachment_name_role).toString();
                auto* download = !filename.isEmpty() ? menu.addAction(QStringLiteral("下载文件")) : nullptr;
                auto* preview = index.data(message_model::attachment_type_role).toString().startsWith(QStringLiteral("image/"))
                    ? menu.addAction(QStringLiteral("查看图片")) : nullptr;
                auto* edit = can_send() && filename.isEmpty() && index.data(message_model::outgoing_role).toBool() ? menu.addAction(QStringLiteral("编辑"))
                                                                               : nullptr;
                auto* remove = index.data(message_model::outgoing_role).toBool()
                                   ? menu.addAction(QStringLiteral("删除"))
                                   : nullptr;
                auto* selected = menu.exec(messages_view_->viewport()->mapToGlobal(position));
                if (!connection_available_ || conversation != active_conversation_) { return; }
                if (group_pin && selected == group_pin)
                {
                    if (connection_available_ && conversation == active_conversation_ && messages_->can_manage_group())
                    {
                        emit group_message_pin_requested(conversation,
                            unpin ? std::nullopt : std::optional<qint64>(message_id));
                    }
                    return;
                }
                if (readers && selected == readers)
                {
                    show_read_details(conversation, message_id);
                    return;
                }
                if ((download && selected == download) || (preview && selected == preview))
                {
                    emit attachment_open_requested(conversation, message_id,
                                                   filename, preview && selected == preview);
                    return;
                }
                if (remove && selected == remove)
                {
                    if (confirm_action(this, QStringLiteral("删除消息"),
                                              QStringLiteral("删除后所有成员均显示“消息已删除”。确认删除？"),
                                              QStringLiteral("删除")) &&
                        connection_available_ && conversation == active_conversation_)
                    {
                        emit delete_message_requested(conversation, message_id);
                    }
                    return;
                }
                if (edit && selected == edit)
                {
                    if (!can_send() || conversation != active_conversation_) { return; }
                    QInputDialog dialog(this);
                    dialog.setObjectName(QStringLiteral("editMessageDialog"));
                    dialog.setWindowTitle(QStringLiteral("编辑消息"));
                    dialog.setLabelText(QStringLiteral("消息内容"));
                    dialog.setOption(QInputDialog::UsePlainTextEditForTextInput);
                    dialog.setInputMode(QInputDialog::TextInput);
                    dialog.setTextValue(message_text);
                    dialog.setOkButtonText(QStringLiteral("保存"));
                    dialog.setCancelButtonText(QStringLiteral("取消"));
                    dialog.layout()->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                                                       chat_theme::dialog_padding, chat_theme::dialog_padding);
                    dialog.layout()->setSpacing(chat_theme::dialog_spacing);
                    auto* editor = dialog.findChild<QPlainTextEdit*>();
                    editor->setObjectName(QStringLiteral("editMessageText"));
                    editor->setAccessibleName(QStringLiteral("消息内容"));
                    editor->setMinimumWidth(chat_theme::dialog_normal_width - 2 * chat_theme::dialog_padding);
                    editor->setMinimumHeight(200);
                    editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
                    editor->setTabChangesFocus(true);
                    editor->style()->unpolish(editor);
                    editor->style()->polish(editor);
                    new emoji_highlighter(editor);
                    auto* buttons = dialog.findChild<QDialogButtonBox*>();
                    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("editMessageButton"));
                    for (auto* button : buttons->buttons())
                    {
                        button->setIcon({});
                        button->style()->unpolish(button);
                        button->style()->polish(button);
                    }
                    if (dialog.exec() != QDialog::Accepted) { return; }
                    auto text = dialog.textValue();
                    if (!text.isEmpty() && can_send() && conversation == active_conversation_)
                    {
                        emit edit_message_requested(conversation, message_id, std::move(text));
                    }
                    return;
                }
                if (!reply || selected != reply || !can_send())
                {
                    return;
                }
                reply_to_ = message_id;
                reply_preview_->setText(QStringLiteral("回复 %1：%2")
                                            .arg(sender_name, grapheme_prefix(message_text, 80)));
                reply_bar_->show();
                message_edit_->setFocus();
            });

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
    message_edit_ = new QPlainTextEdit(input_bar);
    message_edit_->setObjectName(QStringLiteral("messageEdit"));
    message_edit_->setPlaceholderText(QStringLiteral("输入消息…"));
    message_edit_->setAccessibleName(QStringLiteral("消息输入"));
    message_edit_->setToolTip(QStringLiteral("Enter 发送，Shift+Enter 换行"));
    message_edit_->setTabChangesFocus(true);
    message_edit_->document()->setDocumentMargin(8);
    message_edit_->setFixedHeight(chat_theme::compose_field_min_height);
    message_edit_->installEventFilter(this);
    message_edit_->setEnabled(false);
    new emoji_highlighter(message_edit_);
    input_layout->addWidget(message_edit_, 1, Qt::AlignBottom);
    auto const resize_composer = [this] {
        auto* document = message_edit_->document();
        auto height = 2 * document->documentMargin() + 1;
        int lines = 0;
        for (auto block = document->begin(); block.isValid() && lines < 6; block = block.next())
        {
            document->documentLayout()->blockBoundingRect(block);
            auto* text = block.layout();
            for (int row = 0; row < text->lineCount() && lines < 6; ++row, ++lines)
            {
                auto const line = text->lineAt(row);
                height += line.height() + qMin(0, qCeil(line.leading()));
            }
        }
        message_edit_->setFixedHeight(qMax(chat_theme::compose_field_min_height, qCeil(height)));
    };
    connect(message_edit_->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, resize_composer, Qt::QueuedConnection);
    connect(message_edit_, &QPlainTextEdit::updateRequest, this, resize_composer, Qt::QueuedConnection);
    attachment_button_ = new QToolButton(input_bar);
    attachment_button_->setObjectName(QStringLiteral("sendAttachmentButton"));
    attachment_button_->setText(QStringLiteral("文件/图片"));
    attachment_button_->setEnabled(false);
    input_layout->addWidget(attachment_button_, 0, Qt::AlignBottom);
    connect(attachment_button_, &QToolButton::clicked, this, [this] {
        auto const path = QFileDialog::getOpenFileName(this, QStringLiteral("发送文件或图片"));
        if (path.isEmpty())
        {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            set_message_status(file.errorString());
            return;
        }
        if (file.size() > static_cast<qint64>(chat::max_attachment_size))
        {
            set_message_status(QStringLiteral("文件不能超过 10 MiB。"));
            return;
        }
        auto data = file.read(chat::max_attachment_size + 1);
        if (file.error() != QFileDevice::NoError || data.size() > static_cast<qint64>(chat::max_attachment_size))
        {
            set_message_status(QStringLiteral("无法读取文件，或文件超过 10 MiB。"));
            return;
        }
        if (!can_send())
        {
            set_message_status(connection_available_ ? QStringLiteral("当前会话暂时无法发送文件。")
                                                     : QStringLiteral("连接已断开，请重新发送文件。"));
            return;
        }
        auto const reply = reply_to_;
        reply_to_ = 0;
        reply_bar_->hide();
        attachment_sending_ = true;
        stop_typing();
        attachment_button_->setEnabled(false);
        set_message_status(QStringLiteral("正在发送 %1…").arg(QFileInfo(path).fileName()));
        emit attachment_send_requested(active_conversation_, QFileInfo(path).fileName(), std::move(data), reply);
    });
    send_button_ = new QToolButton(input_bar);
    send_button_->setObjectName(QStringLiteral("sendButton"));
    send_button_->setIcon(svg_icon(QStringLiteral("send"), QColor(QStringLiteral("#315A4B")), QSize(22, 22)));
    send_button_->setIconSize(QSize(22, 22));
    send_button_->setToolTip(QStringLiteral("发送消息（Enter）"));
    send_button_->setAccessibleName(QStringLiteral("发送消息"));
    send_button_->setFixedSize(chat_theme::compose_button_width, chat_theme::compose_button_height);
    send_button_->setEnabled(false);
    send_button_->setCursor(Qt::PointingHandCursor);
    input_layout->addWidget(send_button_, 0, Qt::AlignBottom);
    chat_layout->addWidget(input_bar);

    layout->addWidget(navigation_panel);
    layout->addWidget(conversation_panel);
    layout->addWidget(make_separator(this));
    layout->addWidget(chat_panel, 1);

    connect(profile_avatar_, &QToolButton::clicked, this, [this] {
        show_user_details(self_user_, profile_avatar_->toolTip());
    });
    connect(&avatars_, &avatar_cache::changed, this, [this](qint64 user) {
        if (user == self_user_)
        {
            profile_avatar_->setIcon(avatar_icon(profile_avatar_->toolTip(), 44, avatars_.image(user)));
        }
        if (!active_group_ && user == active_peer_) { update_chat_header(active_username_); }
        for (auto* list : {incoming_friends_, outgoing_friends_})
        {
            for (int row = 0; row < list->count(); ++row)
            {
                auto* item = list->item(row);
                if (item->data(Qt::UserRole).toLongLong() == user)
                { item->setData(Qt::DecorationRole, avatars_.image(user)); }
            }
        }
    });
    connect(chats_navigation_, &QToolButton::clicked, this, [this] { show_conversations_section(); });
    connect(contacts_navigation_, &QToolButton::clicked, this, [this] { show_contacts_section(); });
    connect(create_group, &QAction::triggered, this, [this] { this->create_group(); });
    connect(add_friend, &QAction::triggered, this, [this] { show_add_contact_section(); });
    connect(join_group, &QAction::triggered, this, [this] {
        QInputDialog dialog(this);
        dialog.setObjectName(QStringLiteral("joinGroupDialog"));
        dialog.setWindowTitle(QStringLiteral("加入群聊"));
        dialog.setLabelText(QStringLiteral("粘贴完整邀请链接"));
        dialog.setInputMode(QInputDialog::TextInput);
        dialog.setOkButtonText(QStringLiteral("加入"));
        dialog.setCancelButtonText(QStringLiteral("取消"));
        dialog.layout()->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                                           chat_theme::dialog_padding, chat_theme::dialog_padding);
        dialog.layout()->setSpacing(chat_theme::dialog_spacing);
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("joinGroupButton"));
        for (auto* button : buttons->buttons())
        {
            button->setIcon({});
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
        auto* input = dialog.findChild<QLineEdit*>();
        input->setMinimumWidth(chat_theme::dialog_small_width - 2 * chat_theme::dialog_padding);
        input->setPlaceholderText(QStringLiteral("chat://join/…"));
        input->setAccessibleName(QStringLiteral("邀请链接"));
        if (dialog.exec() != QDialog::Accepted || !connection_available_) { return; }
        auto const link = dialog.textValue().trimmed();
        static QRegularExpression const pattern(QStringLiteral("\\Achat://join/([0-9a-f]{64})\\z"));
        auto const matched = pattern.match(link);
        if (!matched.hasMatch()) { set_error(QStringLiteral("邀请链接无效，请复制完整链接。")); return; }
        emit group_join_requested(matched.captured(1));
    });
    connect(sidebar_back_button_, &QToolButton::clicked, this, [this] {
        if (sidebar_pages_->currentIndex() == 2)
        {
            switch (add_friend_parent_)
            {
                case sidebar_parent::chats: show_conversations_section(); return;
                case sidebar_parent::new_friends: show_new_friends_section(); return;
                case sidebar_parent::contacts: break;
            }
        }
        show_contacts_section();
    });
    connect(add_contact_button_, &QToolButton::clicked, this, [this] { show_add_contact_section(); });
    connect(contact_search_, &QLineEdit::textChanged, this, [this](QString const& query) { filter_contacts(query); });
    connect(contacts_delegate, &user_delegate::body_clicked, this, [this](QModelIndex const& index) { select_contact(index); });
    connect(contacts_view_, &QListView::activated, this, &chat_widget::select_contact);
    connect(contacts_delegate, &user_delegate::avatar_clicked, this, [this](QModelIndex index) {
        auto const* user = contacts_->user_at(contacts_filter_->mapToSource(index));
        if (user) { show_user_details(user->id, user->username); }
    });
    connect(add_user_search_, &QLineEdit::returnPressed, this, [this] { search_users(); });
    connect(add_users_view_, &QListView::clicked, this, [this](QModelIndex const& index) { select_add_user(index); });
    connect(add_users_view_, &QListView::activated, this, &chat_widget::select_add_user);
    connect(conversations_delegate, &conversation_delegate::body_clicked, this, [this](QModelIndex const& index) { select_conversation(index); });
    connect(conversations_view_, &QListView::activated, this, &chat_widget::select_conversation);
    connect(conversations_view_, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        auto const* item = conversations_->conversation_at(conversations_view_->indexAt(point));
        if (!connection_available_ || !item) { return; }
        auto const conversation = item->id;
        auto const muted = !item->muted;
        auto const pinned = !item->pinned;
        QMenu menu(this);
        menu.addAction(pinned ? QStringLiteral("置顶") : QStringLiteral("取消置顶"),
            [this, conversation, pinned] {
                if (connection_available_ && conversations_->index_for_conversation(conversation).isValid())
                {
                    emit pin_requested(conversation, pinned);
                }
            });
        menu.addAction(muted ? QStringLiteral("静音") : QStringLiteral("取消静音"),
            [this, conversation, muted] {
                if (connection_available_ && conversations_->index_for_conversation(conversation).isValid())
                {
                    emit mute_requested(conversation, muted);
                }
            });
        menu.exec(conversations_view_->viewport()->mapToGlobal(point));
    });
    connect(conversations_delegate, &conversation_delegate::avatar_clicked, this, [this](QModelIndex const& index) {
        if (auto const* item = conversations_->conversation_at(index))
        {
            if (item->group)
            {
                open_conversation(*item);
                emit members_requested(item->id, self_user_, item->username);
            }
            else
            {
            show_user_details(item->user, item->username);
        }
        }
    });
    connect(messages_delegate, &message_delegate::avatar_clicked, this,
            [this](QModelIndex const& index)
        {
                show_user_details(index.data(message_model::from_role).toLongLong(),
                                  index.data(message_model::sender_name_role).toString());
            });
    connect(messages_delegate, &message_delegate::read_details_clicked, this, [this](QModelIndex const& index) {
        if (index.data(message_model::read_count_role).isValid())
        {
            show_read_details(active_conversation_, index.data(message_model::id_role).toLongLong());
        }
    });
    connect(messages_delegate, &message_delegate::image_clicked, this, [this](QModelIndex const& index) {
        if (connection_available_ && !index.data(message_model::deleted_role).toBool())
        {
            emit attachment_open_requested(active_conversation_, index.data(message_model::id_role).toLongLong(),
                                           index.data(message_model::attachment_name_role).toString(), true);
        }
    });
    connect(messages_delegate, &message_delegate::reaction_clicked, this,
            [this](QModelIndex const& index, QString emoji) {
        if (can_send() && !index.data(message_model::deleted_role).toBool())
        {
            emit reaction_requested(active_conversation_, index.data(message_model::id_role).toLongLong(),
                emoji == index.data(message_model::own_reaction_role).toString() ? QString{} : emoji);
        }
    });
    connect(chat_title_, &QPushButton::clicked, this,
            [this]
            {
                if (active_conversation_ > 0)
                {
                    if (active_group_)
                    {
                        emit members_requested(active_conversation_, self_user_, active_username_);
                    }
                    else
                    {
                        show_user_details(active_peer_, active_username_);
                    }
        }
    });
    connect(connection_status_, &QToolButton::clicked, this, [this] {
        if (connection_status_->isEnabled())
        {
            emit reconnect_requested();
        }
    });
    connect(send_button_, &QToolButton::clicked, this, [this] { send_current_message(); });
    typing_clock_.start();
    typing_idle_timer_ = new QTimer(this);
    typing_idle_timer_->setSingleShot(true);
    typing_idle_timer_->setInterval(3000);
    connect(typing_idle_timer_, &QTimer::timeout, this, [this] {
        if (can_send()) { emit typing_requested(active_conversation_, false); }
    });
    connect(message_edit_, &QPlainTextEdit::textChanged, this, [this] {
        update_compose_state();
        if (!can_send())
        {
            return;
        }
        if (message_edit_->toPlainText().isEmpty())
        {
            stop_typing();
            return;
        }
        auto const now = typing_clock_.elapsed();
        if (!typing_idle_timer_->isActive() || now - last_typing_sent_ >= 2000)
        {
            last_typing_sent_ = now;
            emit typing_requested(active_conversation_, true);
        }
        typing_idle_timer_->start();
    });
    auto* typing_expiry = new QTimer(this);
    typing_expiry->setInterval(500);
    connect(typing_expiry, &QTimer::timeout, this, [this] {
        for (auto it = typing_users_.begin(); it != typing_users_.end();)
        {
            it = it->expiry.hasExpired() ? typing_users_.erase(it) : std::next(it);
        }
        update_typing_label();
    });
    typing_expiry->start();
    connect(messages_view_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        load_visible_images();
        mark_visible_messages();
        if (value == messages_view_->verticalScrollBar()->minimum())
        {
            request_older_messages();
        }
    });
}

void chat_widget::set_user(QString const& username, qint64 user)
{
    stop_typing();
    typing_users_.clear();
    update_typing_label();
    avatars_.clear();
    images_.clear();
    avatar_updating_ = false;
    profile_avatar_->setIcon(avatar_icon(username, 44));
    profile_avatar_->setToolTip(username);
    profile_avatar_->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
    self_user_ = user;
    drafts_.clear();
    message_sending_.clear();
    attachment_sending_ = false;
    active_peer_ = 0;
    active_group_ = false;
    reply_to_ = 0;
    reply_bar_->hide();
    active_conversation_ = 0;
    update_pinned_message();
    active_username_.clear();
    presence_.clear();
    conversations_->set_conversations({});
    conversations_view_->setCurrentIndex({});
    conversations_status_->setText(QStringLiteral("暂无会话"));
    messages_->set_self_user(user);
    messages_->reset(0);
    contacts_->set_users({});
    set_friend_requests({}, {}, {});
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
    chat_presence_->clear();
    chat_presence_->hide();
    set_message_status(QStringLiteral("选择一个会话开始聊天"));
    message_edit_->clear();
    message_edit_->setEnabled(false);
    send_button_->setEnabled(false);
    message_search_button_->setEnabled(false);
    attachment_button_->setEnabled(false);
}

bool chat_widget::eventFilter(QObject* object, QEvent* event)
{
    if ((object == incoming_friends_->viewport() || object == outgoing_friends_->viewport()) && event->type() == QEvent::Resize)
    {
        auto* list = object == incoming_friends_->viewport() ? incoming_friends_ : outgoing_friends_;
        QTimer::singleShot(0, list, [list] {
            if (list->hasFocus() && list->currentItem()) { list->scrollToItem(list->currentItem()); }
        });
    }
    if ((object == incoming_friends_ || object == outgoing_friends_) && event->type() == QEvent::KeyPress)
    {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            auto* item = static_cast<QListWidget*>(object)->currentItem();
            if (item) { show_user_details(item->data(Qt::UserRole).toLongLong(), item->data(Qt::UserRole + 1).toString()); }
            return true;
        }
    }
    if (object == message_edit_ && event->type() == QEvent::KeyPress)
    {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            !(key->modifiers() & Qt::ShiftModifier))
        {
            if (!message_edit_->textCursor().block().layout()->preeditAreaText().isEmpty())
            {
                QGuiApplication::inputMethod()->commit();
                return true;
            }
            send_current_message();
            return true;
        }
    }
    if (object == messages_view_->viewport() && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
    {
        auto* scroll = messages_view_->verticalScrollBar();
        if (event->type() == QEvent::Resize && scroll->value() == scroll->maximum())
        {
            auto const position = scroll->value();
            auto const conversation = active_conversation_;
            QTimer::singleShot(0, messages_view_, [this, position, conversation] {
                if (conversation == active_conversation_ && messages_view_->verticalScrollBar()->value() == position)
                {
                    messages_view_->scrollToBottom();
                    mark_visible_messages();
                }
            });
        }
        QTimer::singleShot(0, messages_view_, [this] { load_visible_images(); });
    }
    return QWidget::eventFilter(object, event);
}

void chat_widget::load_visible_images()
{
    if (!connection_available_ || active_conversation_ <= 0 || messages_->rowCount() == 0) { return; }
    auto const first = messages_view_->indexAt(QPoint(0, 0));
    auto const last = messages_view_->indexAt(QPoint(0, messages_view_->viewport()->height() - 1));
    for (int row = first.isValid() ? first.row() : 0;
         row <= (last.isValid() ? last.row() : messages_->rowCount() - 1); ++row)
    {
        auto const index = messages_->index(row, 0);
        if (!messages_view_->visualRect(index).intersects(messages_view_->viewport()->rect())) { continue; }
        if (index.data(message_model::attachment_type_role).toString().startsWith(QStringLiteral("image/")))
        {
            images_.observe(active_conversation_, index.data(message_model::id_role).toLongLong());
        }
    }
}

void chat_widget::set_loading() { conversations_status_->setText(QStringLiteral("正在加载…")); }

void chat_widget::set_error(QString message)
{
    // Operation feedback outlives list refreshes, which own conversations_status_.
    notice_->setVisible(!message.isEmpty());
    notice_->show_error(std::move(message));
    notice_timer_->start();
}

void chat_widget::set_conversations_error(QString message) { conversations_status_->setText(std::move(message)); }

void chat_widget::set_connection_available(bool available)
{
    if (connection_available_ == available) { return; }
    if (!available)
    {
        stop_typing();
        set_presences({});
        typing_users_.clear();
        update_typing_label();
        if (message_sending_.contains(active_conversation_))
        {
            set_message_status(QStringLiteral("连接已断开，草稿已保留。重连后请查看历史确认是否送达。"));
        }
        message_sending_.clear();
    }
    if (!available && avatar_updating_)
    {
        finish_avatar_update(QStringLiteral("连接已断开，请重新上传头像。"));
    }
    connection_available_ = available;
    emit friendship_updated();
    images_.retry();
    if (available) { QTimer::singleShot(0, messages_view_, [this] { load_visible_images(); }); }
    if (!available && attachment_sending_)
    {
        attachment_sending_ = false;
        set_message_status(QStringLiteral("连接已断开，请重新发送文件。"));
    }
    if (!available && active_group_)
    {
        messages_->set_read_positions({});
    }
    chats_actions_->setEnabled(available);
    for (auto* action : chats_actions_->menu()->actions()) { action->setEnabled(available); }
    messages_loading_ = available && active_conversation_ > 0;
    update_compose_state();
    add_contact_button_->setEnabled(available);
    message_search_button_->setEnabled(available && active_conversation_ > 0);
    update_pinned_message();
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
    auto const previous_user = active_conversation_;
    conversations_->set_conversations(std::move(conversations));
    for (auto it = drafts_.begin(); it != drafts_.end();)
    {
        if (!conversations_->index_for_conversation(it.key()).isValid()) { it = drafts_.erase(it); }
        else { ++it; }
    }
    message_sending_.removeIf([this](qint64 id) { return !conversations_->index_for_conversation(id).isValid(); });
    update_compose_state();
    update_pinned_message();
    for (auto const& item : presence_)
    {
        conversations_->set_online(item.user, item.online);
    }
    if (previous_user > 0 && !conversations_->index_for_conversation(previous_user).isValid())
    {
        close_conversation(previous_user);
    }
    if (conversations_->rowCount() == 0)
    {
        conversations_status_->setText(QStringLiteral("暂无会话"));
        conversations_view_->setCurrentIndex({});
        return;
    }

    conversations_status_->clear();
    if (previous_user > 0)
    {
        auto const index = conversations_->index_for_conversation(previous_user);
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
            active_member_count_ = item->member_count;
            update_chat_header(item->username);
        }
        return;
    }

    // Opening a conversation reports it read; only the user's own choice may do that.
    conversations_view_->setCurrentIndex({});
    conversations_view_->clearSelection();
}

void chat_widget::set_contacts(QList<user_data> contacts)
{
    contacts_->set_users(std::move(contacts));
    for (auto it = presence_.begin(); it != presence_.end();)
    {
        if (!is_contact(it.key()))
        {
            conversations_->set_online(it.key(), false);
            it = presence_.erase(it);
        }
        else { ++it; }
    }
    for (auto const& item : presence_)
    {
        contacts_->set_presence(item.user, item.online, item.last_seen);
    }
    filter_contacts(contact_search_->text());
    update_compose_state();
    update_chat_presence();
}

void chat_widget::set_presences(QList<presence_data> users)
{
    for (auto const& old : presence_)
    {
        conversations_->set_online(old.user, false);
        contacts_->set_presence(old.user, false, 0);
    }
    presence_.clear();
    for (auto& user : users)
    {
        set_presence(std::move(user));
    }
    update_chat_presence();
}

void chat_widget::set_presence(presence_data user)
{
    if (user.user <= 0 || !is_contact(user.user))
    {
        return;
    }

    presence_.insert(user.user, user);
    if (!user.online && typing_users_.remove(user.user))
    {
        update_typing_label();
    }
    conversations_->set_online(user.user, user.online);
    contacts_->set_presence(user.user, user.online, user.last_seen);
    if (user.user == active_peer_)
    {
        update_chat_presence();
    }
}

void chat_widget::set_contacts_error(QString message)
{
    set_contacts({});
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

void chat_widget::finish_add_contact(qint64 user, QString error)
{
    emit contact_add_finished(user, error);
    friend_requests_status_->setText(error);
    friend_requests_status_->setVisible(!error.isEmpty());
}

void chat_widget::set_friend_requests(QList<user_data> incoming, QList<user_data> outgoing, QString error)
{
    if (!error.isEmpty()) { friend_requests_status_->setText(error); friend_requests_status_->show(); return; }
    incoming_requests_ = std::move(incoming);
    outgoing_requests_ = std::move(outgoing);
    new_friends_button_->setText(incoming_requests_.empty() ? QStringLiteral("新的朋友")
        : QStringLiteral("新的朋友  ·  %1").arg(incoming_requests_.size()));
    auto populate = [this](QListWidget* list, QList<user_data> const& users, QString const& hint) {
        auto const selected_user = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : 0;
        auto const selected_visible = list->currentItem() &&
            list->viewport()->rect().intersects(list->visualItemRect(list->currentItem()));
        auto const scroll_position = list->verticalScrollBar()->value();
        list->clear();
        for (auto const& user : users)
        {
            avatars_.observe(user.id, user.avatar);
            auto* item = new QListWidgetItem(user.username, list);
            item->setData(Qt::UserRole, user.id);
            item->setData(Qt::UserRole + 1, user.username);
            item->setData(Qt::StatusTipRole, hint);
            item->setData(Qt::DecorationRole, avatars_.image(user.id));
            item->setToolTip(user.username + QStringLiteral(" · ") + hint);
            if (user.id == selected_user) { list->setCurrentItem(item); }
        }
        list->setMaximumHeight(users.size() * chat_theme::dialog_row_height);
        list->setVisible(!users.empty());
        list->doItemsLayout();
        list->verticalScrollBar()->setValue(scroll_position);
        if (selected_visible && list->currentItem()) { list->scrollToItem(list->currentItem()); }
    };
    populate(incoming_friends_, incoming_requests_, QStringLiteral("待处理"));
    populate(outgoing_friends_, outgoing_requests_, QStringLiteral("等待验证"));
    incoming_friends_title_->setText(QStringLiteral("收到的申请 · %1").arg(incoming_requests_.size()));
    incoming_friends_title_->setVisible(!incoming_requests_.empty());
    outgoing_friends_title_->setText(QStringLiteral("发出的申请 · %1").arg(outgoing_requests_.size()));
    outgoing_friends_title_->setVisible(!outgoing_requests_.empty());
    friend_requests_status_->setText(incoming_requests_.empty() && outgoing_requests_.empty()
        ? QStringLiteral("暂无好友申请") : QString{});
    friend_requests_status_->setVisible(incoming_requests_.empty() && outgoing_requests_.empty());
    update_compose_state();
    emit friendship_updated();
}

chat::friendship_state chat_widget::friend_state(qint64 user) const
{
    if (is_contact(user)) { return chat::friendship_state::accepted; }
    for (auto const& request : incoming_requests_) { if (request.id == user) { return chat::friendship_state::incoming_pending; } }
    for (auto const& request : outgoing_requests_) { if (request.id == user) { return chat::friendship_state::outgoing_pending; } }
    return chat::friendship_state::none;
}

void chat_widget::set_messages(qint64 user, QList<message_data> messages, read_positions positions, bool older,
                               bool recovering, bool has_more)
{
    if (user != active_conversation_)
    {
        return;
    }

    auto* scroll = messages_view_->verticalScrollBar();
    auto const old_maximum = scroll->maximum();
    auto const old_value = scroll->value();
    messages_->merge_messages(std::move(messages));
    messages_->set_read_positions(std::move(positions));
    messages_loading_ = recovering && has_more;
    if (!recovering)
    {
        history_exhausted_ = history_exhausted_ || !has_more;
    }

    if (!older)
    {
        messages_loaded_ = true;
        set_message_status(messages_->rowCount() == 0 ? QStringLiteral("暂无消息") : QString{});
        QTimer::singleShot(0, messages_view_, [this] {
            messages_view_->scrollToBottom();
            mark_visible_messages();
        });
        return;
    }

    set_message_status({});
    QTimer::singleShot(0, messages_view_, [scroll, old_maximum, old_value] {
        scroll->setValue(old_value + scroll->maximum() - old_maximum);
    });
}

void chat_widget::set_read_message(qint64 conversation, qint64 user, qint64 message)
{
    if (conversation == active_conversation_)
    {
        messages_->set_read_message(user, message);
    }
}

void chat_widget::reset_read_positions(qint64 conversation)
{
    if (conversation == active_conversation_ && active_group_)
    {
        messages_->set_read_positions({});
    }
}

void chat_widget::add_message(qint64 user, message_data message)
{
    if (user != active_conversation_)
    {
        return;
    }

    auto* scroll = messages_view_->verticalScrollBar();
    auto const position = scroll->value();
    auto const at_bottom = position == scroll->maximum();
    if (messages_->add_message(std::move(message)))
    {
        set_message_status({});
        if (at_bottom)
        {
            QTimer::singleShot(0, messages_view_, [this, user, position] {
                if (user == active_conversation_ && messages_view_->verticalScrollBar()->value() == position)
                {
                    messages_view_->scrollToBottom();
                    mark_visible_messages();
                }
            });
        }
    }
}

void chat_widget::update_message(message_data message)
{
    messages_->update_message(message);
    if (message.conversation != active_conversation_)
    {
        return;
    }
    if (reply_to_ == message.id)
    {
        if (message.deleted)
        {
            reply_to_ = 0;
            reply_bar_->hide();
        }
        else
        {
            reply_preview_->setText(QStringLiteral("回复 %1：%2").arg(message.username, grapheme_prefix(message.text, 80)));
        }
    }
}

bool chat_widget::set_reactions(qint64 conversation, qint64 message, qint64 revision, QList<reaction_data> reactions)
{
    return conversation != active_conversation_ || messages_->set_reactions(message, revision, std::move(reactions));
}

void chat_widget::finish_message_send(qint64 user, qint64 message, qint64 timestamp, QString text,
                                      quoted_message_data reply, QList<mention_data> mentions, QString error)
{
    if (message_sending_.remove(user) && error.isEmpty())
    {
        if (user == active_conversation_)
        {
            if (message_edit_->toPlainText() == text && reply_to_ == reply.id)
            {
                stop_typing();
                message_edit_->clear();
                reply_to_ = 0;
                reply_bar_->hide();
            }
        }
        else if (drafts_.value(user) == text) { drafts_.remove(user); }
    }
    update_compose_state();
    if (!error.isEmpty())
    {
        set_message_error(user, QStringLiteral("发送失败：%1。草稿已保留。").arg(error));
        return;
    }
    if (user != active_conversation_)
    {
        return;
    }

    message_data value;
    value.id = message;
    value.conversation = user;
    value.from = self_user_;
    value.username = profile_avatar_->toolTip();
    value.timestamp = timestamp;
    value.text = std::move(text);
    value.reply = std::move(reply);
    value.mentions = std::move(mentions);
    add_message(user, std::move(value));
}

void chat_widget::finish_attachment_send(qint64 conversation, QString error_message)
{
    attachment_sending_ = false;
    update_compose_state();
    if (conversation == active_conversation_)
    {
        set_message_status(std::move(error_message));
    }
}

void chat_widget::set_message_error(qint64 user, QString message)
{
    if (user == active_conversation_)
    {
        messages_loading_ = false;
        set_message_status(std::move(message));
    }
}

qint64 chat_widget::active_conversation() const noexcept
{
    return active_conversation_;
}

qint64 chat_widget::latest_message_id() const { return messages_->last_message_id(); }

void chat_widget::show_conversations_section()
{
    section_title_->setText(QStringLiteral("消息"));
    sidebar_pages_->setCurrentIndex(0);
    sidebar_back_button_->hide();
    add_contact_button_->hide();
    chats_actions_->show();
    set_navigation_button(chats_navigation_, QStringLiteral("chat"), true);
    set_navigation_button(contacts_navigation_, QStringLiteral("contacts"), false);
}

void chat_widget::show_contacts_section()
{
    section_title_->setText(QStringLiteral("联系人"));
    sidebar_pages_->setCurrentIndex(1);
    sidebar_back_button_->hide();
    add_contact_button_->show();
    chats_actions_->hide();
    set_navigation_button(chats_navigation_, QStringLiteral("chat"), false);
    set_navigation_button(contacts_navigation_, QStringLiteral("contacts"), true);
    contact_search_->setFocus();
}

void chat_widget::show_add_contact_section()
{
    if (sidebar_pages_->currentIndex() != 2)
    {
        add_friend_parent_ = sidebar_pages_->currentIndex() == 0 ? sidebar_parent::chats
            : sidebar_pages_->currentIndex() == 3 ? sidebar_parent::new_friends : sidebar_parent::contacts;
    }
    section_title_->setText(QStringLiteral("添加好友"));
    sidebar_pages_->setCurrentIndex(2);
    sidebar_back_button_->show();
    add_contact_button_->hide();
    chats_actions_->hide();
    set_navigation_button(chats_navigation_, QStringLiteral("chat"), add_friend_parent_ == sidebar_parent::chats);
    set_navigation_button(contacts_navigation_, QStringLiteral("contacts"), add_friend_parent_ != sidebar_parent::chats);
    add_user_search_->setFocus();
}

void chat_widget::show_new_friends_section()
{
    section_title_->setText(QStringLiteral("新的朋友"));
    sidebar_pages_->setCurrentIndex(3);
    sidebar_back_button_->show();
    add_contact_button_->show();
    chats_actions_->hide();
    set_navigation_button(chats_navigation_, QStringLiteral("chat"), false);
    set_navigation_button(contacts_navigation_, QStringLiteral("contacts"), true);
}

void chat_widget::filter_contacts(QString const& query)
{
    contacts_filter_->setFilterFixedString(query);
    contacts_filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);

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

    show_user_details(item->id, item->username);
}

void chat_widget::select_conversation(QModelIndex const& index)
{
    auto const* item = conversations_->conversation_at(index);
    if (item)
    {
        open_conversation(*item);
    }
}

void chat_widget::open_chat(qint64 user, QString username)
{
    if (connection_available_ && is_contact(user))
    {
        emit direct_conversation_requested(user, std::move(username));
    }
}

bool chat_widget::is_contact(qint64 user) const
{
    for (int row = 0; row < contacts_->rowCount(); ++row)
    {
        if (contacts_->user_at(contacts_->index(row, 0))->id == user) { return true; }
    }
    return false;
}

bool chat_widget::can_send() const
{
    auto const current = conversation(active_conversation_);
    return connection_available_ && current && current->can_send;
}

void chat_widget::update_compose_state()
{
    auto const current = conversation(active_conversation_);
    auto const allowed = can_send();
    message_edit_->setEnabled(allowed);
    send_button_->setEnabled(allowed && !message_sending_.contains(active_conversation_) && !message_edit_->toPlainText().isEmpty());
    attachment_button_->setEnabled(allowed && !attachment_sending_);
    auto hint = QStringLiteral("输入消息…");
    if (current && !current->group && !current->can_send)
    {
        switch (friend_state(current->user))
        {
            case chat::friendship_state::outgoing_pending:
                hint = QStringLiteral("好友申请已发出，等待对方接受"); break;
            case chat::friendship_state::incoming_pending:
                hint = QStringLiteral("有待处理的好友申请，接受后可发送消息"); break;
            case chat::friendship_state::accepted:
                hint = QStringLiteral("正在更新会话状态，请稍候"); break;
            case chat::friendship_state::none:
                hint = QStringLiteral("你们还不是好友，添加好友并通过验证后可发送消息"); break;
        }
    }
    message_edit_->setPlaceholderText(hint);
    if (!allowed)
    {
        typing_idle_timer_->stop();
        reply_to_ = 0;
        reply_bar_->hide();
    }
}

void chat_widget::open_conversation(conversation_data conversation)
{
    auto const user = conversation.id;
    if (user <= 0) { return; }
    auto const existing = conversations_->index_for_conversation(user);
    if (!existing.isValid() || conversations_->conversation_at(existing)->can_send != conversation.can_send)
    {
        QList<conversation_data> items;
        for (int row = 0; row < conversations_->rowCount(); ++row)
        {
            auto item = *conversations_->conversation_at(conversations_->index(row, 0));
            if (item.id == user) { item.can_send = conversation.can_send; }
            items.push_back(std::move(item));
        }
        if (!existing.isValid()) { items.push_back(conversation); }
        conversations_->set_conversations(std::move(items));
    }
    if (sidebar_pages_->currentIndex() != 0) { show_conversations_section(); }
    auto username = conversation.username;
    active_peer_ = conversation.user;
    active_group_ = conversation.group;
    active_member_count_ = conversation.member_count;

    active_username_ = std::move(username);
    update_chat_header(active_username_);

    auto const conversation_index = conversations_->index_for_conversation(user);
    if (conversation_index.isValid())
    {
        conversations_view_->setCurrentIndex(conversation_index);
    }
    else
    {
        conversations_view_->setCurrentIndex({});
        conversations_view_->clearSelection();
    }

    if (active_conversation_ == user)
    {
        update_compose_state();
        if (!messages_loaded_ && !messages_loading_)
        {
            messages_loading_ = true;
            set_message_status(QStringLiteral("正在加载消息…"));
            if (connection_available_)
            {
                emit conversation_selected(active_conversation_, active_group_);
            }
        }
        return;
    }

    stop_typing();
    typing_users_.clear();
    update_typing_label();
    reply_to_ = 0;
    reply_bar_->hide();
    images_.discard_queued();
    if (active_conversation_ > 0)
    {
        auto text = message_edit_->toPlainText();
        if (text.isEmpty()) { drafts_.remove(active_conversation_); }
        else { drafts_.insert(active_conversation_, std::move(text)); }
    }
    active_conversation_ = user;
    {
        QSignalBlocker const blocker(message_edit_);
        message_edit_->setPlainText(drafts_.take(user));
        message_edit_->moveCursor(QTextCursor::End);
    }
    message_search_button_->setEnabled(connection_available_);
    messages_->reset(active_conversation_, active_group_);
    update_pinned_message();
    messages_loaded_ = false;
    messages_loading_ = true;
    history_exhausted_ = false;
    set_message_status(QStringLiteral("正在加载消息…"));
    update_compose_state();
    if (connection_available_)
    {
        emit conversation_selected(active_conversation_, active_group_);
    }
}

void chat_widget::close_conversation(qint64 conversation)
{
    drafts_.remove(conversation);
    message_sending_.remove(conversation);
    if (conversation != active_conversation_)
    {
        images_.remove(conversation);
        return;
    }
    stop_typing();
    typing_users_.clear();
    update_typing_label();
    active_conversation_ = 0;
    update_pinned_message();
    active_peer_ = 0;
    active_group_ = false;
    active_member_count_ = 0;
    active_username_.clear();
    attachment_sending_ = false;
    messages_loaded_ = false;
    messages_loading_ = false;
    history_exhausted_ = false;
    reply_to_ = 0;
    reply_bar_->hide();
    messages_->reset(0);
    images_.remove(conversation);
    conversations_view_->setCurrentIndex({});
    conversations_view_->clearSelection();
    chat_title_->setText(QStringLiteral("聊天"));
    chat_title_->setIcon({});
    chat_title_->setEnabled(false);
    chat_presence_->clear();
    chat_presence_->hide();
    message_edit_->clear();
    message_edit_->setEnabled(false);
    send_button_->setEnabled(false);
    message_search_button_->setEnabled(false);
    attachment_button_->setEnabled(false);
    set_message_status(QStringLiteral("选择一个会话开始聊天"));
}

void chat_widget::update_pinned_message()
{
    auto const item = conversation(active_conversation_);
    auto const visible = active_group_ && item && item->pinned_message.id > 0;
    pinned_message_button_->setVisible(visible);
    unpin_message_button_->setVisible(visible && messages_->can_manage_group());
    pinned_message_button_->setEnabled(connection_available_);
    unpin_message_button_->setEnabled(connection_available_);
    if (visible)
    {
        auto const summary = QStringLiteral("置顶消息 · %1：%2")
            .arg(item->pinned_message.username, grapheme_prefix(item->pinned_message.text.simplified(), 80));
        pinned_message_button_->setText(QString(summary).replace(QLatin1Char('&'), QStringLiteral("&&")));
        pinned_message_button_->setToolTip(summary + QStringLiteral("\n点击定位；较早的消息通过搜索查看。"));
    }
}

void chat_widget::set_members(qint64 conversation, QList<member_data> members, QString const& error)
{
    if (conversation != active_conversation_ || !error.isEmpty())
    {
        return;
    }
    for (auto const& member : members) { avatars_.observe(member.id, member.avatar); }
    messages_->set_members(members);
    update_pinned_message();
    for (auto it = typing_users_.begin(); it != typing_users_.end();)
    {
        auto const present = std::any_of(members.begin(), members.end(), [id = it.key()](auto const& member) {
            return member.id == id;
        });
        it = present ? std::next(it) : typing_users_.erase(it);
    }
    update_typing_label();
}

void chat_widget::show_read_details(qint64 conversation, qint64 message)
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("readDetailsDialog"));
    dialog.setWindowTitle(QStringLiteral("已读详情"));
    dialog.resize(chat_theme::dialog_small_width, 360);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                              chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    auto* count = new QLabel(&dialog);
    count->setObjectName(QStringLiteral("readDetailsCount"));
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("readMembersList"));
    list->setAccessibleName(QStringLiteral("已读成员"));
    list->setItemDelegate(new user_delegate(list));
    list->setUniformItemSizes(true);
    list->setFrameShape(QFrame::NoFrame);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setMouseTracking(true);
    layout->addWidget(count);
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    buttons->button(QDialogButtonBox::Close)->setIcon({});
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto refresh = [this, &dialog, conversation, message, count, list] {
        bool visible = false;
        for (int row = 0; row < messages_->rowCount(); ++row)
        {
            auto const item = messages_->index(row, 0);
            if (item.data(message_model::id_role).toLongLong() == message)
            {
                visible = item.data(message_model::read_count_role).isValid();
                break;
            }
        }
        if (active_conversation_ != conversation || !visible)
        {
            dialog.reject();
            return false;
        }
        auto const members = messages_->read_members(message);
        count->setText(QStringLiteral("已读 %1 人").arg(members.size()));
        auto const selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : 0;
        auto const scroll = list->verticalScrollBar()->value();
        list->clear();
        for (auto const& member : members)
        {
            auto* item = new QListWidgetItem(member.username, list);
            item->setData(Qt::UserRole, member.id);
            item->setData(Qt::StatusTipRole, QStringLiteral("已读"));
            item->setData(Qt::DecorationRole, avatars_.image(member.id));
            item->setToolTip(member.username);
            if (member.id == selected) { list->setCurrentItem(item); }
        }
        list->doItemsLayout();
        list->verticalScrollBar()->setValue(scroll);
        return true;
    };
    connect(messages_, &QAbstractItemModel::dataChanged, &dialog, refresh);
    connect(messages_, &QAbstractItemModel::modelReset, &dialog, refresh);
    connect(&avatars_, &avatar_cache::changed, &dialog, refresh);
    if (!refresh()) { return; }
    dialog.exec();
}

std::optional<qint64> chat_widget::recovery_cursor() const
{
    return messages_loaded_ ? std::optional<qint64>(std::max<qint64>(0, messages_->first_message_id() - 1))
                            : std::nullopt;
}

bool chat_widget::messages_ready() const
{
    return messages_loaded_ && !messages_loading_;
}

bool chat_widget::viewing_latest() const
{
    auto const* scroll = messages_view_->verticalScrollBar();
    return connection_available_ && messages_ready() && messages_view_->isVisible() &&
        window()->isActiveWindow() && !window()->isMinimized() &&
        !QApplication::activeModalWidget() && !QApplication::activePopupWidget() && scroll->value() == scroll->maximum();
}

void chat_widget::mark_visible_messages()
{
    auto const message = messages_->last_message_id();
    if (active_conversation_ > 0 && viewing_latest() && message > messages_->read_position(self_user_))
    {
        emit read_requested(active_conversation_, message);
    }
}

std::optional<conversation_data> chat_widget::conversation(qint64 id) const
{
    if (auto const* item = conversations_->conversation_at(conversations_->index_for_conversation(id))) { return *item; }
    return std::nullopt;
}

void chat_widget::set_conversation_muted(qint64 conversation, bool muted)
{
    conversations_->set_muted(conversation, muted);
}

void chat_widget::set_conversation_pinned(qint64 conversation, bool pinned)
{
    conversations_->set_pinned(conversation, pinned);
}

void chat_widget::create_group()
{
    if (!connection_available_) { return; }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("createGroupDialog"));
    dialog.setWindowTitle(QStringLiteral("创建群聊"));
    dialog.resize(chat_theme::dialog_normal_width, 540);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                              chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    auto* steps = new QStackedWidget(&dialog);
    auto* pick_page = new QWidget(steps);
    pick_page->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    auto* pick_layout = new QVBoxLayout(pick_page);
    pick_layout->setContentsMargins(0, 0, 0, 0);
    pick_layout->setSpacing(chat_theme::dialog_spacing);
    auto* search = new QLineEdit(pick_page);
    search->setObjectName(QStringLiteral("groupContactSearch"));
    search->setPlaceholderText(QStringLiteral("搜索好友"));
    pick_layout->addWidget(search);
    auto* selected_count = new QLabel(pick_page);
    selected_count->setObjectName(QStringLiteral("groupSelectedCount"));
    pick_layout->addWidget(selected_count);
    auto* selected = new QListWidget(pick_page);
    selected->setObjectName(QStringLiteral("groupSelectedContacts"));
    selected->setTextElideMode(Qt::ElideRight);
    selected->setFlow(QListView::LeftToRight);
    selected->setWrapping(true);
    selected->setResizeMode(QListView::Adjust);
    pick_layout->addWidget(selected);
    auto* list = new QListWidget(pick_page);
    list->setObjectName(QStringLiteral("groupContactPicker"));
    list->setIconSize(QSize(32, 32));
    list->setAccessibleName(QStringLiteral("选择群成员"));
    pick_layout->addWidget(list, 1);
    steps->addWidget(pick_page);
    auto* name_page = new QWidget(steps);
    auto* name_layout = new QVBoxLayout(name_page);
    name_layout->setContentsMargins(0, 0, 0, 0);
    name_layout->setSpacing(chat_theme::dialog_spacing);
    name_layout->addWidget(new QLabel(QStringLiteral("群名称"), name_page));
    auto* title = new QLineEdit(name_page);
    title->setObjectName(QStringLiteral("newGroupTitleEdit"));
    title->setPlaceholderText(QStringLiteral("群名称"));
    title->setAccessibleName(QStringLiteral("群名称"));
    name_layout->addWidget(title);
    auto* summary = new QLabel(name_page);
    summary->setObjectName(QStringLiteral("groupNamingSummary"));
    name_layout->addWidget(summary);
    auto* member_list = new QListWidget(name_page);
    member_list->setObjectName(QStringLiteral("groupNamingMembers"));
    member_list->setIconSize(QSize(32, 32));
    member_list->setSelectionMode(QAbstractItemView::NoSelection);
    member_list->setAccessibleName(QStringLiteral("已选群成员"));
    name_layout->addWidget(member_list, 1);
    steps->addWidget(name_page);
    layout->addWidget(steps, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* previous = buttons->addButton(QStringLiteral("上一步"), QDialogButtonBox::ActionRole);
    previous->setObjectName(QStringLiteral("groupPreviousButton"));
    previous->hide();
    auto* proceed = buttons->button(QDialogButtonBox::Ok);
    proceed->setObjectName(QStringLiteral("groupNextButton"));
    proceed->setText(QStringLiteral("下一步"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    for (auto* button : buttons->buttons())
    {
        button->setIcon({});
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
    layout->addWidget(buttons);

    auto update = [list, selected, selected_count, title, summary, member_list, steps, proceed] {
        QSignalBlocker blocked(selected);
        selected->clear();
        member_list->clear();
        QStringList names;
        for (int row = 0; row < list->count(); ++row)
        {
            auto* item = list->item(row);
            if (item->checkState() != Qt::Checked) { continue; }
            auto* chip = new QListWidgetItem(item->text() + QStringLiteral(" ×"), selected);
            chip->setData(Qt::UserRole, item->data(Qt::UserRole));
            chip->setToolTip(item->text());
            chip->setSizeHint(QSize(210, 32));
            auto* member = new QListWidgetItem(item->icon(), item->text(), member_list);
            member->setToolTip(item->text());
            member->setSizeHint(QSize(0, 44));
            names.push_back(item->text());
        }
        selected_count->setText(QStringLiteral("已选 %1 位好友 · 点击姓名取消").arg(names.size()));
        selected->setVisible(!names.empty());
        selected->setFixedHeight(names.size() <= 2 ? 40 : 76);
        summary->setText(QStringLiteral("已选 %1 位好友").arg(names.size()));
        member_list->setFixedHeight(std::clamp(static_cast<int>(names.size()) * 44 + 4, 48, 136));
        proceed->setEnabled(!names.empty() && (steps->currentIndex() == 0 ||
            chat::valid_group_title(title->text().toUtf8().toStdString())));
    };
    auto populate = [this, list, search, update] {
        QSet<qint64> chosen;
        for (int row = 0; row < list->count(); ++row)
        {
            if (list->item(row)->checkState() == Qt::Checked) { chosen.insert(list->item(row)->data(Qt::UserRole).toLongLong()); }
        }
        {
            QSignalBlocker blocked(list);
            list->clear();
            for (int row = 0; row < contacts_->rowCount(); ++row)
            {
                auto const* user = contacts_->user_at(contacts_->index(row, 0));
                if (!user || user->id == self_user_) { continue; }
                auto* item = new QListWidgetItem(avatar_icon(user->username, 32, avatars_.image(user->id)), user->username, list);
                item->setData(Qt::UserRole, user->id);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(chosen.contains(user->id) ? Qt::Checked : Qt::Unchecked);
                item->setToolTip(user->username);
                item->setSizeHint(QSize(0, 50));
                item->setHidden(!user->username.contains(search->text(), Qt::CaseInsensitive));
            }
        }
        update();
    };
    connect(contacts_, &QAbstractItemModel::modelReset, &dialog, populate);
    connect(list, &QListWidget::itemChanged, &dialog, [update](QListWidgetItem*) { update(); });
    connect(selected, &QListWidget::itemClicked, &dialog, [list](QListWidgetItem* chip) {
        auto const id = chip->data(Qt::UserRole).toLongLong();
        for (int row = 0; row < list->count(); ++row)
        {
            auto* item = list->item(row);
            if (item->data(Qt::UserRole).toLongLong() == id) { item->setCheckState(Qt::Unchecked); break; }
        }
    });
    connect(search, &QLineEdit::textChanged, &dialog, [list](QString const& query) {
        for (int row = 0; row < list->count(); ++row)
        { list->item(row)->setHidden(!list->item(row)->text().contains(query, Qt::CaseInsensitive)); }
    });
    connect(title, &QLineEdit::textChanged, &dialog, [update] { update(); });
    connect(previous, &QPushButton::clicked, &dialog, [&dialog, steps, proceed, previous, search, update] {
        steps->setCurrentIndex(0); previous->hide(); proceed->setText(QStringLiteral("下一步"));
        dialog.resize(chat_theme::dialog_normal_width, 540); search->setFocus(); update();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, steps, proceed, previous, title, update] {
        if (steps->currentIndex() == 0)
        {
            steps->setCurrentIndex(1); previous->show(); proceed->setText(QStringLiteral("创建"));
            update(); dialog.resize(chat_theme::dialog_normal_width, 300); title->setFocus();
        }
        else if (chat::valid_group_title(title->text().toUtf8().toStdString())) { dialog.accept(); }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    populate();
    if (dialog.exec() != QDialog::Accepted || !connection_available_) { return; }
    QList<qint64> members;
    for (int row = 0; row < list->count(); ++row)
    {
        auto* item = list->item(row);
        auto const id = item->data(Qt::UserRole).toLongLong();
        if (item->checkState() == Qt::Checked && is_contact(id)) { members.push_back(id); }
    }
    if (!members.empty()) { emit group_create_requested(title->text(), std::move(members)); }
}

void chat_widget::request_older_messages()
{
    if (!connection_available_ || active_conversation_ <= 0 || !messages_loaded_ || messages_loading_ ||
        history_exhausted_)
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
    emit older_messages_requested(active_conversation_, before);
}

void chat_widget::send_current_message()
{
    if (!can_send() || message_sending_.contains(active_conversation_) || message_edit_->toPlainText().isEmpty())
    {
        return;
    }

    auto text = message_edit_->toPlainText();
    stop_typing();
    message_sending_.insert(active_conversation_);
    update_compose_state();
    set_message_status(QStringLiteral("正在发送…"));
    auto const reply = reply_to_;
    emit send_message_requested(active_conversation_, std::move(text), reply);
}

void chat_widget::stop_typing()
{
    if (typing_idle_timer_->isActive())
    {
        typing_idle_timer_->stop();
        if (can_send()) { emit typing_requested(active_conversation_, false); }
    }
}

void chat_widget::set_typing(qint64 conversation, qint64 user, QString username, bool typing)
{
    if (!connection_available_ || conversation != active_conversation_ || user == self_user_)
    {
        return;
    }
    if (typing)
    {
        typing_users_.insert(user, {std::move(username), QDeadlineTimer(5000)});
    }
    else
    {
        typing_users_.remove(user);
    }
    update_typing_label();
}

void chat_widget::update_typing_label()
{
    QStringList names;
    for (auto const& user : typing_users_)
    {
        names.push_back(user.username);
    }
    names.sort();
    typing_label_->setText(names.isEmpty() ? QString{} : names.mid(0, 3).join(QStringLiteral("、")) +
        (names.size() > 3 ? QStringLiteral("等人正在输入…") : QStringLiteral("正在输入…")));
    typing_label_->setVisible(!names.isEmpty());
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
    dialog.setFixedWidth(520);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QFrame(&dialog);
    header->setObjectName(QStringLiteral("profileHeaderSection"));
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(24, 16, 24, 24);
    header_layout->setSpacing(12);

    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->addStretch();
    auto* close_button = new QToolButton(header);
    close_button->setObjectName(QStringLiteral("profileCloseButton"));
    close_button->setAccessibleName(QStringLiteral("关闭资料"));
    close_button->setToolTip(QStringLiteral("关闭资料"));
    close_button->setIcon(svg_icon(QStringLiteral("close"), QColor(QStringLiteral("#3F4542")), QSize(22, 22)));
    close_button->setIconSize(QSize(22, 22));
    close_button->setFixedSize(36, 36);
    close_button->setCursor(Qt::PointingHandCursor);
    top->addWidget(close_button);
    header_layout->addLayout(top);

    auto* avatar = new QLabel(header);
    avatar->setObjectName(QStringLiteral("profileDialogAvatar"));
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(104, 104);
    avatar->setPixmap(avatar_icon(username, 104, avatars_.image(user)).pixmap(104, 104));
    connect(&avatars_, &avatar_cache::changed, &dialog, [this, avatar, user, username](qint64 changed) {
        if (changed == user) { avatar->setPixmap(avatar_icon(username, 104, avatars_.image(user)).pixmap(104, 104)); }
    });
    header_layout->addWidget(avatar, 0, Qt::AlignHCenter);

    auto* name = new QLabel(username, header);
    name->setObjectName(QStringLiteral("profileDialogName"));
    name->setAlignment(Qt::AlignCenter);
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    header_layout->addWidget(name);
    auto* relationship = new QLabel(header);
    relationship->setObjectName(QStringLiteral("profileRelationship"));
    relationship->setAlignment(Qt::AlignCenter);
    relationship->setWordWrap(true);
    header_layout->addWidget(relationship);

    header_layout->addSpacing(4);

    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(12);
    actions->addStretch();
    auto make_action = [header](QString text, QStringView icon, QColor const& color) {
        auto* button = new QToolButton(header);
        button->setObjectName(QStringLiteral("profileActionButton"));
        button->setText(std::move(text));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIcon(svg_icon(icon, color, QSize(20, 20)));
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(160, 40);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };
    auto* message_button = make_action(QStringLiteral("消息"), QStringLiteral("chat"), QColor(Qt::white));
    message_button->setProperty("primary", true);
    auto* copy_username_button = make_action(QStringLiteral("复制用户名"), QStringLiteral("copy"), QColor(QStringLiteral("#315A4B")));
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
    info_layout->setContentsMargins(24, 16, 24, 24);
    info_layout->setSpacing(12);

    QPushButton* remove_contact_button = nullptr;
    if (user != self_user_)
    {
        auto* remove_button = new QPushButton(QStringLiteral("移除联系人"), info);
        remove_contact_button = remove_button;
        remove_button->setObjectName(QStringLiteral("removeContactButton"));
        remove_button->setEnabled(connection_available_);
        info_layout->addWidget(remove_button);
        connect(remove_button, &QPushButton::clicked, &dialog, [this, &dialog, user, username] {
            if (!confirm_action(&dialog, QStringLiteral("移除联系人"),
                QStringLiteral("删除与 %1 的好友关系？双方将无法继续发送新消息，聊天记录和群成员资格会保留。").arg(username),
                QStringLiteral("移除联系人")))
            {
                return;
            }
            dialog.accept();
            emit contact_remove_requested(user);
        });
    }
    auto* contact_status = new QLabel(info);
    contact_status->setObjectName(QStringLiteral("profileContactStatus"));
    contact_status->setWordWrap(true);
    info_layout->addWidget(contact_status);
    auto* reject_request = new QPushButton(QStringLiteral("拒绝申请"), info);
    reject_request->setObjectName(QStringLiteral("rejectFriendRequestButton"));
    auto* cancel_request = new QPushButton(QStringLiteral("取消申请"), info);
    cancel_request->setObjectName(QStringLiteral("cancelFriendRequestButton"));
    info_layout->addWidget(reject_request);
    info_layout->addWidget(cancel_request);
    auto update_contact = [this, user, relationship, message_button, remove_contact_button, reject_request, cancel_request] {
        auto const state = friend_state(user);
        if (user == self_user_) { relationship->setText(QStringLiteral("我的账号")); }
        else if (state == chat::friendship_state::accepted)
        {
            auto const presence = presence_.value(user);
            auto const text = presence_text(presence.online, presence.last_seen);
            relationship->setText(text.isEmpty() ? QStringLiteral("已是好友") : text);
        }
        else
        {
            relationship->setText(state == chat::friendship_state::incoming_pending ? QStringLiteral("对方向你发送了好友申请")
                : state == chat::friendship_state::outgoing_pending ? QStringLiteral("已发出申请，等待对方确认")
                : QStringLiteral("添加好友后可发送消息"));
        }
        message_button->setVisible(user != self_user_);
        message_button->setText(state == chat::friendship_state::accepted ? QStringLiteral("消息") : state == chat::friendship_state::incoming_pending ? QStringLiteral("接受申请")
            : state == chat::friendship_state::outgoing_pending ? QStringLiteral("等待验证") : QStringLiteral("添加好友"));
        message_button->setEnabled(connection_available_ && state != chat::friendship_state::outgoing_pending);
        if (remove_contact_button) { remove_contact_button->setVisible(state == chat::friendship_state::accepted); remove_contact_button->setEnabled(connection_available_); }
        reject_request->setVisible(user != self_user_ && state == chat::friendship_state::incoming_pending);
        cancel_request->setVisible(user != self_user_ && state == chat::friendship_state::outgoing_pending);
        reject_request->setEnabled(connection_available_);
        cancel_request->setEnabled(connection_available_);
    };
    update_contact();
    connect(contacts_, &QAbstractItemModel::modelReset, &dialog, update_contact);
    connect(contacts_, &QAbstractItemModel::dataChanged, &dialog, update_contact);
    connect(this, &chat_widget::friendship_updated, &dialog, update_contact);
    connect(reject_request, &QPushButton::clicked, &dialog, [this, user, reject_request, message_button] {
        if (!connection_available_ || friend_state(user) != chat::friendship_state::incoming_pending) { return; }
        reject_request->setEnabled(false); message_button->setEnabled(false);
        emit friend_request_respond_requested(user, false);
    });
    connect(cancel_request, &QPushButton::clicked, &dialog, [this, user, cancel_request] {
        if (!connection_available_ || friend_state(user) != chat::friendship_state::outgoing_pending) { return; }
        cancel_request->setEnabled(false);
        emit friend_request_cancel_requested(user);
    });
    connect(this, &chat_widget::contact_add_finished, &dialog,
        [user, contact_status, update_contact](qint64 changed, QString error) {
            if (changed != user) { return; }
            contact_status->setText(error);
            if (!error.isEmpty()) { update_contact(); }
        });
    if (user == self_user_)
    {
        message_button->hide();
        auto* change = new QPushButton(QStringLiteral("更换头像"), info);
        change->setObjectName(QStringLiteral("changeAvatarButton"));
        auto* remove = new QPushButton(QStringLiteral("移除头像"), info);
        remove->setObjectName(QStringLiteral("removeAvatarButton"));
        auto* status = new QLabel(info);
        status->setObjectName(QStringLiteral("avatarUploadStatus"));
        status->setWordWrap(true);
        info_layout->addWidget(change);
        info_layout->addWidget(remove);
        info_layout->addWidget(status);
        auto update = [this, user, change, remove, status] {
            change->setEnabled(connection_available_ && !avatar_updating_);
            remove->setEnabled(connection_available_ && !avatar_updating_ && avatars_.state(user).present);
            if (avatar_updating_) { status->setText(QStringLiteral("正在更新头像…")); }
        };
        update();
        connect(&avatars_, &avatar_cache::changed, &dialog, [update](qint64) { update(); });
        connect(this, &chat_widget::avatar_update_finished, &dialog, [status, update](QString error) {
            update();
            status->setText(error);
        });
        connect(change, &QPushButton::clicked, &dialog, [this, &dialog, status, update] {
            auto const path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("更换头像"), {},
                                                        QStringLiteral("图片 (*.png *.jpg *.jpeg)"));
            if (path.isEmpty()) { return; }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > static_cast<qint64>(chat::max_avatar_size))
            {
                status->setText(QStringLiteral("无法读取图片，头像文件不得超过 1 MiB。"));
                return;
            }
            auto bytes = file.readAll();
            if (decode_avatar(bytes).isNull())
            {
                status->setText(QStringLiteral("请选择完整的 PNG 或 JPEG 图片，最多 1600 万像素。"));
                return;
            }
            avatar_updating_ = true;
            update();
            emit avatar_set_requested(std::move(bytes));
        });
        connect(remove, &QPushButton::clicked, &dialog, [this, update] {
            avatar_updating_ = true;
            update();
            emit avatar_clear_requested();
        });
        auto* logout = new QPushButton(QStringLiteral("退出登录"), info);
        logout->setObjectName(QStringLiteral("profileLogoutButton"));
        info_layout->addWidget(logout);
        connect(logout, &QPushButton::clicked, &dialog, [this, &dialog] {
            if (!confirm_action(&dialog, QStringLiteral("退出登录"),
                QStringLiteral("退出当前账号？应用会返回登录页。"),
                QStringLiteral("退出登录"))) { return; }
            dialog.accept();
            emit logout_requested();
        });
    }
    layout->addWidget(info);

    connect(close_button, &QToolButton::clicked, &dialog, &QDialog::reject);
    connect(message_button, &QToolButton::clicked, &dialog, [this, &dialog, user, username, message_button, contact_status] {
        if (!connection_available_ || user == self_user_) { return; }
        if (is_contact(user)) { dialog.accept(); open_chat(user, username); }
        else if (friend_state(user) != chat::friendship_state::outgoing_pending)
        {
            message_button->setEnabled(false);
            message_button->setText(QStringLiteral("正在处理…"));
            contact_status->clear();
            if (friend_state(user) == chat::friendship_state::incoming_pending) { emit friend_request_respond_requested(user, true); }
            else { emit contact_add_requested(user); }
        }
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
    chat_title_->setIcon(avatar_icon(username, 38, active_group_ ? QPixmap{} : avatars_.image(active_peer_)));
    chat_title_->setEnabled(true);
    update_chat_presence();
}

void chat_widget::update_chat_presence()
{
    if (active_conversation_ <= 0)
    {
        chat_presence_->clear();
        chat_presence_->hide();
        return;
    }

    if (active_group_)
    {
        chat_presence_->setObjectName(QStringLiteral("chatPresence"));
        chat_presence_->setText(QStringLiteral("%1 名成员 · 点击群名称查看").arg(active_member_count_));
        chat_presence_->show();
        return;
    }
    auto const presence = presence_.value(active_peer_);
    auto const text = presence_text(presence.online, presence.last_seen);
    if (text.isEmpty())
    {
        chat_presence_->clear();
        chat_presence_->hide();
        return;
    }

    chat_presence_->setObjectName(
        presence.online ? QStringLiteral("chatPresenceOnline") : QStringLiteral("chatPresence"));
    chat_presence_->setText(text);
    chat_presence_->style()->unpolish(chat_presence_);
    chat_presence_->style()->polish(chat_presence_);
    chat_presence_->show();
}

void chat_widget::finish_avatar_update(QString error)
{
    avatar_updating_ = false;
    emit avatar_update_finished(std::move(error));
}
