#include <QShortcut>
#include "chat_widget.hpp"
#include "sidebar_search.hpp"
#include "friend_list_model.hpp"
#include "download_tasks.hpp"
#include "message_card.hpp"
#include "theme_manager.hpp"
#include <chat/invite.hpp>
#include <chat/text.hpp>

#include <utility>
#include <iterator>
#include <limits>
#include <algorithm>

#include <QAbstractItemView>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QDesktopServices>
#include <QGridLayout>
#include <QComboBox>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QMimeData>
#include <QSettings>
#include <QDrag>
#include <QMouseEvent>
#include <QUrl>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QListWidget>
#include <QFrame>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QMenu>
#include <QInputDialog>
#include <QInputMethod>
#include <QModelIndex>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabWidget>
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
#include <chat/client.hpp>
#include <chat/reaction.hpp>

#include "clipboard_image.hpp"
#include "screenshot_dialog.hpp"
#include "avatar.hpp"
#include "conversation_delegate.hpp"
#include "conversation_model.hpp"
#include "create_group_dialog.hpp"
#include "emoji_text.hpp"
#include "icons.hpp"
#include "message_delegate.hpp"
#include "message_list_view.hpp"
#include "message_model.hpp"
#include "presence.hpp"
#include "theme.hpp"
#include "user_delegate.hpp"
#include "user_model.hpp"

namespace
{

QString local_file_path(QUrl url)
{
    if (!url.isLocalFile())
    {
        return {};
    }
    if (url.host() == QStringLiteral("localhost"))
    {
        url.setHost({});
    }
    return url.host().isEmpty() ? url.toLocalFile() : QString{};
}

QIcon composer_send_icon()
{
    constexpr QSize size{20, 20};
    auto icon = svg_icon(u"send", themed("accent"), size);
    icon.addPixmap(svg_icon(u"send", themed("text_muted"), size).pixmap(size), QIcon::Disabled);
    return icon;
}

class conversation_list_view final : public QListView
{
   public:
    using QListView::QListView;
    std::function<void(qint64, qint64)> move_to_folder;

   protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        auto* rooms = static_cast<conversation_model*>(model());
        auto pos = event->position().toPoint();
        auto const* item = rooms->conversation_at(indexAt(pos));
        drag_conversation_ = event->button() == Qt::LeftButton && !rooms->folders().isEmpty() && item ? item->id : 0;
        drag_origin_ = pos;
        QListView::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!drag_conversation_ || !(event->buttons() & Qt::LeftButton))
        {
            QListView::mouseMoveEvent(event);
            return;
        }
        if ((event->position().toPoint() - drag_origin_).manhattanLength() < QApplication::startDragDistance())
        {
            return;
        }
        // A sidebar refresh may reset the current index between the press and the drag.
        QDrag drag(this);
        auto* mime = new QMimeData;
        mime->setData("application/x-chat-conversation", QByteArray::number(std::exchange(drag_conversation_, 0)));
        drag.setMimeData(mime);
        drag.exec(Qt::MoveAction);
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        drag_conversation_ = 0;
        QListView::mouseReleaseEvent(event);
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->source() == this)
        {
            e->acceptProposedAction();
        }
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (e->source() == this && indexAt(e->position().toPoint()).isValid())
        {
            e->acceptProposedAction();
        }
    }
    void dropEvent(QDropEvent* e) override
    {
        auto target = indexAt(e->position().toPoint());
        if (e->source() != this || !target.isValid())
        {
            return;
        }
        move_to_folder(e->mimeData()->data("application/x-chat-conversation").toLongLong(),
                       target.data(conversation_model::folder_role).toLongLong());
        e->acceptProposedAction();
    }

   private:
    qint64 drag_conversation_ = 0;
    QPoint drag_origin_;
};

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
        paint_emoji_line(
            painter, contents, displayed, font(), option.palette.color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText));
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

    auto* conversation_panel = create_sidebar();
    timeline_ = create_chat_panel();
    layout->addWidget(conversation_panel);
    layout->addWidget(make_separator(this));
    layout->addWidget(timeline_, 1);
    connect_chat_controls();
}

void chat_widget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layout_poll_results();
}

QWidget* chat_widget::create_sidebar()
{
    auto* conversation_panel = new QFrame(this);
    conversation_panel->setObjectName(QStringLiteral("conversationPanel"));
    conversation_panel->setFixedWidth(332);
    auto* conversation_layout = new QVBoxLayout(conversation_panel);
    conversation_layout->setContentsMargins(0, 0, 0, 0);
    conversation_layout->setSpacing(0);

    auto* conversation_header = new QFrame(conversation_panel);
    conversation_header->setObjectName(QStringLiteral("conversationHeader"));
    auto* conversation_header_layout = new QHBoxLayout(conversation_header);
    conversation_header_layout->setContentsMargins(12, 9, 13, 9);
    conversation_header_layout->setSpacing(8);
    main_menu_button_ = new QToolButton(conversation_header);
    main_menu_button_->setObjectName(QStringLiteral("mainMenuButton"));
    main_menu_button_->setProperty("navigationTool", true);
    main_menu_button_->setAccessibleName(QStringLiteral("主菜单"));
    main_menu_button_->setToolTip(QStringLiteral("主菜单"));
    main_menu_button_->setIcon(svg_icon(u"menu", themed("text_secondary"), QSize(22, 22)));
    main_menu_button_->setIconSize(QSize(22, 22));
    main_menu_button_->setFixedSize(40, 40);
    main_menu_button_->setCursor(Qt::PointingHandCursor);
    main_menu_button_->setPopupMode(QToolButton::InstantPopup);
    auto* main_menu = new QMenu(main_menu_button_);
    main_menu->setObjectName(QStringLiteral("mainNavigationMenu"));
    main_menu->setAccessibleName(QStringLiteral("主菜单"));
    resume_reading_after_menu(*main_menu);
    chats_action_ = main_menu->addAction(QStringLiteral("聊天"));
    chats_action_->setObjectName(QStringLiteral("showChatsAction"));
    chats_action_->setIcon(svg_icon(u"chat", themed("text"), QSize(20, 20)));
    contacts_action_ = main_menu->addAction(QStringLiteral("好友"));
    contacts_action_->setObjectName(QStringLiteral("showContactsAction"));
    contacts_action_->setIcon(svg_icon(u"contacts", themed("text"), QSize(20, 20)));
    chats_action_->setCheckable(true);
    main_menu->addSeparator();
    auto* search_messages = new QAction(QStringLiteral("全局消息搜索"), this);
    search_messages->setObjectName(QStringLiteral("globalSearchAction"));
    search_messages->setEnabled(connection_available_);
    connect(search_messages, &QAction::triggered, this, [this] { emit global_search_requested(conversation_search_->text()); });
    search_messages->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    search_messages->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(search_messages);
    message_search_action_ = new QAction(QStringLiteral("当前会话搜索"), this);
    message_search_action_->setObjectName(QStringLiteral("messageSearchAction"));
    message_search_action_->setShortcut(QKeySequence::Find);
    message_search_action_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    message_search_action_->setEnabled(false);
    addAction(message_search_action_);
    auto* add_button = new QToolButton(conversation_header);
    add_button->setObjectName(QStringLiteral("sidebarAddButton"));
    add_button->setProperty("navigationTool", true);
    add_button->setAccessibleName(QStringLiteral("创建或添加"));
    add_button->setToolTip(add_button->accessibleName());
    add_button->setFixedSize(36, 36);
    add_button->setPopupMode(QToolButton::InstantPopup);
    auto* add_menu = new QMenu(add_button);
    resume_reading_after_menu(*add_menu);
    add_button->setMenu(add_menu);
    auto* create_group = add_menu->addAction(QStringLiteral("创建群聊"));
    create_group->setObjectName(QStringLiteral("createGroupAction"));
    create_group->setIcon(svg_icon(u"groups", themed("text"), QSize(20, 20)));
    auto* add_friend = add_menu->addAction(QStringLiteral("添加好友"));
    add_friend->setObjectName(QStringLiteral("addFriendAction"));
    add_friend->setIcon(svg_icon(u"user-plus", themed("text"), QSize(20, 20)));
    auto* join_group = add_menu->addAction(QStringLiteral("加入群聊"));
    join_group->setObjectName(QStringLiteral("joinGroupAction"));
    join_group->setIcon(svg_icon(u"groups", themed("text"), QSize(20, 20)));
    self_profile_action_ = main_menu->addAction(QStringLiteral("我的资料"));
    self_profile_action_->setObjectName(QStringLiteral("selfProfileAction"));
    auto* settings = main_menu->addAction(QStringLiteral("设置"));
    settings->setObjectName(QStringLiteral("showSettingsAction"));
    settings->setToolTip(QStringLiteral("设置"));
    settings->setIcon(svg_icon(u"settings", themed("text"), QSize(20, 20)));
    connect(settings, &QAction::triggered, this, &chat_widget::settings_requested);
    main_menu_button_->setMenu(main_menu);
    main_menu_badge_ = new QLabel(main_menu_button_);
    main_menu_badge_->setObjectName(QStringLiteral("mainMenuBadge"));
    main_menu_badge_->setAlignment(Qt::AlignCenter);
    main_menu_badge_->setAttribute(Qt::WA_TransparentForMouseEvents);
    main_menu_badge_->hide();
    conversation_header_layout->addWidget(main_menu_button_);
    // Chats lead with search beside the global menu.
    conversation_search_ = new QLineEdit(conversation_header);
    conversation_search_->setObjectName(QStringLiteral("conversationSearchEdit"));
    conversation_search_->setPlaceholderText(QStringLiteral("搜索"));
    conversation_search_->setClearButtonEnabled(true);
    conversation_search_->setAccessibleName(QStringLiteral("搜索聊天"));
    style_search_field(*conversation_search_);
    conversation_header_layout->addWidget(conversation_search_, 1);
    conversation_header_layout->addWidget(add_button);
    auto recolor = [add_button] { add_button->setIcon(svg_icon(u"plus", themed("text_secondary"), QSize(20, 20))); };
    recolor();
    connect(&theme_manager::instance(), &theme_manager::changed, add_button, recolor);
    conversation_layout->addWidget(conversation_header);

    auto* conversations_page = new QWidget(conversation_panel);
    conversations_page->setObjectName(QStringLiteral("sidebarRooms"));
    auto* conversations_layout = new QVBoxLayout(conversations_page);
    conversations_layout->setContentsMargins(0, 0, 0, 0);
    conversations_layout->setSpacing(0);
    conversations_status_ = new QLabel(conversations_page);
    conversations_status_->setObjectName(QStringLiteral("subtleText"));
    conversations_status_->setContentsMargins(18, 0, 14, 6);
    conversations_status_->hide();
    conversations_layout->addWidget(conversations_status_);

    conversations_ = new conversation_model(this, &avatars_);
    auto* list = new conversation_list_view(conversations_page);
    conversations_view_ = list;
    list->setDragDropMode(QAbstractItemView::DropOnly);
    list->move_to_folder = [this](qint64 id, qint64 folder)
    {
        if (connection_available_)
        {
            emit move_conversation_to_folder(id, folder);
        }
    };
    conversations_view_->setObjectName(QStringLiteral("conversationList"));
    conversations_view_->setAccessibleName(QStringLiteral("会话列表"));
    conversations_view_->setModel(conversations_);
    mute_expiry_timer_ = new QTimer(this);
    mute_expiry_timer_->setSingleShot(true);
    connect(mute_expiry_timer_,
            &QTimer::timeout,
            this,
            [this]
            {
                conversations_view_->viewport()->update();
                update_main_menu();
            });
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
    setup_contacts_dialog();
    setup_add_contact_dialog();
    setup_friend_requests();
    notice_ = new feedback_label(conversation_panel);
    notice_->setObjectName(QStringLiteral("sidebarNotice"));
    notice_->setWordWrap(true);
    notice_->hide();
    conversation_layout->addWidget(notice_);
    notice_timer_ = new QTimer(this);
    notice_timer_->setSingleShot(true);
    notice_timer_->setInterval(6000);
    connect(notice_timer_,
            &QTimer::timeout,
            this,
            [this]
            {
                notice_->clear();
                notice_->hide();
                contacts_feedback_->clear();
                contacts_feedback_->hide();
            });
    // Loading is usually instant; saying so only when it is not avoids a flash on every switch.
    loading_status_timer_ = new QTimer(this);
    loading_status_timer_->setSingleShot(true);
    loading_status_timer_->setInterval(400);
    connect(loading_status_timer_,
            &QTimer::timeout,
            this,
            [this]
            {
                if (active_conversation_ > 0 && messages_loading_ && !messages_loaded_)
                {
                    set_message_status(QStringLiteral("正在加载消息…"));
                }
            });
    conversation_layout->addWidget(conversations_page, 1);
    connect(create_group, &QAction::triggered, this, [this] { this->create_group(); });
    connect(add_friend, &QAction::triggered, this, [this] { show_add_contact_dialog(); });
    connect(join_group,
            &QAction::triggered,
            this,
            [this]
            {
                QInputDialog dialog(this);
                dialog.setObjectName(QStringLiteral("joinGroupDialog"));
                dialog.setWindowTitle(QStringLiteral("加入群聊"));
                dialog.setLabelText(QStringLiteral("输入群主或管理员分享的邀请码"));
                dialog.setInputMode(QInputDialog::TextInput);
                dialog.setOkButtonText(QStringLiteral("加入"));
                dialog.setCancelButtonText(QStringLiteral("取消"));
                dialog.layout()->setContentsMargins(
                    chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
                dialog.layout()->setSpacing(chat_theme::dialog_spacing);
                frame_dialog(dialog, false);
                auto* buttons = dialog.findChild<QDialogButtonBox*>();
                buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("joinGroupButton"));
                buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
                for (auto* button : buttons->buttons())
                {
                    button->setIcon({});
                    button->style()->unpolish(button);
                    button->style()->polish(button);
                }
                auto* input = dialog.findChild<QLineEdit*>();
                input->setMinimumWidth(chat_theme::dialog_small_width - 2 * chat_theme::dialog_padding);
                input->setPlaceholderText(QStringLiteral("输入邀请码"));
                input->setAccessibleName(QStringLiteral("邀请码"));
                auto* ok = buttons->button(QDialogButtonBox::Ok);
                ok->setEnabled(false);
                connect(input,
                        &QLineEdit::textChanged,
                        &dialog,
                        [&dialog, ok](QString const& text)
                        {
                            auto const valid = chat::normalize_invite_token(text.toStdString()).has_value();
                            ok->setEnabled(valid);
                            dialog.setLabelText(!text.isEmpty() && !valid ? QStringLiteral("邀请码格式不正确，请检查后重试")
                                                                          : QStringLiteral("输入群主或管理员分享的邀请码"));
                        });
                if (dialog.exec() != QDialog::Accepted || !connection_available_)
                {
                    return;
                }
                // Any case is accepted, with spaces or a dash between the two groups.
                auto const token = chat::normalize_invite_token(dialog.textValue().toStdString());
                if (!token)
                {
                    set_error(QStringLiteral("邀请码无效，请检查后重新输入"));
                    return;
                }
                emit group_join_requested(QString::fromStdString(*token));
            });
    connect(conversations_delegate,
            &conversation_delegate::body_clicked,
            this,
            [this](QModelIndex const& index)
            {
                if (index.data(conversation_model::folder_header_role).toBool())
                {
                    toggle_folder(index.data(conversation_model::folder_role).toLongLong());
                }
                else
                {
                    select_conversation(index);
                }
            });
    connect(conversations_view_, &QListView::activated, this, &chat_widget::select_conversation);
    connect(conversations_view_, &QWidget::customContextMenuRequested, this, &chat_widget::show_conversation_menu);
    connect(conversations_delegate,
            &conversation_delegate::avatar_clicked,
            this,
            [this](QModelIndex const& index)
            {
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
    return conversation_panel;
}

void chat_widget::set_downloads(download_tasks& downloads)
{
    downloads_ = &downloads;
    messages_->set_downloads(downloads);
}
void chat_widget::setup_unified_search(client_bridge& client)
{
    auto* rooms = findChild<QWidget*>(QStringLiteral("sidebarRooms"));
    auto* layout = qobject_cast<QVBoxLayout*>(rooms->parentWidget()->layout());
    auto* panel = new sidebar_search(client, *conversation_search_, *contacts_, *conversations_, avatars_, rooms->parentWidget());
    layout->addWidget(panel);
    auto update = [this, rooms, panel]
    {
        auto const empty = conversation_search_->text().isEmpty();
        rooms->setVisible(empty);
        panel->setVisible(!empty);
    };
    connect(conversation_search_, &QLineEdit::textChanged, panel, update);
    update();
    connect(panel,
            &sidebar_search::contact_requested,
            this,
            [this](qint64 user, QString name)
            {
                conversation_search_->clear();
                open_chat(user, std::move(name));
            });
    connect(panel,
            &sidebar_search::room_requested,
            this,
            [this](qint64 id)
            {
                if (auto room = conversation(id))
                {
                    conversation_search_->clear();
                    open_conversation(*room);
                }
            });
    connect(panel, &sidebar_search::message_requested, this, &chat_widget::message_context_requested);
}

void chat_widget::add_local_content(QMenu& menu)
{
    auto listed = [this](qint64 id)
    {
        auto room = conversation(id);
        return room && !room->hidden;
    };
    auto* unavailable_texts = menu.addMenu(QStringLiteral("未显示会话的本地内容"));
    unavailable_texts->setObjectName(QStringLiteral("unavailableLocalMessages"));
    unavailable_texts->clear();
    for (auto it = drafts_.cbegin(); it != drafts_.cend(); ++it)
    {
        if (it->text.isEmpty() || listed(it.key()))
        {
            continue;
        }
        auto* actions = unavailable_texts->addMenu(QStringLiteral("草稿 · ") + grapheme_prefix(it->text, 40));
        auto const id = it.key();
        connect(actions->addAction(QStringLiteral("复制")),
                &QAction::triggered,
                this,
                [this, id] { QGuiApplication::clipboard()->setText(drafts_.value(id).text); });
        connect(actions->addAction(QStringLiteral("删除草稿")),
                &QAction::triggered,
                this,
                [this, id]
                {
                    auto const owner = self_user_;
                    auto const revision = drafts_.value(id).revision;
                    if (!confirm_action(this,
                                        QStringLiteral("删除草稿"),
                                        QStringLiteral("删除这台设备上该会话的草稿？已发送的消息会保留。"),
                                        QStringLiteral("删除草稿")) ||
                        owner != self_user_ || !drafts_.contains(id) || drafts_.value(id).revision != revision)
                    {
                        return;
                    }
                    drafts_.remove(id);
                    emit draft_save_requested(self_user_, id, chat::client::new_client_id(), 0, {}, {});
                });
    }
    for (auto const& item : outgoing_)
    {
        if (!item.path.isEmpty() || (!item.failed && !item.waiting) || listed(item.conversation))
        {
            continue;
        }
        auto* actions = unavailable_texts->addMenu(grapheme_prefix(item.text, 40));
        auto const id = item.client_id;
        actions->addAction(QStringLiteral("会话 #%1").arg(item.conversation))->setEnabled(false);
        if (!item.error.isEmpty())
        {
            actions->addAction(item.error)->setEnabled(false);
        }
        if (item.failed && !item.unavailable)
        {
            auto* retry = actions->addAction(QStringLiteral("重新发送"));
            retry->setEnabled(connection_available_);
            connect(retry, &QAction::triggered, this, [this, id] { emit pending_text_action_requested(id, false); });
        }
        connect(actions->addAction(QStringLiteral("复制")),
                &QAction::triggered,
                this,
                [this, id]
                {
                    auto const found = std::ranges::find(outgoing_, id, &outgoing::client_id);
                    if (found != outgoing_.end())
                    {
                        QGuiApplication::clipboard()->setText(found->text);
                    }
                });
        auto* discard = actions->addAction(QStringLiteral("删除待发消息"));
        connect(discard, &QAction::triggered, this, [this, id] { emit pending_text_action_requested(id, true); });
    }
    for (auto const& item : outgoing_)
    {
        if (!item.path.isEmpty() && !conversation(item.conversation))
        {
            auto* local = unavailable_texts->addMenu(QStringLiteral("会话 #%1 · %2").arg(item.conversation).arg(item.filename));
            add_upload_actions(*local, item.client_id);
        }
    }
    unavailable_texts->menuAction()->setVisible(!unavailable_texts->isEmpty());
}

void chat_widget::add_list_actions(QMenu& menu)
{
    menu.addAction(QStringLiteral("新建会话分组"), this, [this] { edit_folder_name(0); })->setEnabled(connection_available_);
    menu.addAction(QStringLiteral("所有会话标为已读"), this, [this] { emit mark_read_scope(0, -1); })->setEnabled(connection_available_);
    add_local_content(menu);
}

void chat_widget::show_conversation_menu(QPoint point)
{
    auto const ix = conversations_view_->indexAt(point);
    if (connection_available_ && ix.data(conversation_model::folder_header_role).toBool())
    {
        folder_menu(point, ix);
        return;
    }
    auto const* item = conversations_->conversation_at(ix);
    if (!item)
    {
        QMenu menu(this);
        resume_reading_after_menu(menu);
        add_list_actions(menu);
        menu.exec(conversations_view_->viewport()->mapToGlobal(point));
        return;
    }
    if (!connection_available_)
    {
        QMenu menu(this);
        resume_reading_after_menu(menu);
        add_list_actions(menu);
        menu.exec(conversations_view_->viewport()->mapToGlobal(point));
        return;
    }
    auto const conversation = item->id;

    auto const pinned = !item->pinned;
    QMenu menu(this);
    resume_reading_after_menu(menu);
    menu.addAction(pinned ? QStringLiteral("置顶") : QStringLiteral("取消置顶"),
                   [this, conversation, pinned]
                   {
                       if (connection_available_ && conversations_->index_for_conversation(conversation).isValid())
                       {
                           emit pin_requested(conversation, pinned);
                       }
                   });
    if (!conversations_->folders().isEmpty())
    {
        auto* move = menu.addMenu(QStringLiteral("移动到分组"));
        auto add = [this, move, conversation, current = item->folder](qint64 id, QString const& name)
        { move->addAction(name, this, [this, conversation, id] { emit move_conversation_to_folder(conversation, id); })->setEnabled(id != current); };
        for (auto const& [id, name] : conversations_->folders())
        {
            add(id, name);
        }
        add(0, QStringLiteral("未分组"));
    }
    menu.addSeparator();
    add_list_actions(*menu.addMenu(QStringLiteral("会话列表")));
    auto* levels = menu.addMenu(QStringLiteral("通知"));
    auto add_level = [this, levels, conversation](QString title, chat::notification_level level, qint64 duration = 0)
    {
        return levels->addAction(
            title,
            this,
            [this, conversation, level, duration]
            { emit notification_level_requested(conversation, level, duration ? QDateTime::currentMSecsSinceEpoch() + duration : 0); });
    };
    auto* all = add_level(QStringLiteral("全部通知"), chat::notification_level::all);
    auto* mentions = add_level(item->group ? QStringLiteral("仅提及") : QStringLiteral("仅直接发给我"), chat::notification_level::mentions);
    add_level(QStringLiteral("免打扰 1 小时"), chat::notification_level::muted, 3600000);
    add_level(QStringLiteral("免打扰 8 小时"), chat::notification_level::muted, 28800000);
    auto* permanent = add_level(QStringLiteral("永久免打扰"), chat::notification_level::muted);
    auto* until = levels->addAction(QString{});
    until->setEnabled(false);
    for (auto* action : {all, mentions, permanent, until})
    {
        action->setCheckable(true);
    }
    auto refresh_levels = [this, conversation, all, mentions, permanent, until, &menu]
    {
        auto const* room = conversations_->conversation_at(conversations_->index_for_conversation(conversation));
        if (!room || !connection_available_)
        {
            menu.close();
            return;
        }
        auto const muted = room->muted();
        mentions->setChecked(room->notifications == chat::notification_level::mentions);
        all->setChecked(!muted && !mentions->isChecked());
        permanent->setChecked(muted && !room->mute_until);
        until->setVisible(muted && room->mute_until > 0);
        if (until->isVisible())
        {
            until->setChecked(true);
            until->setText(QStringLiteral("免打扰至 %1").arg(QDateTime::fromMSecsSinceEpoch(room->mute_until).toString("MM-dd HH:mm")));
        }
    };
    connect(levels, &QMenu::aboutToShow, &menu, refresh_levels);
    connect(mute_expiry_timer_, &QTimer::timeout, &menu, refresh_levels);
    connect(conversations_, &QAbstractItemModel::modelReset, &menu, refresh_levels);
    connect(conversations_, &QAbstractItemModel::dataChanged, &menu, refresh_levels);
    menu.addSeparator();
    menu.addAction(QStringLiteral("标为已读"), this, [this, conversation] { emit mark_read_scope(conversation, -1); });
    auto const mark = !item->marked_unread;
    menu.addAction(mark ? QStringLiteral("标记未读") : QStringLiteral("清除未读标记"),
                   this,
                   [this, conversation, mark] { emit unread_mark_requested(conversation, mark); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("隐藏会话"), [this, conversation] { emit history_view_requested(conversation, true); });
    menu.addAction(svg_icon(u"close", themed("danger")),
                   QStringLiteral("清空聊天记录"),
                   [this, conversation] { emit history_view_requested(conversation, false); });
    menu.exec(conversations_view_->viewport()->mapToGlobal(point));
}

void chat_widget::set_friend_groups(QStringList const& groups) { friends_list_->set_groups(groups); }

void chat_widget::friend_group_menu(QPoint point, QString const& group)
{
    QMenu menu(contacts_dialog_);
    auto* create = menu.addAction(QStringLiteral("新建分组"));
    auto* rename = group.isEmpty() ? nullptr : menu.addAction(QStringLiteral("重命名分组"));
    auto* remove = group.isEmpty() ? nullptr : menu.addAction(QStringLiteral("删除分组"));
    auto* action = menu.exec(contacts_view_->viewport()->mapToGlobal(point));
    if (!action || !connection_available_ || !friends_list_->groups().contains(group))
    {
        return;
    }
    if (action == remove)
    {
        if (confirm_action(contacts_dialog_, QStringLiteral("删除分组"), QStringLiteral("该组好友将移回「我的好友」。"), QStringLiteral("删除")))
        {
            emit delete_friend_group_requested(group);
        }
        return;
    }
    bool ok = false;
    auto name = prompt_text(contacts_dialog_, action->text(), QStringLiteral("分组名称（最多 32 个字符）"), action == rename ? group : QString{}, ok)
                    .trimmed();
    if (!ok)
    {
        return;
    }
    if (name.isEmpty() || name.toUcs4().size() > 32 || name == QStringLiteral("我的好友"))
    {
        set_contacts_error(QStringLiteral("请输入 1–32 个字符的自定义分组名称"));
        return;
    }
    if (action == create)
    {
        emit create_friend_group_requested(name);
    }
    else
    {
        emit rename_friend_group_requested(group, name);
    }
}

void chat_widget::setup_contacts_dialog()
{
    contacts_dialog_ = new QDialog(this);
    contacts_dialog_->setObjectName(QStringLiteral("contactsDialog"));
    contacts_dialog_->setWindowTitle(QStringLiteral("好友"));
    contacts_dialog_->setAccessibleName(QStringLiteral("好友"));
    contacts_dialog_->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    contacts_dialog_->setWindowModality(Qt::WindowModal);
    contacts_dialog_->resize(460, 560);
    contacts_dialog_->setMinimumSize(390, 460);
    auto* contacts_layout = new QVBoxLayout(contacts_dialog_);
    contacts_layout->setContentsMargins(0, 0, 0, 0);
    contacts_layout->setSpacing(0);
    frame_dialog(*contacts_dialog_, false);
    contacts_pages_ = new QStackedWidget(contacts_dialog_);
    contacts_pages_->setObjectName(QStringLiteral("contactsPages"));
    auto* contacts_home = new QWidget(contacts_pages_);
    auto* contacts_home_layout = new QVBoxLayout(contacts_home);
    contacts_home_layout->setContentsMargins(0, 0, 0, 0);
    contacts_home_layout->setSpacing(0);
    contacts_pages_->addWidget(contacts_home);
    contacts_layout->addWidget(contacts_pages_, 1);
    contacts_feedback_ = new feedback_label(contacts_dialog_);
    contacts_feedback_->setObjectName(QStringLiteral("contactsRequestFeedback"));
    contacts_feedback_->setWordWrap(true);
    contacts_feedback_->hide();
    contacts_layout->addWidget(contacts_feedback_);
    connect(contacts_dialog_, &QDialog::finished, this, [this] { show_contacts_home(); });
    contact_search_ = new QLineEdit(contacts_dialog_);
    contact_search_->setObjectName(QStringLiteral("userSearchEdit"));
    contact_search_->setPlaceholderText(QStringLiteral("搜索好友"));
    contact_search_->setAccessibleName(QStringLiteral("搜索好友"));
    contact_search_->setToolTip(QStringLiteral("搜索已确认的好友，按 Enter 选择结果"));
    contact_search_->setClearButtonEnabled(false);
    style_search_field(*contact_search_);
    auto* contact_search_layout = new QHBoxLayout;
    contact_search_layout->setContentsMargins(16, 2, 16, 4);
    contact_search_layout->addWidget(contact_search_, 1);
    contacts_add_button_ = new QToolButton(contacts_dialog_);
    contacts_add_button_->setObjectName(QStringLiteral("contactsAddButton"));
    contacts_add_button_->setAccessibleName(QStringLiteral("添加好友"));
    contacts_add_button_->setToolTip(QStringLiteral("添加好友"));
    contacts_add_button_->setFixedHeight(36);
    contacts_add_button_->setCursor(Qt::PointingHandCursor);
    contacts_add_button_->setIcon(svg_icon(u"user-plus", themed("text_secondary"), QSize(20, 20)));
    connect(contacts_add_button_, &QToolButton::clicked, this, [this] { show_add_contact_dialog(); });
    contacts_home_layout->addLayout(contact_search_layout);
    auto* tabs = new QTabWidget(contacts_home);
    tabs->setObjectName(QStringLiteral("contactCategories"));
    auto* friends_page = new QWidget(tabs);
    auto* friends_layout = new QVBoxLayout(friends_page);
    friends_layout->setContentsMargins(0, 0, 0, 0);
    friends_layout->setSpacing(0);
    tabs->addTab(friends_page, QStringLiteral("好友"));
    contacts_home_layout->addWidget(tabs, 1);
    contacts_requests_button_ = new QPushButton(contacts_home);
    contacts_requests_button_->setAutoDefault(false);
    contacts_requests_button_->setObjectName(QStringLiteral("contactsRequestsButton"));
    contacts_requests_button_->setAccessibleName(QStringLiteral("好友申请"));
    contacts_requests_button_->setToolTip(QStringLiteral("查看收到和发出的好友申请"));
    contacts_requests_button_->setFixedHeight(56);
    contacts_requests_button_->setCursor(Qt::PointingHandCursor);
    auto* requests_entry_layout = new QHBoxLayout(contacts_requests_button_);
    requests_entry_layout->setContentsMargins(16, 4, 16, 4);
    requests_entry_layout->setSpacing(12);
    contacts_requests_icon_ = new QLabel(contacts_requests_button_);
    contacts_requests_icon_->setObjectName(QStringLiteral("contactsRequestsIcon"));
    contacts_requests_icon_->setFixedSize(40, 40);
    contacts_requests_icon_->setAlignment(Qt::AlignCenter);
    contacts_requests_icon_->setPixmap(svg_icon(u"user-plus", themed("text_secondary"), QSize(22, 22)).pixmap(22, 22));
    requests_entry_layout->addWidget(contacts_requests_icon_);
    auto* requests_entry_text = new QVBoxLayout;
    requests_entry_text->setContentsMargins(0, 0, 0, 0);
    requests_entry_text->setSpacing(3);
    auto* requests_entry_title = new QLabel(QStringLiteral("好友申请"), contacts_requests_button_);
    requests_entry_title->setObjectName(QStringLiteral("contactsRequestsTitle"));
    requests_entry_text->addWidget(requests_entry_title);
    contacts_requests_summary_ = new QLabel(contacts_requests_button_);
    contacts_requests_summary_->setObjectName(QStringLiteral("contactsRequestsSummary"));
    requests_entry_text->addWidget(contacts_requests_summary_);
    requests_entry_layout->addLayout(requests_entry_text, 1);
    contacts_requests_badge_ = new QLabel(contacts_requests_button_);
    contacts_requests_badge_->setObjectName(QStringLiteral("contactsRequestsBadge"));
    contacts_requests_badge_->setAlignment(Qt::AlignCenter);
    requests_entry_layout->addWidget(contacts_requests_badge_);
    contacts_requests_arrow_ = new QLabel(contacts_requests_button_);
    contacts_requests_arrow_->setFixedSize(16, 16);
    contacts_requests_arrow_->setPixmap(svg_icon(u"chevron-right", themed("text_secondary"), QSize(16, 16)).pixmap(16, 16));
    requests_entry_layout->addWidget(contacts_requests_arrow_);
    for (auto* label : contacts_requests_button_->findChildren<QLabel*>())
    {
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    friends_layout->addWidget(contacts_requests_button_);
    connect(contacts_requests_button_, &QPushButton::clicked, this, [this] { show_friend_requests(); });
    contacts_status_ = new QLabel(QStringLiteral("暂无好友"), contacts_dialog_);
    contacts_status_->setObjectName(QStringLiteral("contactsStatus"));
    contacts_status_->setContentsMargins(18, 8, 14, 8);
    contacts_status_->setWordWrap(true);
    friends_layout->addWidget(contacts_status_);

    contacts_ = new user_model(this, &avatars_);
    friends_list_ = new friend_list_model(*contacts_);
    connect(friends_list_,
            &friend_list_model::move_requested,
            this,
            [this](qint64 user, QString group)
            {
                if (connection_available_)
                {
                    emit move_friend_requested(user, group);
                }
            });
    contacts_view_ = new QListView(contacts_dialog_);
    contacts_view_->setObjectName(QStringLiteral("userList"));
    contacts_view_->setAccessibleName(QStringLiteral("好友列表"));
    contacts_view_->setModel(friends_list_);
    contacts_view_->setDragDropMode(QAbstractItemView::DragDrop);
    contacts_view_->setDefaultDropAction(Qt::CopyAction);
    contacts_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(contacts_view_,
            &QWidget::customContextMenuRequested,
            this,
            [this](QPoint point)
            {
                auto index = contacts_view_->indexAt(point);
                if (!connection_available_)
                {
                    return;
                }
                auto const* item = contacts_->user_at(friends_list_->source_index(index));
                if (!item)
                {
                    friend_group_menu(point, index.data(user_model::friend_group_role).toString());
                    return;
                }
                auto user = item->id;
                auto current = item->remark;
                QMenu menu(contacts_dialog_);
                auto* remark = menu.addAction(QStringLiteral("编辑备注"));
                auto* move = menu.addMenu(QStringLiteral("移动到分组"));
                for (auto const& name : friends_list_->groups())
                {
                    auto* group = move->addAction(name.isEmpty() ? QStringLiteral("我的好友") : name);
                    group->setData(name);
                    group->setEnabled(item->group != group->data().toString());
                }
                auto* action = menu.exec(contacts_view_->viewport()->mapToGlobal(point));
                if (!action)
                {
                    return;
                }
                if (action != remark)
                {
                    emit move_friend_requested(user, action->data().toString());
                    return;
                }
                bool ok = false;
                auto value =
                    prompt_text(contacts_dialog_, QStringLiteral("编辑备注"), QStringLiteral("备注（最多 32 个字符，留空恢复用户名）"), current, ok);
                if (!ok)
                {
                    return;
                }
                if (value.toUcs4().size() > 32)
                {
                    set_contacts_error(QStringLiteral("好友备注最多 32 个字符"));
                    return;
                }
                emit set_friend_remark_requested(user, value);
            });
    auto* contacts_delegate = new friend_list_delegate(contacts_view_);
    contacts_view_->setItemDelegate(contacts_delegate);
    contacts_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    contacts_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    contacts_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    contacts_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    contacts_view_->setMouseTracking(true);
    contacts_view_->verticalScrollBar()->setSingleStep(24);
    friends_layout->addWidget(contacts_view_, 1);
    contacts_status_->setAlignment(Qt::AlignCenter);
    auto* groups_page = new QWidget(tabs);
    auto* groups_layout = new QVBoxLayout(groups_page);
    groups_layout->setContentsMargins(0, 0, 0, 0);
    groups_layout->setSpacing(0);
    auto* groups = new QListWidget(groups_page);
    groups->setObjectName(QStringLiteral("joinedGroupsList"));
    groups->setContextMenuPolicy(Qt::CustomContextMenu);
    groups->setIconSize(QSize(34, 34));
    groups_layout->addWidget(groups, 1);
    auto* empty_groups = new QLabel(groups_page);
    empty_groups->setAlignment(Qt::AlignCenter);
    groups_layout->addWidget(empty_groups, 1);
    tabs->addTab(groups_page, QStringLiteral("群聊"));
    auto* pending = new QToolButton(groups_page);
    pending->setObjectName(QStringLiteral("groupApplicationsButton"));
    pending->setProperty("navigationTool", true);
    pending->setText(QStringLiteral("待处理入群申请"));
    pending->setPopupMode(QToolButton::InstantPopup);
    groups_layout->insertWidget(0, pending);
    auto* applications = new QMenu(pending);
    pending->setMenu(applications);
    connect(applications,
            &QMenu::aboutToShow,
            this,
            [this, applications]
            {
                applications->clear();
                for (auto const& room : conversations_->items())
                {
                    if (room.join_requests > 0)
                    {
                        applications->addAction(QStringLiteral("%1 (%2)").arg(room.username).arg(room.join_requests),
                                                this,
                                                [this, id = room.id]
                                                {
                                                    if (auto current = conversation(id); current && current->join_requests > 0)
                                                    {
                                                        contacts_dialog_->accept();
                                                        emit members_requested(id, self_user_, current->username, true);
                                                    }
                                                });
                    }
                }
            });
    auto refresh_groups = [this, groups, empty_groups, pending]
    {
        groups->clear();
        pending->setVisible(std::ranges::any_of(conversations_->items(), [](auto const& room) { return room.join_requests > 0; }));
        auto const query = contact_search_->text().trimmed();
        for (bool owned : {true, false})
        {
            QListWidgetItem* heading = nullptr;
            for (auto const& room : conversations_->items())
            {
                if (!room.group || (room.speech_role == chat::member_role::owner) != owned || !room.username.contains(query, Qt::CaseInsensitive))
                {
                    continue;
                }
                if (!heading)
                {
                    heading = new QListWidgetItem(owned ? QStringLiteral("我创建的群聊") : QStringLiteral("我加入的群聊"), groups);
                    heading->setFlags(Qt::NoItemFlags);
                }
                QString label = room.username + QStringLiteral("  ·  %1 人").arg(room.member_count);
                if (room.speech_role == chat::member_role::admin)
                {
                    label += QStringLiteral(" · 管理员");
                }
                if (room.join_requests)
                {
                    label += QStringLiteral("  ·  %1 个入群申请").arg(room.join_requests);
                }
                auto* row = new QListWidgetItem(avatar_icon(room.username, 34, avatars_.image(room.avatar_key()), room.id), label, groups);
                row->setData(Qt::UserRole, room.id);
                row->setToolTip(label);
                row->setSizeHint(QSize(0, 46));
            }
        }
        groups->setVisible(groups->count() > 0);
        empty_groups->setVisible(groups->count() == 0);
        empty_groups->setText(query.isEmpty() ? QStringLiteral("暂无群聊") : QStringLiteral("没有匹配的群聊"));
    };
    connect(tabs,
            &QTabWidget::currentChanged,
            this,
            [this, refresh_groups](int tab)
            {
                contact_search_->setPlaceholderText(tab ? QStringLiteral("搜索群聊") : QStringLiteral("搜索好友"));
                refresh_groups();
            });
    connect(contact_search_, &QLineEdit::textChanged, groups, refresh_groups);
    connect(conversations_, &QAbstractItemModel::modelReset, groups, refresh_groups);
    connect(&avatars_,
            &avatar_cache::changed,
            groups,
            [groups, refresh_groups](qint64 key)
            {
                if (key < 0 && groups->isVisible())
                {
                    refresh_groups();
                }
            });
    auto open = [this](QListWidgetItem* row)
    {
        if (auto room = conversation(row->data(Qt::UserRole).toLongLong()))
        {
            contacts_dialog_->accept();
            open_conversation(*room);
        }
    };
    connect(groups, &QListWidget::itemClicked, this, open);
    connect(groups, &QListWidget::itemActivated, this, open);
    connect(groups,
            &QWidget::customContextMenuRequested,
            this,
            [this, groups](QPoint pos)
            {
                auto* row = groups->itemAt(pos);
                if (!row)
                {
                    return;
                }
                auto const id = row->data(Qt::UserRole).toLongLong();
                if (!id)
                {
                    return;
                }
                QMenu menu(groups);
                auto* info = menu.addAction(QStringLiteral("群资料"));
                if (menu.exec(groups->viewport()->mapToGlobal(pos)) == info)
                {
                    if (auto room = conversation(id))
                    {
                        contacts_dialog_->accept();
                        emit members_requested(id, self_user_, room->username);
                    }
                }
            });
    auto* group_create = new QPushButton(QStringLiteral("创建群聊"), contacts_home);
    auto* group_join = new QPushButton(QStringLiteral("加入群聊"), contacts_home);
    connect(group_create,
            &QPushButton::clicked,
            this,
            [this]
            {
                contacts_dialog_->accept();
                create_group();
            });
    connect(group_join,
            &QPushButton::clicked,
            this,
            [this]
            {
                contacts_dialog_->accept();
                findChild<QAction*>(QStringLiteral("joinGroupAction"))->trigger();
            });
    auto* footer_row = new QHBoxLayout;
    footer_row->setContentsMargins(16, 6, 16, 10);
    footer_row->addWidget(contacts_add_button_);
    contacts_add_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    contacts_add_button_->setText(QStringLiteral("添加好友"));
    contacts_add_button_->setMinimumWidth(100);
    footer_row->addWidget(group_create);
    footer_row->addWidget(group_join);
    footer_row->addStretch();
    contacts_home_layout->addLayout(footer_row);
    for (auto* button : {group_create, group_join})
    {
        button->setFixedHeight(36);
    }
    auto update_page_actions = [this, tabs, group_create, group_join]
    {
        bool const friends = tabs->currentIndex() == 0;
        contacts_add_button_->setVisible(friends);
        group_create->setVisible(!friends);
        group_join->setVisible(!friends);
    };
    connect(tabs, &QTabWidget::currentChanged, this, update_page_actions);
    connect(contacts_pages_, &QStackedWidget::currentChanged, this, update_page_actions);
    update_page_actions();
    refresh_groups();
    connect(contact_search_,
            &QLineEdit::returnPressed,
            this,
            [this]
            {
                if (!contacts_view_->isVisible() || friends_list_->rowCount() == 0)
                {
                    return;
                }
                auto index = contacts_view_->currentIndex();
                if (!index.isValid())
                {
                    index = friends_list_->index(friends_list_->rowCount() > 1 && !contact_search_->text().isEmpty() ? 1 : 0, 0);
                }
                contacts_view_->setCurrentIndex(index);
                contacts_view_->setFocus();
            });
    connect(contact_search_, &QLineEdit::textChanged, this, [this](QString const& query) { filter_contacts(query); });
    connect(contacts_delegate, &user_delegate::body_clicked, this, [this](QModelIndex const& index) { select_contact(index); });
    connect(contacts_view_, &QListView::activated, this, &chat_widget::select_contact);
    connect(contacts_delegate,
            &user_delegate::avatar_clicked,
            this,
            [this](QModelIndex index)
            {
                auto const* user = contacts_->user_at(friends_list_->source_index(index));
                if (user)
                {
                    show_user_details(user->id, user->username);
                }
            });
}

void chat_widget::setup_add_contact_dialog()
{
    add_contact_dialog_ = new QDialog(this);
    add_contact_dialog_->setObjectName(QStringLiteral("addContactDialog"));
    add_contact_dialog_->setWindowTitle(QStringLiteral("添加好友"));
    add_contact_dialog_->setAccessibleName(QStringLiteral("添加好友"));
    add_contact_dialog_->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    add_contact_dialog_->setWindowModality(Qt::WindowModal);
    add_contact_dialog_->setMinimumWidth(420);
    auto* add_contacts_layout = new QVBoxLayout(add_contact_dialog_);
    add_contacts_layout->setContentsMargins(0, 0, 0, 0);
    add_contacts_layout->setSpacing(12);
    frame_dialog(*add_contact_dialog_, false);
    add_user_search_ = new QLineEdit(add_contact_dialog_);
    add_user_search_->setObjectName(QStringLiteral("userSearchEdit"));
    add_user_search_->setPlaceholderText(QStringLiteral("搜索用户名或昵称"));
    add_user_search_->setAccessibleName(QStringLiteral("搜索用户"));
    add_user_search_->setClearButtonEnabled(true);
    style_search_field(*add_user_search_);
    auto* add_search_layout = new QHBoxLayout;
    add_search_layout->setContentsMargins(18, 0, 18, 0);
    add_search_layout->addWidget(add_user_search_);
    add_contacts_layout->addLayout(add_search_layout);
    add_users_status_ = new QLabel(QStringLiteral("输入用户名后按 Enter 查找"), add_contact_dialog_);
    add_users_status_->setWordWrap(true);
    add_users_status_->setObjectName(QStringLiteral("addUsersStatus"));
    add_users_status_->setContentsMargins(18, 8, 14, 8);
    add_contacts_layout->addWidget(add_users_status_);

    add_users_ = new user_model(this, &avatars_);
    add_users_view_ = new QListView(add_contact_dialog_);
    add_users_view_->setObjectName(QStringLiteral("userList"));
    add_users_view_->setAccessibleName(QStringLiteral("用户搜索结果"));
    add_users_view_->setModel(add_users_);
    auto* add_users_delegate = new user_delegate(add_users_view_,
                                                 [this](QModelIndex const& index)
                                                 {
                                                     auto const user = index.data(user_model::id_role).toLongLong();
                                                     if (user == self_user_)
                                                     {
                                                         return QStringLiteral("这是你");
                                                     }
                                                     switch (friend_state(user))
                                                     {
                                                         case chat::friendship_state::accepted:
                                                             return QStringLiteral("已是好友");
                                                         case chat::friendship_state::incoming_pending:
                                                             return QStringLiteral("对方向你发送了好友申请");
                                                         case chat::friendship_state::outgoing_pending:
                                                             return QStringLiteral("已发出申请，等待对方确认");
                                                         default:
                                                             return QStringLiteral("添加好友后可发送消息");
                                                     }
                                                 });
    add_users_view_->setItemDelegate(add_users_delegate);
    add_users_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    add_users_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    add_users_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    add_users_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    add_users_view_->setMouseTracking(true);
    add_users_view_->verticalScrollBar()->setSingleStep(24);
    add_users_view_->hide();
    add_contacts_layout->addWidget(add_users_view_);
    auto* add_buttons = new QDialogButtonBox(add_contact_dialog_);
    add_buttons->setContentsMargins(18, 0, 18, 16);
    add_search_button_ = add_buttons->addButton(QStringLiteral("搜索"), QDialogButtonBox::ActionRole);
    add_search_button_->setObjectName(QStringLiteral("addContactSearchButton"));
    add_search_button_->setProperty("primary", true);
    add_search_button_->setDefault(true);
    add_search_button_->setEnabled(false);
    add_contacts_layout->addWidget(add_buttons);
    connect(add_search_button_, &QPushButton::clicked, this, [this] { search_users(); });
    connect(add_user_search_,
            &QLineEdit::textChanged,
            this,
            [this]
            {
                ++add_search_request_;
                add_users_->set_users({});
                add_users_view_->hide();
                add_users_status_->setText(connection_available_ ? QStringLiteral("输入用户名后按 Enter 或点击搜索")
                                                                 : QStringLiteral("当前离线，连接恢复后可搜索用户"));
                add_search_button_->setEnabled(connection_available_ && !add_user_search_->text().trimmed().isEmpty());
                add_contact_dialog_->adjustSize();
            });
    connect(add_contact_dialog_, &QDialog::finished, this, [this] { ++add_search_request_; });
    connect(add_users_delegate,
            &user_delegate::avatar_clicked,
            this,
            [this](QModelIndex const& index)
            {
                if (auto const* item = add_users_->user_at(index))
                {
                    show_user_details(item->id, item->username);
                }
            });
    connect(add_users_delegate,
            &user_delegate::body_clicked,
            this,
            [this](QModelIndex const& index)
            {
                add_users_view_->setCurrentIndex(index);
                select_add_user(index);
            });
    add_users_view_->installEventFilter(this);
}

void chat_widget::setup_friend_requests()
{
    auto* requests_page = new QWidget(contacts_pages_);
    auto* requests_layout = new QVBoxLayout(requests_page);
    requests_layout->setContentsMargins(0, 0, 0, 0);
    requests_layout->setSpacing(0);
    friend_requests_status_ = new QLabel(requests_page);
    friend_requests_status_->setObjectName(QStringLiteral("friendRequestsStatus"));
    friend_requests_status_->setWordWrap(true);
    friend_requests_status_->setContentsMargins(18, 8, 18, 8);
    friend_requests_status_->hide();
    requests_layout->addWidget(friend_requests_status_);
    auto make_request_group = [requests_page, requests_layout](QString name, QString accessible, QListWidget*& list, QLabel*& heading)
    {
        auto* group = new QWidget(requests_page);
        auto* group_layout = new QVBoxLayout(group);
        group_layout->setContentsMargins(0, 0, 0, 8);
        group_layout->setSpacing(0);
        heading = new QLabel(group);
        heading->setObjectName(QStringLiteral("friendRequestHeading"));
        auto* heading_layout = new QHBoxLayout;
        heading_layout->setContentsMargins(18, 12, 18, 8);
        heading_layout->addWidget(heading);
        group_layout->addLayout(heading_layout);
        list = new QListWidget(group);
        list->setObjectName(std::move(name));
        list->setAccessibleName(std::move(accessible));
        group_layout->addWidget(list, 1);
        requests_layout->addWidget(group, 1);
        return group_layout;
    };
    auto* incoming_layout =
        make_request_group(QStringLiteral("incomingFriendRequests"), QStringLiteral("收到的好友申请"), incoming_friends_, incoming_friends_title_);
    auto* outgoing_layout =
        make_request_group(QStringLiteral("outgoingFriendRequests"), QStringLiteral("发出的好友申请"), outgoing_friends_, outgoing_friends_title_);
    auto make_request_button = [](QVBoxLayout* group_layout, QString text, QString name)
    {
        auto* button = new QPushButton(std::move(text), group_layout->parentWidget());
        button->setAutoDefault(false);
        button->setObjectName(std::move(name));
        button->setAccessibleName(button->text());
        button->setMinimumHeight(36);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };
    incoming_accept_button_ = make_request_button(incoming_layout, QStringLiteral("接受"), QStringLiteral("friendRequestAcceptButton"));
    incoming_accept_button_->setProperty("primary", true);
    incoming_reject_button_ = make_request_button(incoming_layout, QStringLiteral("拒绝"), QStringLiteral("friendRequestRejectButton"));
    outgoing_cancel_button_ = make_request_button(outgoing_layout, QStringLiteral("撤回申请"), QStringLiteral("friendRequestCancelButton"));
    auto* incoming_actions = new QHBoxLayout;
    incoming_actions->setContentsMargins(18, 8, 18, 0);
    incoming_actions->addStretch();
    incoming_actions->addWidget(incoming_reject_button_);
    incoming_actions->addWidget(incoming_accept_button_);
    incoming_layout->addLayout(incoming_actions);
    auto* outgoing_actions = new QHBoxLayout;
    outgoing_actions->setContentsMargins(18, 8, 18, 0);
    outgoing_actions->addStretch();
    outgoing_actions->addWidget(outgoing_cancel_button_);
    outgoing_layout->addLayout(outgoing_actions);
    auto request_selected = [this](QListWidget* list, int action)
    {
        if (auto* item = list->currentItem())
        {
            request_friend_action(item->data(Qt::UserRole).toLongLong(), item->data(Qt::UserRole + 1).toString(), action);
        }
    };
    connect(incoming_accept_button_,
            &QPushButton::clicked,
            this,
            [request_selected, this] { request_selected(incoming_friends_, static_cast<int>(chat::friendship_state::incoming_pending)); });
    connect(incoming_reject_button_, &QPushButton::clicked, this, [request_selected, this] { request_selected(incoming_friends_, -1); });
    connect(outgoing_cancel_button_,
            &QPushButton::clicked,
            this,
            [request_selected, this] { request_selected(outgoing_friends_, static_cast<int>(chat::friendship_state::outgoing_pending)); });
    requests_layout->addStretch();
    contacts_pages_->addWidget(requests_page);
    auto* back = new QShortcut(QKeySequence(Qt::Key_Escape), requests_page);
    back->setContext(Qt::WidgetWithChildrenShortcut);
    connect(back, &QShortcut::activated, this, &chat_widget::show_contacts_home);
    for (auto* list : {incoming_friends_, outgoing_friends_})
    {
        auto* delegate = new user_delegate(list);
        list->setItemDelegate(delegate);
        // Requests use the existing profile and its relationship actions.
        connect(delegate,
                &user_delegate::avatar_clicked,
                this,
                [this](QModelIndex const& index)
                { show_user_details(index.data(Qt::UserRole).toLongLong(), index.data(Qt::UserRole + 1).toString()); });
        connect(delegate,
                &user_delegate::body_clicked,
                this,
                [this, list](QModelIndex const& index)
                {
                    list->setCurrentRow(index.row());
                    show_user_details(index.data(Qt::UserRole).toLongLong(), index.data(Qt::UserRole + 1).toString());
                });
        connect(list, &QListWidget::currentItemChanged, this, [this] { update_request_actions(); });
        list->setUniformItemSizes(true);
        list->setFrameShape(QFrame::NoFrame);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setMouseTracking(true);
        list->installEventFilter(this);
    }
}

QWidget* chat_widget::create_chat_panel()
{
    auto* chat_panel = new QFrame(this);
    chat_panel->setObjectName(QStringLiteral("chatPanel"));
    chat_panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* chat_layout = new QVBoxLayout(chat_panel);
    chat_layout->setContentsMargins(0, 0, 0, 0);
    chat_layout->setSpacing(0);

    setup_chat_header(chat_layout);
    setup_message_list(chat_layout);
    setup_composer(chat_layout);
    update_chat_header({});
    return chat_panel;
}

void chat_widget::setup_chat_header(QVBoxLayout* chat_layout)
{
    auto* chat_panel = chat_layout->parentWidget();
    auto* header = new QFrame(chat_panel);
    header->setObjectName(QStringLiteral("chatHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(chat_theme::message_side_margin, 8, 16, 8);
    header_layout->setSpacing(8);

    chat_title_ = new QPushButton(header);
    chat_title_->setObjectName(QStringLiteral("chatHeaderButton"));
    chat_title_->setFlat(true);
    chat_title_->setEnabled(false);
    chat_title_->setFixedSize(42, 42);
    chat_title_->setCursor(Qt::PointingHandCursor);
    chat_title_->setIconSize(QSize(38, 38));
    header_layout->addWidget(chat_title_);
    auto* title_layout = new QVBoxLayout;
    title_layout->setSpacing(0);
    chat_name_ = new emoji_label(header);
    chat_name_->setObjectName(QStringLiteral("chatHeaderName"));
    chat_name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    title_layout->addWidget(chat_name_);
    chat_status_ = new emoji_label(header);
    chat_status_->setObjectName(QStringLiteral("chatHeaderStatus"));
    chat_status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    title_layout->addWidget(chat_status_);
    header_layout->addLayout(title_layout, 1);
    header_layout->setAlignment(title_layout, Qt::AlignVCenter);
    auto* search = new QToolButton(header);
    search->setDefaultAction(message_search_action_);
    search->setObjectName(QStringLiteral("searchChatButton"));
    search->setProperty("navigationTool", true);
    search->setToolTip(QStringLiteral("当前会话搜索 (Ctrl+F)"));
    auto search_icon = [this] { message_search_action_->setIcon(svg_icon(u"search", themed("text"), QSize(20, 20))); };
    search_icon();
    connect(&theme_manager::instance(), &theme_manager::changed, search, search_icon);
    search->setFixedSize(36, 36);
    header_layout->addWidget(search);
    connect(message_search_action_,
            &QAction::triggered,
            this,
            [this] { emit message_search_requested(active_conversation_, self_user_, active_group_, active_username_, {}); });
    chat_headers_ = new QStackedWidget(chat_panel);
    chat_headers_->addWidget(header);
    chat_headers_->setFixedHeight(header_layout->sizeHint().height());
    chat_layout->addWidget(chat_headers_);

    connection_status_ = new QToolButton(chat_panel);
    connection_status_->setObjectName(QStringLiteral("connectionStatusButton"));
    connection_status_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connection_status_->setCursor(Qt::PointingHandCursor);
    connection_status_->hide();
    chat_layout->addWidget(connection_status_, 0, Qt::AlignHCenter);

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
    pinned_row->hide();
    chat_layout->addWidget(pinned_row);
    connect(pinned_message_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const item = conversation(active_conversation_);
                if (item && item->pinned_message.id > 0)
                {
                    jump_to_message(item->pinned_message.id);
                }
            });
    connect(unpin_message_button_,
            &QToolButton::clicked,
            this,
            [this]
            {
                if (connection_available_ && messages_->can_manage_group())
                {
                    emit group_message_pin_requested(active_conversation_, std::nullopt);
                }
            });

    message_status_ = new QLabel(chat_panel);
    message_status_->setObjectName(QStringLiteral("subtleText"));
    message_status_->setTextFormat(Qt::PlainText);
    message_status_->setAlignment(Qt::AlignCenter);
    message_status_->setContentsMargins(0, 6, 0, 6);
    message_status_->hide();
    chat_layout->addWidget(message_status_);
    connect(chat_title_,
            &QPushButton::clicked,
            this,
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
    connect(connection_status_,
            &QToolButton::clicked,
            this,
            [this]
            {
                if (connection_status_->isEnabled())
                {
                    emit reconnect_requested();
                }
            });
}

void chat_widget::setup_message_list(QVBoxLayout* chat_layout)
{
    auto* chat_panel = chat_layout->parentWidget();
    messages_ = new message_model(this, &avatars_, &images_);
    auto* list = new message_list_view(chat_panel);
    messages_view_ = list;
    list->preserve_layout = [this](const auto& update) { preserve_reading(update); };
    auto* hint_layout = new QGridLayout(list->viewport());
    hint_layout->setContentsMargins(0, 0, 0, 0);
    empty_conversation_hint_ = new QLabel(QStringLiteral("选择一个对话开始聊天"), list->viewport());
    empty_conversation_hint_->setObjectName(QStringLiteral("emptyConversationHint"));
    empty_conversation_hint_->setAttribute(Qt::WA_TransparentForMouseEvents);
    empty_conversation_hint_->setAlignment(Qt::AlignCenter);
    hint_layout->addWidget(empty_conversation_hint_, 0, 0, Qt::AlignCenter);
    history_down_ = new QPushButton(messages_view_->viewport());
    history_down_->setObjectName(QStringLiteral("historyDownButton"));
    history_down_->setIcon(svg_icon(u"chevron-down", themed("text"), QSize(20, 20)));
    history_down_->setAutoDefault(false);
    history_down_->hide();
    connect(history_down_, &QPushButton::clicked, this, &chat_widget::navigate_down);
    messages_view_->setMouseTracking(true);
    messages_view_->setObjectName(QStringLiteral("messageList"));
    messages_view_->setAccessibleName(QStringLiteral("消息记录"));
    connect(messages_, &QAbstractItemModel::modelReset, this, [this] { QTimer::singleShot(0, messages_view_, [this] { load_visible_images(); }); });
    connect(messages_, &QAbstractItemModel::rowsInserted, this, [this] { QTimer::singleShot(0, messages_view_, [this] { load_visible_images(); }); });
    messages_view_->setAcceptDrops(true);
    messages_view_->viewport()->setAcceptDrops(true);
    file_drop_hint_ = new QLabel(QStringLiteral("松开发送文件"), messages_view_->viewport());
    file_drop_hint_->setObjectName(QStringLiteral("fileDropHint"));
    file_drop_hint_->setAutoFillBackground(true);
    file_drop_hint_->setFrameStyle(QFrame::StyledPanel);
    file_drop_hint_->setMargin(8);
    file_drop_hint_->move(12, 12);
    file_drop_hint_->setAttribute(Qt::WA_TransparentForMouseEvents);
    file_drop_hint_->hide();
    messages_view_->viewport()->installEventFilter(this);
    messages_view_->installEventFilter(this);
    messages_view_->setModel(messages_);
    auto* messages_delegate = new message_delegate(messages_view_, nullptr, &avatars_);
    messages_view_->setItemDelegate(messages_delegate);
    messages_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    messages_view_->setSelectionMode(QAbstractItemView::NoSelection);
    messages_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    messages_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    messages_view_->setSpacing(0);
    messages_view_->verticalScrollBar()->setSingleStep(24);
    chat_layout->addWidget(messages_view_, 1);
    connect(messages_view_->verticalScrollBar(), &QScrollBar::rangeChanged, this, &chat_widget::update_reading_navigation);

    selection_bar_ = new QWidget(chat_headers_);
    selection_bar_->setObjectName(QStringLiteral("messageSelectionBar"));
    auto* selection = new QHBoxLayout(selection_bar_);
    selection->setContentsMargins(chat_theme::message_side_margin, 8, 16, 8);
    auto action_button = [&](QString name, QString text, QString icon, bool danger = false)
    {
        auto* button = new QToolButton(selection_bar_);
        button->setObjectName(name);
        button->setText(text);
        button->setToolTip(text);
        button->setAccessibleName(text);
        button->setProperty("navigationTool", true);
        button->setFixedSize(38, 38);
        auto refresh = [button, icon, danger] { button->setIcon(svg_icon(icon, themed(danger ? "danger" : "text"))); };
        refresh();
        connect(&theme_manager::instance(), &theme_manager::changed, button, refresh);
        button->installEventFilter(this);
        selection->addWidget(button);
        return button;
    };
    auto* cancel = action_button(QStringLiteral("cancelSelection"), QStringLiteral("取消选择"), QStringLiteral("back"));
    selection_count_ = new QLabel(selection_bar_);
    selection_count_->setObjectName(QStringLiteral("selectedMessageCount"));
    selection->addWidget(selection_count_, 1);
    selection_copy_ = action_button(QStringLiteral("copySelection"), QStringLiteral("复制所选消息"), QStringLiteral("copy"));
    selection_forward_ = action_button(QStringLiteral("forwardSelection"), QStringLiteral("转发所选消息"), QStringLiteral("send"));
    selection_retract_ = action_button(QStringLiteral("retractSelection"), QStringLiteral("撤回所选消息"), QStringLiteral("close"), true);
    connect(selection_retract_, &QToolButton::clicked, this, &chat_widget::retract_selected_messages);
    list->selection_finished = [this] { update_message_selection(); };
    connect(messages_view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &chat_widget::update_message_selection);
    connect(messages_, &QAbstractItemModel::modelReset, this, &chat_widget::update_message_selection);
    connect(messages_,
            &QAbstractItemModel::dataChanged,
            this,
            [this, messages_delegate](QModelIndex const& first, QModelIndex const& last, QList<int> const& roles)
            {
                const auto selected = messages_delegate->text_index();
                if (selected.isValid() && selected.row() >= first.row() && selected.row() <= last.row() &&
                    (roles.isEmpty() || roles.contains(message_model::text_role)))
                {
                    messages_delegate->clear_text_selection();
                    messages_view_->viewport()->update();
                }
                // Retraction changes which indexes are selectable without emitting selectionChanged.
                if (messages_view_->selectionMode() == QAbstractItemView::ExtendedSelection && roles.isEmpty())
                {
                    update_message_selection();
                }
            });
    selection_copy_->setToolTip(QStringLiteral("复制选中消息（Ctrl+C）"));
    cancel->setToolTip(QStringLiteral("退出多选（Esc）"));
    connect(selection_copy_, &QToolButton::clicked, this, &chat_widget::copy_selected_messages);
    connect(selection_forward_,
            &QToolButton::clicked,
            this,
            [this]
            {
                QList<QPersistentModelIndex> rows;
                for (auto const& ix : selected_messages())
                {
                    rows.emplaceBack(ix);
                }
                choose_forward_target(rows);
            });
    connect(cancel, &QToolButton::clicked, this, [this] { set_message_selection(false); });
    update_message_selection();
    chat_headers_->addWidget(selection_bar_);
    reply_bar_ = new QWidget(chat_panel);
    reply_bar_->setObjectName(QStringLiteral("replyBar"));
    reply_bar_->setAttribute(Qt::WA_StyledBackground);
    auto* reply_layout = new QHBoxLayout(reply_bar_);
    reply_layout->setContentsMargins(12, 4, 12, 4);
    reply_preview_ = new emoji_label(reply_bar_);
    reply_preview_->setObjectName(QStringLiteral("replyPreview"));
    auto* cancel_reply = new QToolButton(reply_bar_);
    cancel_reply_button_ = cancel_reply;
    cancel_reply->setObjectName(QStringLiteral("cancelReplyButton"));
    cancel_reply->setAccessibleName(QStringLiteral("取消回复"));
    cancel_reply->setToolTip(QStringLiteral("取消回复"));
    cancel_reply->setIcon(svg_icon(QStringLiteral("close"), themed("text"), QSize(20, 20)));
    cancel_reply->setIconSize(QSize(20, 20));
    cancel_reply->setFixedSize(36, 36);
    cancel_reply->setCursor(Qt::PointingHandCursor);
    reply_layout->addWidget(reply_preview_, 1);
    reply_layout->addWidget(cancel_reply);
    reply_bar_->hide();
    chat_layout->addWidget(reply_bar_);
    connect(cancel_reply,
            &QToolButton::clicked,
            this,
            [this]
            {
                reset_reply();
                draft_edited();
                message_edit_->setFocus();
            });
    connect(messages_view_, &QListView::customContextMenuRequested, this, &chat_widget::show_message_menu);
    connect(messages_delegate, &message_delegate::reply_clicked, this, &chat_widget::jump_to_message);
    connect(messages_delegate,
            &message_delegate::avatar_clicked,
            this,
            [this](QModelIndex const& index)
            { show_user_details(index.data(message_model::from_role).toLongLong(), index.data(message_model::sender_name_role).toString()); });
    connect(messages_delegate,
            &message_delegate::vote_clicked,
            this,
            [this](QModelIndex const& ix, qint64 choice)
            {
                if (connection_available_ && !ix.data(message_model::poll_pending_role).toBool())
                {
                    auto id = ix.data(message_model::id_role).toLongLong();
                    auto poll = ix.data(message_model::poll_role).value<chat::poll_summary>();
                    if (poll.closed || poll.selection || poll.options.empty())
                    {
                        return;
                    }
                    auto mask = ix.data(message_model::poll_draft_role).toLongLong();
                    if (choice > 0 && choice <= std::ssize(poll.options))
                    {
                        mask = poll.multiple ? mask ^ chat::poll_bit(choice) : chat::poll_bit(choice);
                        messages_->set_poll_draft(id, mask);
                        if (poll.multiple)
                        {
                            return;
                        }
                    }
                    else if (choice != poll_submit || !poll.multiple || !mask)
                    {
                        return;
                    }
                    messages_->set_poll_pending(id, true);
                    emit poll_write_requested(active_conversation_, id, mask);
                }
            });
    connect(messages_delegate,
            &message_delegate::content_clicked,
            this,
            [this](QModelIndex const& ix)
            {
                emit content_requested(
                    active_conversation_, ix.data(message_model::id_role).toLongLong(), ix.data(message_model::content_role).toString());
            });
    connect(messages_delegate,
            &message_delegate::image_clicked,
            this,
            [this](QModelIndex const& index)
            {
                if (connection_available_ && !index.data(message_model::deleted_role).toBool())
                {
                    emit attachment_open_requested(active_conversation_,
                                                   index.data(message_model::id_role).toLongLong(),
                                                   index.data(message_model::attachment_name_role).toString(),
                                                   true);
                }
            });
    connect(messages_delegate,
            &message_delegate::reaction_clicked,
            this,
            [this](QModelIndex const& index, QString emoji)
            {
                auto const removing = emoji == index.data(message_model::own_reaction_role).toString();
                if (connection_available_ && (removing || can_send()) && !index.data(message_model::deleted_role).toBool())
                {
                    emit reaction_requested(active_conversation_, index.data(message_model::id_role).toLongLong(), removing ? QString{} : emoji);
                }
            });
}

void chat_widget::setup_composer(QVBoxLayout* chat_layout)
{
    auto* chat_panel = chat_layout->parentWidget();
    speech_notice_ = new QLabel(chat_panel);
    speech_notice_->setObjectName(QStringLiteral("groupSpeechNotice"));
    speech_notice_->hide();
    chat_layout->addWidget(speech_notice_);
    speech_expiry_ = new QTimer(this);
    speech_expiry_->setSingleShot(true);
    connect(speech_expiry_, &QTimer::timeout, this, &chat_widget::update_compose_state);
    auto* input_bar = new QFrame(chat_panel);
    input_bar_ = input_bar;
    input_bar->setAcceptDrops(true);
    input_bar->installEventFilter(this);
    input_bar->hide();
    input_bar->setObjectName(QStringLiteral("inputBar"));
    auto* rows = new QBoxLayout(QBoxLayout::LeftToRight, input_bar);
    rows->setContentsMargins(10, 4, 10, 4);
    rows->setSpacing(4);
    auto* field_layout = new QHBoxLayout;
    field_layout->setSpacing(4);
    auto* input_layout = new QHBoxLayout;
    input_layout->setSpacing(3);
    message_edit_ = new QPlainTextEdit(input_bar);
    message_edit_->setObjectName(QStringLiteral("messageEdit"));
    message_edit_->setPlaceholderText(QStringLiteral("输入消息…"));
    message_edit_->setAccessibleName(QStringLiteral("消息输入"));
    message_edit_->setToolTip(QStringLiteral("Enter 发送，Shift+Enter 换行"));
    message_edit_->setTabChangesFocus(true);
    message_edit_->document()->setDocumentMargin(10);
    message_edit_->setFixedHeight(chat_theme::compose_field_min_height);
    message_edit_->installEventFilter(this);
    message_edit_->viewport()->setAcceptDrops(true);
    message_edit_->viewport()->installEventFilter(this);
    message_edit_->setEnabled(false);
    new emoji_highlighter(message_edit_);
    field_layout->addWidget(message_edit_, 1, Qt::AlignVCenter);
    rows->addLayout(field_layout, 1);
    rows->addLayout(input_layout);
    auto const resize_composer = [this]
    {
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
    connect(
        message_edit_->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, resize_composer, Qt::QueuedConnection);
    connect(message_edit_, &QPlainTextEdit::updateRequest, this, resize_composer, Qt::QueuedConnection);
    attachment_button_ = new QToolButton(input_bar);
    attachment_button_->setObjectName(QStringLiteral("sendAttachmentButton"));
    attachment_button_->setText(QStringLiteral("文件/图片"));
    attachment_button_->setToolTip(QStringLiteral("发送文件或图片"));
    attachment_button_->setAccessibleName(QStringLiteral("发送文件或图片"));
    attachment_button_->setIcon(svg_icon(u"attach", themed("text"), QSize(22, 22)));
    attachment_button_->setIconSize(QSize(22, 22));
    attachment_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    attachment_button_->setFixedSize(chat_theme::compose_button_width, chat_theme::compose_button_height);
    attachment_button_->setEnabled(false);
    field_layout->insertWidget(0, attachment_button_, 0, Qt::AlignVCenter);
    input_layout->addStretch();
    auto make_composer_button = [input_bar](QString const& name, QStringView icon, QString const& label)
    {
        auto* button = new QToolButton(input_bar);
        button->setObjectName(name);
        button->setToolTip(label);
        button->setAccessibleName(label);
        button->setIcon(svg_icon(icon, themed("text"), QSize(22, 22)));
        button->setIconSize(QSize(22, 22));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(chat_theme::compose_button_width, chat_theme::compose_button_height);
        button->setCursor(Qt::PointingHandCursor);
        button->setEnabled(false);
        return button;
    };
    screenshot_button_ = make_composer_button(QStringLiteral("screenshotButton"), u"screenshot", QStringLiteral("截图"));
    input_layout->addWidget(screenshot_button_, 0, Qt::AlignVCenter);
    connect(screenshot_button_,
            &QToolButton::clicked,
            this,
            [this]
            {
                if (findChild<screenshot_dialog*>(QString(), Qt::FindDirectChildrenOnly))
                {
                    return;
                }
                auto* dialog = new screenshot_dialog(this);
                auto const visit = conversation_visit_;
                connect(messages_,
                        &QAbstractItemModel::modelReset,
                        dialog,
                        [this, dialog, visit]
                        {
                            if (conversation_visit_ != visit)
                            {
                                dialog->reject();
                            }
                        });
                connect(dialog,
                        &QDialog::accepted,
                        this,
                        [this, dialog, visit]
                        {
                            if (visit == conversation_visit_ && can_send())
                            {
                                send_image(dialog->selected_image());
                            }
                        });
                connect(dialog, &QDialog::finished, dialog, &QObject::deleteLater);
                dialog->request_screenshot();
            });
    mention_button_ = new QToolButton(input_bar);
    mention_button_->setProperty("composeText", true);
    mention_button_->setText(QStringLiteral("@"));
    mention_button_->setObjectName(QStringLiteral("mentionMemberButton"));
    mention_button_->setToolTip(QStringLiteral("提及群成员"));
    mention_button_->setFixedSize(chat_theme::compose_button_width, chat_theme::compose_button_height);
    input_layout->addWidget(mention_button_, 0, Qt::AlignVCenter);
    auto* candidates = new QMenu(mention_button_);
    mention_button_->setMenu(candidates);
    mention_button_->setPopupMode(QToolButton::InstantPopup);
    connect(candidates,
            &QMenu::aboutToShow,
            this,
            [this, candidates]
            {
                candidates->clear();
                if (messages_->can_manage_group())
                {
                    auto* all = candidates->addAction(QStringLiteral("@所有人"));
                    connect(all,
                            &QAction::triggered,
                            this,
                            [this, room = active_conversation_]
                            {
                                if (room == active_conversation_ && can_send() && messages_->can_manage_group())
                                {
                                    mention_all_button_->setChecked(true);
                                }
                            });
                }
                for (auto const& member : messages_->members())
                {
                    if (member.id == self_user_)
                    {
                        continue;
                    }
                    auto const name = user_display_name(member.username, member.nickname, friend_remark(member.id));
                    auto* action = candidates->addAction(name == member.username ? name : QStringLiteral("%1 (@%2)").arg(name, member.username));
                    connect(action,
                            &QAction::triggered,
                            this,
                            [this, id = member.id, room = active_conversation_]
                            {
                                auto const& members = messages_->members();
                                auto it = std::ranges::find(members, id, &member_data::id);
                                if (active_conversation_ != room || it == members.end() || !can_send())
                                {
                                    return;
                                }
                                auto cursor = message_edit_->textCursor();
                                auto prefix = cursor.selectionStart() > 0 && !cursor.document()->characterAt(cursor.selectionStart() - 1).isSpace()
                                                  ? QStringLiteral(" @")
                                                  : QStringLiteral("@");
                                message_edit_->insertPlainText(prefix + it->username + QStringLiteral(" "));
                                message_edit_->setFocus();
                            });
                }
            });
    mention_all_button_ = new QToolButton(input_bar);
    mention_all_button_->setProperty("composeText", true);
    mention_all_button_->setCheckable(true);
    mention_all_button_->setText(QStringLiteral("@所有人 ×"));
    mention_all_button_->setToolTip(QStringLiteral("移除全员提及"));
    mention_all_button_->setObjectName(QStringLiteral("mentionAllTag"));
    input_layout->addWidget(mention_all_button_, 0, Qt::AlignVCenter);
    mention_all_button_->hide();
    connect(mention_all_button_, &QToolButton::toggled, mention_all_button_, &QWidget::setVisible);
    connect(mention_all_button_, &QToolButton::toggled, this, &chat_widget::draft_edited);
    auto* attachments = new QMenu(attachment_button_);
    attachments->addAction(QStringLiteral("发送文件"), this, [this] { choose_attachment(active_conversation_, false, this); });
    create_poll_action_ = attachments->addAction(QStringLiteral("发起投票"),
                                                 this,
                                                 [this]
                                                 {
                                                     if (active_group_ && can_send())
                                                     {
                                                         emit create_poll_requested(active_conversation_);
                                                     }
                                                 });
    attachment_button_->setMenu(attachments);
    attachment_button_->setPopupMode(QToolButton::MenuButtonPopup);
    markdown_button_ = new QToolButton(input_bar);
    markdown_button_->setProperty("composeText", true);
    markdown_button_->setObjectName(QStringLiteral("markdownButton"));
    markdown_button_->setText(QStringLiteral("MD"));
    markdown_button_->setToolTip(QStringLiteral("使用 Markdown 格式发送"));
    markdown_button_->setCheckable(true);
    markdown_button_->setFixedSize(chat_theme::compose_button_width, chat_theme::compose_button_height);
    input_layout->addWidget(markdown_button_, 0, Qt::AlignVCenter);
    connect(markdown_button_, &QToolButton::toggled, this, &chat_widget::draft_edited);
    emoji_button_ = make_composer_button(QStringLiteral("emojiButton"), u"emoji", QStringLiteral("表情"));
    history_button_ = make_composer_button(QStringLiteral("chatHistoryButton"), u"history", QStringLiteral("聊天记录"));
    input_layout->addWidget(emoji_button_, 0, Qt::AlignVCenter);
    input_layout->addWidget(history_button_, 0, Qt::AlignVCenter);
    connect(emoji_button_, &QToolButton::clicked, this, [this] { show_emoji_picker(); });
    connect(history_button_,
            &QToolButton::clicked,
            this,
            [this]
            {
                if (active_conversation_ > 0 && connection_available_)
                {
                    emit chat_history_requested(active_conversation_, active_username_);
                }
            });
    connect(attachment_button_, &QToolButton::clicked, this, [this] { choose_attachment(active_conversation_, false, this); });
    send_button_ = new QToolButton(input_bar);
    send_button_->setObjectName(QStringLiteral("sendButton"));
    send_button_->setIcon(composer_send_icon());
    send_button_->setIconSize(QSize(20, 20));
    send_button_->setToolTip(QStringLiteral("发送消息（Enter）"));
    send_button_->setAccessibleName(QStringLiteral("发送消息"));
    send_button_->setFixedSize(chat_theme::compose_button_width, chat_theme::compose_button_height);
    send_button_->setEnabled(false);
    send_button_->setCursor(Qt::PointingHandCursor);
    input_layout->addWidget(send_button_, 0, Qt::AlignVCenter);
    chat_layout->addWidget(input_bar);
}

QModelIndexList chat_widget::selected_messages() const
{
    auto rows = messages_view_->selectionModel()->selectedIndexes();
    std::ranges::sort(rows, {}, &QModelIndex::row);
    return rows;
}
void chat_widget::set_message_selection(bool enabled)
{
    preserve_reading(
        [&]
        {
            static_cast<message_delegate*>(messages_view_->itemDelegate())->clear_text_selection();
            messages_view_->setSelectionMode(enabled ? QAbstractItemView::ExtendedSelection : QAbstractItemView::NoSelection);
            if (!enabled)
            {
                messages_view_->clearSelection();
            }
            chat_headers_->setCurrentIndex(enabled ? 1 : 0);
        });
    messages_view_->viewport()->update();
}
void chat_widget::update_message_selection()
{
    auto selected = selected_messages();
    selection_count_->setText(QStringLiteral("已选 %1 条").arg(selected.size()));
    selection_copy_->setEnabled(!selected.isEmpty());
    selection_forward_->setEnabled(
        !selected.isEmpty() && connection_available_ &&
        std::ranges::all_of(selected, [](auto const& ix) { return ix.data(message_model::content_role).toString() == QStringLiteral("message"); }));
    bool own = !selected.isEmpty() && std::ranges::all_of(selected, [](auto const& ix) { return ix.data(message_model::outgoing_role).toBool(); });
    selection_retract_->setVisible(own);
    selection_retract_->setEnabled(connection_available_ && own);
    if (selected.isEmpty() && messages_view_->selectionMode() == QAbstractItemView::ExtendedSelection &&
        !static_cast<message_list_view*>(messages_view_)->selecting_range())
    {
        set_message_selection(false);
    }
}

void chat_widget::retract_selected_messages()
{
    QList<QPersistentModelIndex> rows;
    for (auto const& ix : selected_messages())
    {
        rows.append(ix);
    }
    auto room = active_conversation_;
    auto visit = conversation_visit_;
    if (rows.isEmpty() || !connection_available_ ||
        !confirm_action(this,
                        QStringLiteral("撤回所选消息"),
                        QStringLiteral("撤回这 %1 条消息后，所有成员都将看到撤回提示。").arg(rows.size()),
                        QStringLiteral("撤回消息")))
    {
        return;
    }
    if (visit != conversation_visit_ || !connection_available_ ||
        !std::ranges::all_of(rows,
                             [](auto const& ix)
                             { return ix.isValid() && ix.flags().testFlag(Qt::ItemIsSelectable) && ix.data(message_model::outgoing_role).toBool(); }))
    {
        return;
    }
    for (auto const& row : rows)
    {
        emit delete_message_requested(room, row.data(message_model::id_role).toLongLong());
    }
    set_message_selection(false);
}

void chat_widget::copy_selected_messages()
{
    QStringList lines;
    for (auto const& ix : selected_messages())
    {
        lines.append(ix.data(message_model::text_role).toString());
    }
    if (!lines.isEmpty())
    {
        QApplication::clipboard()->setText(lines.join(u'\n'));
    }
}
void chat_widget::show_message_menu(QPoint position)
{
    if (messages_view_->selectionMode() == QAbstractItemView::ExtendedSelection)
    {
        QMenu menu(this);
        resume_reading_after_menu(menu);
        for (auto* button : {selection_copy_, selection_forward_, selection_retract_})
        {
            if (button->isVisible())
            {
                auto* action = menu.addAction(button->icon(), button->text());
                action->setEnabled(button->isEnabled());
                connect(action, &QAction::triggered, button, &QToolButton::click);
            }
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("取消选择"), this, [this] { set_message_selection(false); });
        connect(messages_view_->selectionModel(), &QItemSelectionModel::selectionChanged, &menu, &QMenu::close);
        menu.exec(messages_view_->viewport()->mapToGlobal(position));
        return;
    }
    auto const index = messages_view_->indexAt(position);
    if (index.isValid() && index.data(message_model::send_state_role).toInt() != message_model::sent)
    {
        show_outgoing_menu(index, position);
        return;
    }
    if (!index.isValid() || !connection_available_ || index.data(message_model::deleted_role).toBool())
    {
        return;
    }
    QMenu menu(this);
    resume_reading_after_menu(menu);
    auto const message_id = index.data(message_model::id_role).toLongLong();
    auto const conversation = active_conversation_;
    auto const target_index = QPersistentModelIndex(index);
    auto valid_target = [this, target_index, conversation, message_id, visit = conversation_visit_]
    {
        return visit == conversation_visit_ && conversation == active_conversation_ && message_id > history_floor(conversation) &&
               target_index.isValid() && !target_index.data(message_model::deleted_role).toBool();
    };
    auto const ordinary = index.data(message_model::content_role).toString() == QStringLiteral("message");
    auto* reply = can_send() ? menu.addAction(QStringLiteral("回复")) : nullptr;
    auto* copy = !index.data(message_model::text_role).toString().isEmpty()
                     ? menu.addAction(svg_icon(u"copy", themed("text_secondary")), QStringLiteral("复制"))
                     : nullptr;
    auto* delegate = static_cast<message_delegate*>(messages_view_->itemDelegate());
    auto* copy_selection = delegate->selected_text(index, *messages_view_).isEmpty() ? nullptr : menu.addAction(QStringLiteral("复制选中文字"));
    auto* forward = ordinary ? menu.addAction(QStringLiteral("转发")) : nullptr;
    auto* picker = can_send() ? menu.addMenu(QStringLiteral("表情回应")) : nullptr;
    auto const own_reaction = index.data(message_model::own_reaction_role).toString();
    if (!picker && !own_reaction.isEmpty())
    {
        menu.addAction(QStringLiteral("取消表情回应"),
                       [this, &valid_target, conversation, message_id]
                       {
                           if (connection_available_ && valid_target())
                           {
                               emit reaction_requested(conversation, message_id, {});
                           }
                       });
    }
    for (auto emoji : chat::reaction_choices)
    {
        if (!picker)
        {
            break;
        }
        auto const text = QString::fromUtf8(emoji.data(), static_cast<qsizetype>(emoji.size()));
        auto* action = picker->addAction(text);
        action->setCheckable(true);
        action->setChecked(text == own_reaction);
        connect(action,
                &QAction::triggered,
                &menu,
                [this, &valid_target, target_index, text, conversation]
                {
                    if (can_send() && valid_target())
                    {
                        emit reaction_requested(conversation,
                                                target_index.data(message_model::id_role).toLongLong(),
                                                text == target_index.data(message_model::own_reaction_role).toString() ? QString{} : text);
                    }
                });
    }
    menu.addSeparator();
    QStyleOptionViewItem option;
    option.initFrom(messages_view_);
    option.font = messages_view_->font();
    option.rect = messages_view_->visualRect(index);
    auto const url =
        messages_view_->selectionMode() == QAbstractItemView::ExtendedSelection ? QUrl{} : message_delegate::link_at(option, index, position);
    auto const code = message_delegate::code_at(option, index, position);
    auto* locate_reply = index.data(message_model::reply_target_role).toLongLong() > 0 ? menu.addAction(QStringLiteral("查看被回复的消息")) : nullptr;
    auto* copy_code = code.isEmpty() ? nullptr : menu.addAction(QStringLiteral("复制代码"));
    auto* open_link = url.isEmpty() ? nullptr : menu.addAction(QStringLiteral("打开链接"));
    auto* copy_link = url.isEmpty() ? nullptr : menu.addAction(QStringLiteral("复制链接"));
    auto const filename = index.data(message_model::attachment_name_role).toString();
    auto* download = !filename.isEmpty() ? menu.addAction(QStringLiteral("下载文件")) : nullptr;
    if (download && downloads_)
    {
        downloads_->add_actions(menu, {conversation, message_id, 0, {}});
    }
    auto* preview = index.data(message_model::attachment_type_role).toString().startsWith(QStringLiteral("image/"))
                        ? menu.addAction(QStringLiteral("查看图片"))
                        : nullptr;
    auto poll = target_index.data(message_model::poll_role);
    auto can_write_poll = [&]
    {
        return connection_available_ && valid_target() && poll.isValid() &&
               !target_index.data(message_model::poll_role).value<chat::poll_summary>().closed &&
               !target_index.data(message_model::poll_pending_role).toBool();
    };
    auto can_close_poll = [&]
    {
        auto room = this->conversation(conversation);
        return target_index.data(message_model::outgoing_role).toBool() || (room && room->speech_role != chat::member_role::member);
    };
    auto voted = [&] { return target_index.data(message_model::poll_role).value<chat::poll_summary>().selection != 0; };
    auto* retract_vote = poll.isValid() ? menu.addAction(QStringLiteral("撤回投票")) : nullptr;
    auto* close_poll = poll.isValid() ? menu.addAction(QStringLiteral("结束投票")) : nullptr;
    auto* details =
        ordinary ? nullptr
                 : menu.addAction(index.data(message_model::content_role).toString() == QStringLiteral("poll") ? QStringLiteral("查看结果")
                                                                                                               : QStringLiteral("查看聊天记录"));
    if (poll.isValid())
    {
        auto refresh = [&]
        {
            auto current = target_index.data(message_model::poll_role).value<chat::poll_summary>();
            details->setVisible(has_poll_results(current));
            retract_vote->setVisible(can_write_poll() && voted() && !current.quiz);
            close_poll->setVisible(can_write_poll() && can_close_poll());
        };
        refresh();
        connect(conversations_, &QAbstractItemModel::modelReset, &menu, refresh);
        connect(messages_, &QAbstractItemModel::dataChanged, &menu, refresh);
    }
    menu.addSeparator();
    auto* bookmark = menu.addAction(QStringLiteral("正在查询收藏状态…"));
    bookmark->setEnabled(false);
    emit bookmark_menu_requested(message_id, bookmark);
    auto const current = this->conversation(active_conversation_);
    auto const unpin = current && current->pinned_message.id == message_id;
    auto* group_pin = messages_->can_manage_group() ? menu.addAction(unpin ? QStringLiteral("取消置顶消息") : QStringLiteral("置顶消息")) : nullptr;
    auto* multi = menu.addAction(QStringLiteral("选择消息"));
    auto* readers = index.data(message_model::read_members_role).isValid() ? menu.addAction(QStringLiteral("已读成员")) : nullptr;
    auto const key = index.data(message_model::attachment_key_role).toString();
    if (!key.isEmpty() && index.data(message_model::outgoing_role).toBool() && connection_available_)
    {
        auto* groups = menu.addMenu(QStringLiteral("加入群文件"));
        for (int row = 0; row < conversations_->rowCount(); ++row)
        {
            auto const* group = conversations_->conversation_at(conversations_->index(row, 0));
            if (group && group->group && !group->speech_blocked())
            {
                groups->addAction(group->username,
                                  &menu,
                                  [this, &valid_target, key, id = group->id]
                                  {
                                      auto const target = this->conversation(id);
                                      if (connection_available_ && valid_target() && target && !target->speech_blocked())
                                      {
                                          emit add_file_requested(id, key);
                                      }
                                  });
            }
        }
        groups->menuAction()->setVisible(!groups->isEmpty());
    }
    menu.addSeparator();
    auto* edit = ordinary && can_send() && filename.isEmpty() && index.data(message_model::forwarded_from_role).toString().isEmpty() &&
                         index.data(message_model::outgoing_role).toBool()
                     ? menu.addAction(QStringLiteral("编辑"))
                     : nullptr;
    auto* remove = index.data(message_model::outgoing_role).toBool()
                       ? menu.addAction(svg_icon(u"close", themed("danger")), QStringLiteral("撤回消息"))
                       : nullptr;
    connect(this,
            &chat_widget::history_discarded,
            &menu,
            [&menu, conversation, message_id](qint64 id, qint64 through)
            {
                if (id == conversation && message_id <= through)
                {
                    menu.close();
                }
            });
    auto* selected = menu.exec(messages_view_->viewport()->mapToGlobal(position));
    if (!selected || !valid_target())
    {
        return;
    }
    if (selected == locate_reply)
    {
        auto id = target_index.data(message_model::reply_target_role).toLongLong();
        if (connection_available_ && id > history_floor(conversation))
        {
            jump_to_message(id);
        }
        return;
    }
    if (copy_code && selected == copy_code)
    {
        option.rect = messages_view_->visualRect(target_index);
        if (message_delegate::code_at(option, target_index, position) == code)
        {
            QGuiApplication::clipboard()->setText(code);
        }
        return;
    }
    if (selected && (selected == open_link || selected == copy_link))
    {
        option.rect = messages_view_->visualRect(target_index);
        if (message_delegate::link_at(option, target_index, position) == url)
        {
            if (selected == open_link)
            {
                QDesktopServices::openUrl(url);
            }
            else
            {
                QGuiApplication::clipboard()->setText(url.toString(QUrl::FullyEncoded));
            }
        }
        return;
    }
    if (copy_selection && selected == copy_selection)
    {
        const auto text = delegate->selected_text(target_index, *messages_view_);
        if (!text.isEmpty())
        {
            QGuiApplication::clipboard()->setText(text);
        }
        return;
    }
    if (copy && selected == copy)
    {
        auto const text = target_index.data(message_model::text_role).toString();
        if (!text.isEmpty())
        {
            QGuiApplication::clipboard()->setText(text);
        }
        return;
    }
    if (!connection_available_)
    {
        return;
    }
    if (selected == bookmark)
    {
        emit bookmark_requested(message_id, !bookmark->data().toBool());
        return;
    }
    if (selected == close_poll || selected == retract_vote)
    {
        if (selected == close_poll &&
            (!can_write_poll() || !can_close_poll() ||
             !confirm_action(
                 this, QStringLiteral("结束投票"), QStringLiteral("结束后将保留结果，成员无法继续投票或改票。"), QStringLiteral("结束投票"))))
        {
            return;
        }
        if (can_write_poll() &&
            (selected == close_poll ? can_close_poll() : voted() && !target_index.data(message_model::poll_role).value<chat::poll_summary>().quiz))
        {
            messages_->set_poll_pending(message_id, true);
            emit poll_write_requested(conversation, message_id, selected == close_poll ? -1 : 0);
        }
        return;
    }
    if (details && selected == details &&
        (!poll.isValid() || has_poll_results(target_index.data(message_model::poll_role).value<chat::poll_summary>())))
    {
        emit content_requested(conversation, message_id, target_index.data(message_model::content_role).toString());
        return;
    }
    if (forward && selected == forward)
    {
        choose_forward_target({target_index});
        return;
    }
    if (selected == multi)
    {
        set_message_selection(true);
        messages_view_->setCurrentIndex(target_index);
        messages_view_->selectionModel()->select(target_index, QItemSelectionModel::ClearAndSelect);
        messages_view_->setFocus();
        return;
    }
    if (group_pin && selected == group_pin)
    {
        if (auto const room = this->conversation(conversation);
            room && messages_->can_manage_group() && (!unpin || room->pinned_message.id == message_id))
        {
            emit group_message_pin_requested(conversation, unpin ? std::nullopt : std::optional<qint64>(message_id));
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
        emit attachment_open_requested(conversation, message_id, filename, preview && selected == preview);
        return;
    }
    if (remove && selected == remove)
    {
        if (confirm_action(
                this, QStringLiteral("撤回消息"), QStringLiteral("撤回后，所有成员的聊天记录都将显示撤回提示。"), QStringLiteral("撤回消息")) &&
            connection_available_ && valid_target())
        {
            emit delete_message_requested(conversation, message_id);
        }
        return;
    }
    if (edit && selected == edit)
    {
        if (!can_send() || conversation != active_conversation_)
        {
            return;
        }
        QInputDialog dialog(this);
        dialog.setObjectName(QStringLiteral("editMessageDialog"));
        dialog.setWindowTitle(QStringLiteral("编辑消息"));
        dialog.setLabelText(QStringLiteral("消息内容"));
        dialog.setOption(QInputDialog::UsePlainTextEditForTextInput);
        dialog.setInputMode(QInputDialog::TextInput);
        connect(this,
                &chat_widget::history_discarded,
                &dialog,
                [&dialog, conversation, message_id](qint64 id, qint64 through)
                {
                    if (id == conversation && message_id <= through)
                    {
                        dialog.reject();
                    }
                });
        dialog.setTextValue(target_index.data(message_model::text_role).toString());
        dialog.setOkButtonText(QStringLiteral("保存"));
        dialog.setCancelButtonText(QStringLiteral("取消"));
        dialog.layout()->setContentsMargins(
            chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
        dialog.layout()->setSpacing(chat_theme::dialog_spacing);
        frame_dialog(dialog);
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
        buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
        for (auto* button : buttons->buttons())
        {
            button->setIcon({});
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }
        auto text = dialog.textValue();
        if (!text.isEmpty() && can_send() && valid_target())
        {
            emit edit_message_requested(conversation, message_id, std::move(text));
        }
        return;
    }
    if (!reply || selected != reply || !can_send())
    {
        return;
    }
    auto const target = quote(message_id);
    if (target.deleted)
    {
        return;
    }
    reset_reply(message_id);
    draft_edited();
    show_reply(target);
    message_edit_->setFocus();
}

void chat_widget::connect_chat_controls()
{
    connect(this,
            &chat_widget::profile_received,
            this,
            [this](qint64 id, QString const& login, QString const& nickname)
            {
                messages_->update_member_nickname(id, nickname);
                update_author(id, user_display_name(login, nickname, friend_remark(id)));
            });
    connect(conversation_search_, &QLineEdit::textChanged, this, [this] { filter_conversations(); });
    connect(conversations_,
            &QAbstractItemModel::modelReset,
            this,
            [this]
            {
                filter_conversations();
                update_main_menu();
            });
    connect(conversations_,
            &QAbstractItemModel::dataChanged,
            this,
            [this](QModelIndex const&, QModelIndex const&, QList<int> const& roles)
            {
                if (roles.isEmpty() || roles.contains(Qt::DisplayRole) || roles.contains(conversation_model::username_role) ||
                    roles.contains(conversation_model::last_text_role))
                {
                    filter_conversations();
                }
                update_main_menu();
            });
    connect(self_profile_action_, &QAction::triggered, this, [this] { show_user_details(self_user_, self_username_); });
    connect(&avatars_,
            &avatar_cache::changed,
            this,
            [this](qint64 user)
            {
                if (user == self_user_)
                {
                    self_profile_action_->setIcon(avatar_icon(self_username_, 24, avatars_.image(user)));
                }
                if (user == (active_group_ ? -active_conversation_ : active_peer_))
                {
                    update_chat_header(active_username_);
                }
                for (auto* list : {incoming_friends_, outgoing_friends_})
                {
                    for (int row = 0; row < list->count(); ++row)
                    {
                        auto* item = list->item(row);
                        if (item->data(Qt::UserRole).toLongLong() == user)
                        {
                            item->setData(Qt::DecorationRole, avatars_.image(user));
                        }
                    }
                }
            });
    connect(chats_action_, &QAction::triggered, this, [this] { show_conversations_section(); });
    connect(contacts_action_, &QAction::triggered, this, [this] { show_contacts_dialog(); });
    connect(send_button_, &QToolButton::clicked, this, [this] { send_current_message(); });
    draft_timer_ = new QTimer(this);
    draft_timer_->setSingleShot(true);
    draft_timer_->setInterval(300);
    connect(draft_timer_, &QTimer::timeout, this, &chat_widget::save_active_draft);
    typing_idle_timer_ = new QTimer(this);
    typing_idle_timer_->setSingleShot(true);
    typing_idle_timer_->setInterval(3000);
    connect(typing_idle_timer_,
            &QTimer::timeout,
            this,
            [this]
            {
                if (can_send())
                {
                    emit typing_requested(active_conversation_, false);
                }
            });
    connect(message_edit_,
            &QPlainTextEdit::textChanged,
            this,
            [this]
            {
                draft_edited();
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
                if (!typing_idle_timer_->isActive() || last_typing_sent_.elapsed() >= 2000)
                {
                    last_typing_sent_.start();
                    emit typing_requested(active_conversation_, true);
                }
                typing_idle_timer_->start();
            });
    auto* typing_expiry = new QTimer(this);
    typing_expiry->setInterval(500);
    connect(typing_expiry,
            &QTimer::timeout,
            this,
            [this]
            {
                for (auto it = typing_users_.begin(); it != typing_users_.end();)
                {
                    it = it->expiry.hasExpired() ? typing_users_.erase(it) : std::next(it);
                }
                update_chat_status();
            });
    typing_expiry->start();
    connect(messages_view_->verticalScrollBar(),
            &QScrollBar::valueChanged,
            this,
            [this, last = 0](int value) mutable
            {
                const int previous = std::exchange(last, value);
                if (changing_layout_)
                {
                    return;
                }
                load_visible_images();
                update_reading_navigation();
                mark_visible_messages();
                const int threshold = messages_view_->viewport()->height();
                if (value <= threshold && value < previous)
                {
                    request_older_messages();
                }
                else if (messages_view_->verticalScrollBar()->maximum() - value <= threshold && value > previous)
                {
                    request_newer_messages();
                }
            });
    main_menu_button_->installEventFilter(this);
    contacts_requests_button_->installEventFilter(this);
    for (auto* button : {incoming_accept_button_, incoming_reject_button_, outgoing_cancel_button_})
    {
        button->installEventFilter(this);
    }
}

void chat_widget::set_user(QString const& username, qint64 user, QString const& server)
{
    close_poll_results();
    flush_draft();
    conversation_search_->clear();
    contacts_feedback_->clear();
    contacts_feedback_->hide();
    contacts_dialog_->reject();
    add_contact_dialog_->reject();
    ++add_search_request_;
    stop_typing();
    typing_users_.clear();
    update_chat_status();
    avatars_.clear();
    images_.clear();
    avatar_updating_ = false;
    self_username_ = username;
    self_user_ = user;
    folder_settings_key_ = QStringLiteral("collapsedFolders/%1/%2").arg(QString::fromLatin1(server.toUtf8().toHex())).arg(user);
    QSet<qint64> collapsed;
    for (auto const& id : QSettings().value(folder_settings_key_).toStringList())
    {
        collapsed.insert(id.toLongLong());
    }
    conversations_->set_collapsed(std::move(collapsed));
    conversations_->set_folders({});
    self_profile_action_->setIcon(avatar_icon(username, 24));
    self_profile_action_->setEnabled(user > 0);
    history_floors_.clear();
    reading_positions_.clear();
    draft_timer_->stop();
    drafts_.clear();
    outgoing_.clear();
    saving_text_.reset();
    active_peer_ = 0;
    active_group_ = false;
    reset_reply();
    active_conversation_ = 0;
    history_target_.reset();
    reply_returns_.clear();
    ++conversation_visit_;
    observed_tail_ = requested_read_ = 0;
    tail_loaded_ = false;
    update_reading_navigation();
    update_pinned_message();
    active_username_.clear();
    presence_.clear();
    conversations_->set_conversations({});
    conversations_view_->setCurrentIndex({});
    set_list_status(QStringLiteral("暂无会话"));
    messages_->set_self_user(user);
    set_message_selection(false);
    messages_->reset(0);
    contacts_->set_users({});
    friends_list_->set_account(QStringLiteral("friendGroups/") + folder_settings_key_);
    pending_friend_actions_.clear();
    set_friend_requests({}, {}, {});
    friends_list_->set_query(QString{});
    contact_search_->clear();
    contacts_status_->setText(QStringLiteral("暂无好友"));
    add_users_->set_users({});
    add_user_search_->clear();
    add_users_status_->setText(QStringLiteral("输入用户名后按 Enter 查找"));
    show_conversations_section();
    update_chat_header({});
    set_message_status({});
    message_edit_->clear();
    message_edit_->setEnabled(false);
    send_button_->setEnabled(false);
    message_search_action_->setEnabled(false);
    history_button_->setEnabled(false);
    attachment_button_->setEnabled(false);
    update_compose_state();
}

bool chat_widget::handle_file_drop(QEvent* event)
{
    if (event->type() == QEvent::DragLeave)
    {
        file_drop_hint_->hide();
        return false;
    }
    if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove && event->type() != QEvent::Drop)
    {
        return false;
    }
    auto* drop = static_cast<QDropEvent*>(event);
    if (!drop->mimeData()->hasUrls())
    {
        return false;
    }
    QStringList paths;
    for (auto const& url : drop->mimeData()->urls())
    {
        auto path = local_file_path(url);
        if (!path.isEmpty())
        {
            paths.push_back(std::move(path));
        }
    }
    if (!can_send() || paths.isEmpty() || !(drop->possibleActions() & Qt::CopyAction))
    {
        file_drop_hint_->hide();
        drop->ignore();
        return true;
    }
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    if (event->type() == QEvent::Drop)
    {
        file_drop_hint_->hide();
        send_files(active_conversation_, paths);
    }
    else
    {
        file_drop_hint_->show();
    }
    return true;
}

bool chat_widget::paste_image()
{
    auto const* mime = QGuiApplication::clipboard()->mimeData();
    if (mime == nullptr)
    {
        return false;
    }
    // File managers can offer several image URLs alongside a single preview.
    QStringList paths;
    for (auto const& url : mime->urls())
    {
        auto const path = local_file_path(url);
        auto const suffix = QFileInfo(path).suffix().toLower();
        if (suffix != QStringLiteral("png") && suffix != QStringLiteral("jpg") && suffix != QStringLiteral("jpeg"))
        {
            paths.clear();
            break;
        }
        paths.push_back(path);
    }
    if (!paths.isEmpty())
    {
        send_files(active_conversation_, paths);
        return true;
    }
    if (!(mime->hasImage() || mime->hasFormat(QStringLiteral("image/png")) || mime->hasFormat(QStringLiteral("image/jpeg"))))
    {
        return false;
    }
    if (!can_send())
    {
        set_message_status(QStringLiteral("当前会话暂时无法发送图片"));
        return true;
    }
    auto const data = mime->imageData();
    auto image = qvariant_cast<QImage>(data);
    if (image.isNull())
    {
        for (auto const& format : {QStringLiteral("image/png"), QStringLiteral("image/jpeg")})
        {
            if (mime->hasFormat(format))
            {
                image = QImage::fromData(mime->data(format));
                if (!image.isNull())
                {
                    break;
                }
            }
        }
    }
    if (image.isNull())
    {
        set_message_status(QStringLiteral("无法读取剪贴板图片"));
        return true;
    }
    send_image(image);
    return true;
}
void chat_widget::send_image(QImage const& image)
{
    auto const id = chat::client::new_client_id();
    auto path = save_clipboard_image(image, id);
    if (!path)
    {
        set_message_status(path.error());
        return;
    }
    send_files(active_conversation_, {*path}, id);
}

bool chat_widget::eventFilter(QObject* object, QEvent* event)
{
    if (messages_view_ && object == messages_view_->viewport() && messages_view_->selectionMode() == QAbstractItemView::NoSelection &&
        (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick ||
         event->type() == QEvent::MouseButtonRelease) &&
        static_cast<message_delegate*>(messages_view_->itemDelegate())->select_text(*messages_view_, *static_cast<QMouseEvent*>(event)))
    {
        return true;
    }
    if (messages_view_ && object == messages_view_->viewport() &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick ||
         event->type() == QEvent::ContextMenu))
    {
        auto point = event->type() == QEvent::ContextMenu ? static_cast<QContextMenuEvent*>(event)->pos()
                                                          : static_cast<QMouseEvent*>(event)->position().toPoint();
        auto index = messages_view_->indexAt(point);
        if (index.isValid())
        {
            QStyleOptionViewItem option;
            option.initFrom(messages_view_);
            option.rect = messages_view_->visualRect(index);
            if (!(event->type() == QEvent::MouseButtonRelease && messages_view_->selectionMode() == QAbstractItemView::ExtendedSelection) &&
                message_delegate::on_separator(option, index, point))
            {
                return true;
            }
        }
    }
    if (messages_view_ && object == messages_view_->viewport() && event->type() == QEvent::Wheel && messages_ready() && connection_available_)
    {
        auto* wheel = static_cast<QWheelEvent*>(event);
        auto* scroll = messages_view_->verticalScrollBar();
        if (!history_exhausted_ && scroll->value() == scroll->minimum() && (wheel->pixelDelta().y() > 0 || wheel->angleDelta().y() > 0))
        {
            request_older_messages();
            return true;
        }
        if (!tail_loaded_ && scroll->value() == scroll->maximum() && (wheel->pixelDelta().y() < 0 || wheel->angleDelta().y() < 0))
        {
            request_newer_messages();
            return true;
        }
    }
    if (object == input_bar_ && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
    {
        auto* rows = static_cast<QBoxLayout*>(input_bar_->layout());
        auto const margins = rows->contentsMargins();
        auto const required = margins.left() + rows->itemAt(0)->minimumSize().width() + rows->spacing() +
                              rows->itemAt(1)->minimumSize().width() + margins.right();
        // Keep the same editor and buttons when the visible tools need a second row.
        rows->setDirection(required > input_bar_->contentsRect().width() ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    }
    // The scroll areas deliver drag events to their viewport children.
    if ((object == input_bar_ || object->parent() == messages_view_ || object->parent() == message_edit_) && handle_file_drop(event))
    {
        return true;
    }
    if (object == message_edit_ && event->type() == QEvent::FocusOut && static_cast<QFocusEvent*>(event)->reason() == Qt::PopupFocusReason)
    {
        // Hook the editor's own standard menu, not Chat menus that already have a close handler.
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu && (menu->parentWidget() == message_edit_ || menu->parentWidget() == message_edit_->viewport()))
        {
            resume_reading_after_menu(*menu);
        }
    }
    if ((object == contacts_requests_button_ || object == incoming_accept_button_ || object == incoming_reject_button_ ||
         object == outgoing_cancel_button_) &&
        event->type() == QEvent::KeyPress)
    {
        auto const* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            static_cast<QAbstractButton*>(object)->click();
            return true;
        }
    }
    if (object == main_menu_button_ && event->type() == QEvent::KeyPress)
    {
        auto const* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            main_menu_button_->showMenu();
            return true;
        }
    }
    if (object == add_users_view_ && event->type() == QEvent::KeyPress)
    {
        auto const* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            select_add_user(add_users_view_->currentIndex());
            return true;
        }
    }
    if ((object == incoming_friends_ || object == outgoing_friends_) && event->type() == QEvent::KeyPress)
    {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            auto* item = static_cast<QListWidget*>(object)->currentItem();
            if (item)
            {
                show_user_details(item->data(Qt::UserRole).toLongLong(), item->data(Qt::UserRole + 1).toString());
            }
            return true;
        }
    }
    if (object == message_edit_ && event->type() == QEvent::KeyPress)
    {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Paste) && paste_image())
        {
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            if (!message_edit_->textCursor().block().layout()->preeditAreaText().isEmpty())
            {
                QGuiApplication::inputMethod()->commit();
                return true;
            }
            auto const modifiers = key->modifiers();
            if (!(modifiers & (Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier)) &&
                (!control_enter_send_ || (modifiers & Qt::ControlModifier)))
            {
                send_current_message();
                return true;
            }
        }
    }
    if (object == messages_view_ && event->type() == QEvent::KeyPress && messages_view_->selectionMode() == QAbstractItemView::NoSelection)
    {
        auto const* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Copy))
        {
            auto* delegate = static_cast<message_delegate*>(messages_view_->itemDelegate());
            const auto text = delegate->selected_text(delegate->text_index(), *messages_view_);
            if (!text.isEmpty())
            {
                QApplication::clipboard()->setText(text);
                return true;
            }
        }
        if (key->modifiers() == Qt::NoModifier && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter))
        {
            auto index = messages_view_->currentIndex();
            auto kind = index.data(message_model::content_role).toString();
            auto poll = index.data(message_model::poll_role).value<chat::poll_summary>();
            if (kind == QStringLiteral("bundle") || (kind == QStringLiteral("poll") && has_poll_results(poll)))
            {
                emit content_requested(active_conversation_, index.data(message_model::id_role).toLongLong(), kind);
                return true;
            }
        }
        if (connection_available_ && active_conversation_ > 0 && key->key() == Qt::Key_End &&
            (key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::ControlModifier))
        {
            return_to_latest();
            return true;
        }
        if (key->modifiers() == Qt::NoModifier && (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown))
        {
            if (key->key() == Qt::Key_PageUp && messages_view_->verticalScrollBar()->value() == messages_view_->verticalScrollBar()->minimum())
            {
                request_older_messages();
            }
            if (key->key() == Qt::Key_PageDown && messages_view_->verticalScrollBar()->value() == messages_view_->verticalScrollBar()->maximum())
            {
                request_newer_messages();
            }
            messages_view_->verticalScrollBar()->triggerAction(key->key() == Qt::Key_PageUp ? QAbstractSlider::SliderPageStepSub
                                                                                            : QAbstractSlider::SliderPageStepAdd);
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress && messages_view_->selectionMode() == QAbstractItemView::ExtendedSelection &&
        (object == messages_view_ || selection_bar_->isAncestorOf(qobject_cast<QWidget*>(object))))
    {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape)
        {
            set_message_selection(false);
            return true;
        }
        if (key->matches(QKeySequence::Copy))
        {
            copy_selected_messages();
            return true;
        }
    }
    if (object == messages_view_ && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->matches(QKeySequence::Copy))
    {
        auto const current = messages_view_->currentIndex();
        auto const text = current.data(message_model::text_role).toString();
        if (current.isValid() && !current.data(message_model::deleted_role).toBool() && !text.isEmpty())
        {
            QGuiApplication::clipboard()->setText(text);
        }
        return true;
    }
    if (object == messages_view_->viewport() && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
    {
        update_reading_navigation();
        QTimer::singleShot(0,
                           messages_view_,
                           [this]
                           {
                               load_visible_images();
                               mark_visible_messages();
                           });
    }
    return QWidget::eventFilter(object, event);
}

void chat_widget::load_visible_images()
{
    if (!connection_available_ || active_conversation_ <= 0 || messages_->rowCount() == 0)
    {
        return;
    }
    auto const first = messages_view_->indexAt(QPoint(0, 0));
    auto const last = messages_view_->indexAt(QPoint(0, messages_view_->viewport()->height() - 1));
    for (int row = first.isValid() ? first.row() : 0; row <= (last.isValid() ? last.row() : messages_->rowCount() - 1); ++row)
    {
        auto const index = messages_->index(row, 0);
        if (!messages_view_->visualRect(index).intersects(messages_view_->viewport()->rect()))
        {
            continue;
        }
        if (index.data(message_model::attachment_type_role).toString().startsWith(QStringLiteral("image/")))
        {
            images_.observe(active_conversation_, index.data(message_model::id_role).toLongLong());
        }
    }
}

void chat_widget::set_loading()
{
    contacts_status_->setText(QStringLiteral("正在加载好友…"));
    contacts_status_->show();
    update_request_actions();
    set_list_status(QStringLiteral("正在加载…"));
}

void chat_widget::set_error(QString message)
{
    // Operation feedback outlives list refreshes, which own conversations_status_.
    if (contacts_dialog_->isVisible())
    {
        contacts_feedback_->setVisible(!message.isEmpty());
        contacts_feedback_->show_error(message);
    }
    notice_->setVisible(!message.isEmpty());
    notice_->show_error(std::move(message));
    notice_timer_->start();
}

void chat_widget::set_conversations_error(QString message)
{
    set_list_status(std::move(message));
}

// The status row takes space only while it has something to say.
void chat_widget::set_list_status(QString text)
{
    conversations_status_->setText(std::move(text));
    conversations_status_->setVisible(!conversations_status_->text().isEmpty());
}

void chat_widget::set_connection_available(bool available)
{
    if (!available)
    {
        messages_->clear_poll_requests();
    }
    if (connection_available_ == available)
    {
        return;
    }
    if (!available)
    {
        const auto* scroll = messages_view_->verticalScrollBar();
        if (!history_target_ && messages_loaded_ && tail_loaded_ && scroll->value() == scroll->maximum())
        {
            history_target_ = history_target{history_page::latest};
        }
        stop_typing();
        set_presences({});
        typing_users_.clear();
        update_chat_status();
        for (auto& item : outgoing_)
        {
            if (item.path.isEmpty())
            {
                item.waiting = !item.failed;
            }
            else if (!item.failed)
            {
                item.failed = true;
                item.error = QStringLiteral("连接已断开，上传已暂停");
            }
        }
        show_outgoing();
        if (std::ranges::any_of(outgoing_, [](outgoing const& item) { return !item.failed; }))
        {
            set_message_status(QStringLiteral("连接已断开，未发送的消息会在重新连接后自动发送"));
        }
    }
    if (!available && avatar_updating_)
    {
        finish_avatar_update(QStringLiteral("连接已断开，请重新上传头像。"));
    }
    connection_available_ = available;
    update_message_selection();
    emit friendship_updated();
    images_.retry();
    if (available)
    {
        load_draft_reply();
        QTimer::singleShot(0, messages_view_, [this] { load_visible_images(); });
    }
    if (!available && active_group_)
    {
        invalidate_members(active_conversation_);
    }
    for (auto* action : findChildren<QAction*>())
    {
        if (action->objectName() == QStringLiteral("globalSearchAction") || action->objectName() == QStringLiteral("mentionsMeAction") ||
            action->objectName() == QStringLiteral("addFriendAction") || action->objectName() == QStringLiteral("createGroupAction") ||
            action->objectName() == QStringLiteral("joinGroupAction"))
        {
            action->setEnabled(available);
        }
    }
    set_messages_loading(available && active_conversation_ > 0);
    if (!available)
    {
        requested_read_ = 0;
    }
    update_reading_navigation();
    update_compose_state();
    message_search_action_->setEnabled(available && active_conversation_ > 0);
    history_button_->setEnabled(available && active_conversation_ > 0);
    update_pinned_message();
    update_request_actions();
    add_user_search_->setEnabled(available);
    contacts_add_button_->setEnabled(available);
    add_search_button_->setEnabled(available && !add_user_search_->text().trimmed().isEmpty());
    if (!available)
    {
        ++add_search_request_;
        add_users_->set_users({});
        add_users_view_->hide();
        add_users_status_->setText(QStringLiteral("当前离线，连接恢复后可搜索用户"));
    }
    else if (add_users_->rowCount() == 0)
    {
        add_users_status_->setText(QStringLiteral("输入用户名后按 Enter 或点击搜索"));
    }
}

void chat_widget::set_connection_status(QString text, bool retry_enabled)
{
    connection_status_->setText(std::move(text));
    connection_status_->setEnabled(retry_enabled);
    connection_status_->setCursor(retry_enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
    connection_status_->setVisible(active_conversation_ > 0 && !connection_status_->text().isEmpty());
}

void chat_widget::set_conversations(QList<conversation_data> conversations)
{
    auto const previous_user = active_conversation_;
    // Activity can move an existing conversation across the cursor between pages.
    if (previous_user > 0 && std::ranges::find(conversations, previous_user, &conversation_data::id) == conversations.end())
    {
        if (auto current = conversation(previous_user))
        {
            conversations.push_back(std::move(*current));
        }
    }
    if (connection_available_)
    {
        bool paused = false;
        for (auto& pending : outgoing_)
        {
            if (pending.path.isEmpty())
            {
                continue;
            }
            auto const target = std::ranges::find(conversations, pending.conversation, &conversation_data::id);
            if (target == conversations.end())
            {
                continue;
            }
            if (pending.unavailable)
            {
                pending.unavailable = false;
                pending.error = QStringLiteral("上传已暂停");
            }
            if (!pending.failed && target->speech_blocked())
            {
                paused = pending.failed = true;
                pending.error = QStringLiteral("群聊禁言，上传已暂停");
                emit attachment_interrupt_requested(pending.client_id, false, pending.path);
            }
        }
        if (paused)
        {
            show_outgoing();
        }
    }
    conversations_->set_conversations(std::move(conversations));
    update_reading_navigation();
    update_compose_state();
    update_pinned_message();
    for (auto const& item : presence_)
    {
        conversations_->set_online(item.user, item.online);
    }
    if (conversations_->rowCount() == 0)
    {
        set_list_status(QStringLiteral("暂无会话"));
        conversations_view_->setCurrentIndex({});
        return;
    }

    filter_conversations();
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
            update_chat_header(item->username);
        }
        return;
    }

    // Opening a conversation reports it read; only the user's own choice may do that.
    conversations_view_->setCurrentIndex({});
    conversations_view_->clearSelection();
}

void chat_widget::update_author(qint64 user, QString const& name)
{
    messages_->update_author(user, name);
    if (reply_snapshot_.from == user)
    {
        reply_snapshot_.username = name;
        reply_preview_->setText(QStringLiteral("回复 %1：%2").arg(name, grapheme_prefix(reply_snapshot_.text, 80)));
    }
}

void chat_widget::set_contacts(QList<user_data> contacts)
{
    auto const selected = contacts_view_->currentIndex().data(user_model::id_role).toLongLong();
    auto const scroll = contacts_view_->verticalScrollBar()->value();
    QHash<qint64, QString> previous_names;
    for (int i = 0; i < contacts_->rowCount(); ++i)
    {
        auto const* previous = contacts_->user_at(contacts_->index(i, 0));
        previous_names.insert(previous->id, previous->display_name());
    }
    for (auto const& contact : contacts)
    {
        auto const name = contact.display_name();
        if (previous_names.take(contact.id) != name)
        {
            update_author(contact.id, name);
        }
    }
    for (auto id : previous_names.keys())
    {
        // A former contact's signature and current public name depend on shared group membership.
        emit profile_requested(id);
    }
    contacts_->set_users(std::move(contacts));
    if (active_conversation_ > 0 && !active_group_ && !is_contact(active_peer_))
    {
        leave_conversation(active_conversation_);
    }
    contacts_status_->setText(contacts_->rowCount() == 0 ? QStringLiteral("暂无好友") : QString{});
    if (selected > 0)
    {
        for (int row = 0; row < friends_list_->rowCount(); ++row)
        {
            auto const index = friends_list_->index(row, 0);
            if (index.data(user_model::id_role).toLongLong() == selected)
            {
                contacts_view_->setCurrentIndex(index);
                break;
            }
        }
    }
    contacts_view_->verticalScrollBar()->setValue(scroll);
    for (auto it = presence_.begin(); it != presence_.end();)
    {
        if (!is_contact(it.key()))
        {
            conversations_->set_online(it.key(), false);
            it = presence_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto const& item : presence_)
    {
        contacts_->set_presence(item.user, item.online, item.last_seen);
    }
    filter_contacts(contact_search_->text());
    update_compose_state();
    update_request_actions();
    emit friendship_updated();
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
    update_chat_status();
}

void chat_widget::set_presence(presence_data user)
{
    if (user.user <= 0 || !is_contact(user.user))
    {
        return;
    }

    presence_.insert(user.user, user);
    const auto stopped_typing = !user.online && typing_users_.remove(user.user);
    if (stopped_typing || user.user == active_peer_)
    {
        update_chat_status();
    }
    conversations_->set_online(user.user, user.online);
    contacts_->set_presence(user.user, user.online, user.last_seen);
}

void chat_widget::set_contacts_error(QString message)
{
    contacts_status_->setText(std::move(message));
    contacts_status_->setVisible(!contacts_status_->text().isEmpty());
}

void chat_widget::set_add_contact_search_results(QList<user_data> users, quint64 request)
{
    if (!add_contact_dialog_->isVisible() || request != add_search_request_)
    {
        return;
    }
    add_users_->set_users(std::move(users));
    add_users_status_->setText(add_users_->rowCount() == 0
                                   ? QStringLiteral("没有找到用户名以“%1”开头的用户，请确认用户名").arg(add_user_search_->text().trimmed())
                                   : QStringLiteral("选择用户查看资料和好友状态"));
    add_users_view_->setVisible(add_users_->rowCount() > 0);
    add_users_view_->setFixedHeight(std::min(add_users_->rowCount(), 6) * chat_theme::user_row_height);
    add_contact_dialog_->adjustSize();
}

void chat_widget::set_add_contact_search_error(QString message, quint64 request)
{
    if (!add_contact_dialog_->isVisible() || request != add_search_request_)
    {
        return;
    }
    add_users_->set_users({});
    add_users_view_->hide();
    add_users_status_->setText(std::move(message));
    add_contact_dialog_->adjustSize();
}

void chat_widget::finish_add_contact(qint64 user, QString error)
{
    auto const pending = pending_friend_actions_.take(user);
    emit contact_add_finished(user, error);
    if (!error.isEmpty())
    {
        set_error(error);
    }
    else if (!pending.second.isEmpty())
    {
        auto const action = pending.first;
        auto const name = pending.second;
        if (action < 0)
        {
            set_error(QStringLiteral("已拒绝 %1 的好友申请").arg(name));
        }
        else if (action == static_cast<int>(chat::friendship_state::incoming_pending))
        {
            set_error(QStringLiteral("已添加 %1 为好友").arg(name));
        }
        else if (action == static_cast<int>(chat::friendship_state::outgoing_pending))
        {
            set_error(QStringLiteral("已撤回给 %1 的好友申请").arg(name));
        }
        else
        {
            set_error(QStringLiteral("已向 %1 发送好友申请，等待对方验证").arg(name));
        }
    }
    if (error.isEmpty() && incoming_requests_.isEmpty() && outgoing_requests_.isEmpty() && pending_friend_actions_.isEmpty() &&
        contacts_pages_->currentIndex() == 1)
    {
        show_contacts_home();
    }
    update_request_actions();
    emit friendship_updated();
}

void chat_widget::set_friend_requests(QList<user_data> incoming, QList<user_data> outgoing, QString error)
{
    if (!error.isEmpty())
    {
        friend_requests_status_->setText(error);
        friend_requests_status_->show();
        return;
    }
    incoming_requests_ = std::move(incoming);
    outgoing_requests_ = std::move(outgoing);

    auto populate = [this](QListWidget* list, QList<user_data> const& users, QString const& hint)
    {
        auto const selected_user = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : 0;
        auto const selected_row = list->currentRow();
        auto const scroll_position = list->verticalScrollBar()->value();
        QSignalBlocker const blocked(list);
        list->clear();
        for (auto const& user : users)
        {
            avatars_.observe(user.id, user.avatar);
            auto* item = new QListWidgetItem(user.display_name(), list);
            item->setData(Qt::UserRole, user.id);
            item->setData(Qt::UserRole + 1, user.username);
            item->setData(Qt::StatusTipRole, hint);
            item->setData(Qt::DecorationRole, avatars_.image(user.id));
            item->setToolTip(user.username + QStringLiteral(" · ") + hint);
            if (user.id == selected_user)
            {
                list->setCurrentItem(item);
            }
        }
        if (!list->currentItem() && selected_row >= 0 && list->count() > 0)
        {
            list->setCurrentRow(std::min(selected_row, list->count() - 1));
        }
        list->setMaximumHeight(list->count() * chat_theme::user_row_height);
        auto* group = list->parentWidget();
        group->setVisible(!users.isEmpty());
        group->setMaximumHeight(group->layout()->sizeHint().height());
        list->doItemsLayout();
        list->verticalScrollBar()->setValue(scroll_position);
    };
    populate(incoming_friends_, incoming_requests_, QStringLiteral("请求添加你为好友"));
    populate(outgoing_friends_, outgoing_requests_, QStringLiteral("等待对方确认"));
    incoming_friends_title_->setText(QStringLiteral("收到的申请 · %1").arg(incoming_requests_.size()));
    outgoing_friends_title_->setText(QStringLiteral("发出的申请 · %1").arg(outgoing_requests_.size()));
    auto const incoming_count = incoming_requests_.size();
    auto const outgoing_count = outgoing_requests_.size();
    auto const has_requests = incoming_count > 0 || outgoing_count > 0;
    contacts_requests_button_->setVisible(has_requests);
    friend_requests_status_->setText(has_requests ? QString{} : QStringLiteral("暂无好友申请"));
    friend_requests_status_->setAlignment(Qt::AlignCenter);
    friend_requests_status_->setVisible(!has_requests);
    contacts_requests_badge_->setText(incoming_count > 99 ? QStringLiteral("99+") : QString::number(incoming_count));
    contacts_requests_badge_->setVisible(incoming_count > 0);
    contacts_requests_summary_->setText(incoming_count > 0   ? outgoing_count > 0
                                                                   ? QStringLiteral("%1 条待处理 · %2 条已发送").arg(incoming_count).arg(outgoing_count)
                                                                   : QStringLiteral("%1 条待处理").arg(incoming_count)
                                          : outgoing_count > 0 ? QStringLiteral("%1 条已发送，等待确认").arg(outgoing_count)
                                                             : QStringLiteral("查看收到和已发送的申请"));
    contacts_requests_button_->setAccessibleDescription(contacts_requests_summary_->text());
    update_main_menu();
    update_request_actions();
    update_compose_state();
    emit friendship_updated();
}

chat::friendship_state chat_widget::friend_state(qint64 user) const
{
    if (is_contact(user))
    {
        return chat::friendship_state::accepted;
    }
    for (auto const& request : incoming_requests_)
    {
        if (request.id == user)
        {
            return chat::friendship_state::incoming_pending;
        }
    }
    for (auto const& request : outgoing_requests_)
    {
        if (request.id == user)
        {
            return chat::friendship_state::outgoing_pending;
        }
    }
    return chat::friendship_state::none;
}

void chat_widget::set_messages(qint64 user,
                               QList<message_data> messages,
                               read_positions positions,
                               history_page page,
                               bool has_more,
                               bool has_newer,
                               std::optional<qint64> first_unread)
{
    if (user != active_conversation_)
    {
        return;
    }

    if (page == history_page::unread && messages_loaded_ && first_unread && *first_unread > effective_read_position() &&
        *first_unread < messages_->first_message_id())
    {
        set_messages_loading(false);
        return_to_latest();
        return;
    }
    const bool position_page = page == history_page::latest || page == history_page::unread || page == history_page::around;
    const auto previous = capture_reading_position();
    const auto previous_first = messages_->first_message_id();
    const auto previous_tail = messages_->last_message_id();
    const auto page_end = messages.isEmpty() ? 0 : messages.back().id;
    {
        QScopedValueRollback guard(changing_layout_, true);
        for (auto const& message : messages)
        {
            settle(message);
        }
        for (auto it = messages.begin(); it != messages.end();)
        {
            it = visible_message(*it) ? std::next(it) : messages.erase(it);
        }
        const auto first = messages.isEmpty() ? 0 : messages.front().id;
        const auto last = messages.isEmpty() ? 0 : messages.back().id;
        const bool replaces = position_page && first && previous_first && (first > previous_tail || last < previous_first);
        if (replaces)
        {
            // A navigation jump can skip entire pages. Keep the new interval separate from the old one.
            messages_->discard_stored_rows();
        }
        messages_->merge_messages(std::move(messages));
        messages_->set_read_positions(std::move(positions));
        if (page == history_page::older || (position_page && (replaces || !previous_first || first <= previous_first)))
        {
            history_exhausted_ = !has_more;
        }
        if (page != history_page::older && (replaces || !has_newer || last >= previous_tail))
        {
            tail_loaded_ = !has_newer && observed_tail_ <= messages_->last_message_id();
        }
        show_outgoing();
    }
    QModelIndex anchor;
    if (page == history_page::unread && first_unread)
    {
        anchor = messages_->next_incoming(std::max(*first_unread - 1, effective_read_position()));
        if (!anchor.isValid() && !tail_loaded_)
        {
            // A concurrent clear, withdrawal or another device's receipt overtook the entry snapshot.
            messages_->discard_stored_rows();
            request_navigation({history_page::unread});
            return;
        }
    }
    if (position_page && !anchor.isValid())
    {
        messages_->set_unread_boundary(0);
    }
    messages_view_->doItemsLayout();
    if (anchor.isValid())
    {
        scroll_to_unread(anchor);
    }
    else if (page == history_page::around && history_target_)
    {
        auto target = std::exchange(history_target_, std::nullopt)->position;
        auto index = messages_->message_index(target.message);
        if (index.isValid() && !index.data(message_model::deleted_role).toBool())
        {
            restore_reading_position(target);
        }
        else
        {
            request_entry_history();
            return;
        }
    }
    else if (position_page || (history_target_ && history_target_->page == history_page::latest))
    {
        messages_view_->scrollToBottom();
    }
    else
    {
        restore_reading_position(previous);
    }
    if (history_target_ && (history_target_->page == history_page::unread || (history_target_->page == history_page::latest && tail_loaded_)))
    {
        history_target_.reset();
    }
    messages_loaded_ = true;
    set_message_status(messages_->rowCount() == 0 ? QStringLiteral("暂无消息") : QString{});
    // Refresh changed rows inside the loaded interval. Ordinary downward paging stops after one page.
    if (page == history_page::refresh && has_more && page_end > 0 && page_end < previous_tail)
    {
        set_messages_loading(true);
        emit history_requested(user, history_page::refresh, page_end);
        return;
    }
    set_messages_loading(false);
    if (!tail_loaded_ && ((history_target_ && history_target_->page == history_page::latest) || !messages_->last_message_id() ||
                          (page != history_page::older && !has_newer && observed_tail_ > page_end)))
    {
        request_newer_messages();
    }
    update_reading_navigation();
    mark_visible_messages();
}

void chat_widget::locate_message(qint64 conversation, qint64 message)
{
    if (message <= 0)
    {
        return;
    }
    if (conversation == active_conversation_ && message > history_floor(conversation))
    {
        if (messages_->message_index(message).isValid())
        {
            jump_to_message(message);
            return;
        }
    }
    emit message_context_requested(conversation, message);
}

void chat_widget::set_read_message(qint64 conversation, qint64 user, qint64 message)
{
    if (conversation == active_conversation_)
    {
        messages_->set_read_message(user, message);
        update_reading_navigation();
    }
}

void chat_widget::invalidate_members(qint64 conversation)
{
    if (conversation == active_conversation_ && active_group_)
    {
        // Reads can arrive before the replacement roster. Keep their watermarks,
        // but wait for the latest join boundaries before using them as receipts.
        messages_->set_members({});
    }
}

void chat_widget::finish_poll_write(qint64 room, qint64 message, QString error)
{
    messages_->set_poll_pending(message, false);
    if (!messages_->message_index(message).data(message_model::poll_role).value<chat::poll_summary>().multiple)
    {
        messages_->set_poll_draft(message, 0);
    }
    if (active_conversation_ == room && !error.isEmpty())
    {
        set_error(error);
    }
}

void chat_widget::update_poll(chat::poll_update const& update)
{
    if (update.conversation != active_conversation_ || update.message <= history_floor(update.conversation))
    {
        return;
    }
    messages_->set_poll(update.message, update.summary);
}

void chat_widget::add_message(qint64 user, message_data message)
{
    if (!visible_message(message) || user != active_conversation_)
    {
        settle(message);
        return;
    }

    observed_tail_ = std::max(observed_tail_, message.id);
    if (!tail_loaded_ || !messages_loaded_)
    {
        settle(message);
        return;
    }

    // A fresh unread run gets a fresh marker, even during the same visit.
    auto const read = effective_read_position();
    bool const new_run = !messages_->unread_boundary() || !messages_->next_incoming(read).isValid();
    const bool at_end = viewing_latest();
    const bool clear_boundary = new_run && at_end && message.id > read;
    auto const boundary = new_run && !at_end && message.from != self_user_ && !message.deleted && message.id > read ? message.id : 0;
    auto const poll = message.poll ? message.id : 0;
    preserve_reading(
        [&]
        {
            settle(message);
            if (!messages_->add_message(std::move(message)))
            {
                return;
            }
            if (clear_boundary)
            {
                messages_->set_unread_boundary(0);
            }
            if (boundary)
            {
                messages_->set_unread_boundary(boundary);
            }
            if (poll)
            {
                emit live_poll_added(user, poll);
            }
            set_message_status({});
        });
    mark_visible_messages();
}

void chat_widget::update_message(message_data message)
{
    if (message.deleted)
    {
        discard_draft_reply(message.conversation, message.id, true);
    }
    if (!visible_message(message))
    {
        return;
    }
    // A withdrawal can precede delivery of the original message in the live timeline.
    if (message.deleted && message.conversation == active_conversation_ && tail_loaded_ && message.id >= messages_->first_message_id())
    {
        messages_->add_message(message);
    }
    messages_->update_message(message);
    if (message.conversation != active_conversation_)
    {
        return;
    }
    if (message.deleted)
    {
        reply_returns_.removeIf([&](const reading_position& p) { return p.message == message.id; });
    }
    if (message.deleted && message.id == messages_->unread_boundary())
    {
        messages_->set_unread_boundary(messages_->next_incoming(message.id).data(message_model::id_role).toLongLong());
        update_reading_navigation();
    }
    if (reply_to_ == message.id)
    {
        show_reply({message.id, message.username, grapheme_prefix(message.text, 160), message.edited_at, message.deleted, message.from});
    }
}

bool chat_widget::set_reactions(qint64 conversation, qint64 message, qint64 revision, QList<reaction_data> reactions)
{
    return conversation != active_conversation_ || message <= history_floor(conversation) ||
           messages_->set_reactions(message, revision, std::move(reactions)) || !tail_loaded_;
}

void chat_widget::finish_send(qint64 conversation, qint64 client_id, message_data message, QString error, send_status status)
{
    if (status == send_status::unavailable)
    {
        drop_outgoing(conversation);
        if (conversation == active_conversation_)
        {
            show_outgoing();
        }
        return;
    }
    if (status == send_status::sent)
    {
        // A history page may have shown it already: merge by version first, then insert it if new.
        // settle() through add_message() takes it off the unconfirmed list.
        update_message(message);
        add_message(conversation, std::move(message));
        return;
    }
    auto found = std::ranges::find(outgoing_, client_id, &outgoing::client_id);
    if (found == outgoing_.end())
    {
        return;
    }
    found->failed = true;
    if (!found->unavailable)
    {
        found->error = error;
    }
    if (conversation != active_conversation_)
    {
        return;
    }
    show_outgoing();
    set_message_error(conversation, QStringLiteral("%1。可在对应文件上继续或取消上传。").arg(error));
}

void chat_widget::restore_attachment(qint64 conversation, qint64 id, QString path, qint64 size, qint64 reply, qint64 confirmed, bool group_only)
{
    if (std::ranges::find(outgoing_, id, &outgoing::client_id) != outgoing_.end())
    {
        return;
    }
    outgoing item;
    item.client_id = id;
    item.conversation = conversation;
    item.filename = QFileInfo(path).fileName();
    item.path = std::move(path);
    item.size = size;
    item.reply.id = reply;
    item.group_only = group_only;
    item.failed = true;
    item.error = QStringLiteral("已暂停，已确认 %1 / %2 字节").arg(confirmed).arg(size);
    outgoing_.push_back(std::move(item));
    emit uploads_changed();
    if (active_conversation_ == conversation)
    {
        show_outgoing();
    }
}
void chat_widget::attachment_progress(qint64 id, qint64 bytes, qint64 total)
{
    auto item = std::ranges::find(outgoing_, id, &outgoing::client_id);
    if (item == outgoing_.end() || item->failed)
    {
        return;
    }
    auto label = QStringLiteral("正在上传 %1%").arg(total ? 100 * bytes / total : 100);
    if (item->error != label)
    {
        item->error = std::move(label);
        if (item->group_only)
        {
            emit uploads_changed();
        }
        if (item->conversation == active_conversation_)
        {
            show_outgoing();
        }
    }
}
void chat_widget::attachment_cancelled(qint64 id)
{
    if (outgoing_.removeIf([&](outgoing const& item) { return item.client_id == id; }))
    {
        show_outgoing();
    }
}
void chat_widget::set_message_error(qint64 user, QString message)
{
    if (user == active_conversation_)
    {
        set_message_status(std::move(message));
    }
}

void chat_widget::set_history_error(qint64 conversation, QString message)
{
    if (conversation == active_conversation_)
    {
        set_messages_loading(false);
        update_reading_navigation();
        const auto target = std::exchange(history_target_, std::nullopt);
        if (target && target->page == history_page::around && !messages_loaded_)
        {
            request_entry_history();
        }
        set_message_status(std::move(message));
    }
}

qint64 chat_widget::active_conversation() const noexcept { return active_conversation_; }

qint64 chat_widget::latest_message_id() const { return messages_->last_message_id(); }

void chat_widget::show_conversations_section()
{
    update_main_menu();
    conversation_search_->setFocus();
}

void chat_widget::show_contacts_home()
{
    contacts_pages_->setCurrentIndex(0);
    contacts_dialog_->setWindowTitle(QStringLiteral("好友"));
    if (contacts_dialog_->isVisible())
    {
        contact_search_->setFocus();
    }
}

void chat_widget::show_friend_requests()
{
    contacts_pages_->setCurrentIndex(1);
    contacts_dialog_->setWindowTitle(QStringLiteral("好友申请"));
    for (auto* list : {incoming_friends_, outgoing_friends_})
    {
        if (list->count() > 0 && !list->currentItem())
        {
            list->setCurrentRow(0);
        }
    }
    incoming_friends_->setFocus();
}

void chat_widget::show_contacts_dialog()
{
    if (contacts_dialog_->isVisible())
    {
        contacts_dialog_->raise();
        contacts_dialog_->activateWindow();
        return;
    }
    show_contacts_home();
    contact_search_->clear();
    contacts_dialog_->open();
    contact_search_->setFocus();
}

void chat_widget::show_add_contact_dialog()
{
    if (add_contact_dialog_->isVisible())
    {
        add_contact_dialog_->raise();
        add_contact_dialog_->activateWindow();
        return;
    }
    add_contact_dialog_->setParent(contacts_dialog_->isVisible() ? contacts_dialog_ : static_cast<QWidget*>(this),
                                   Qt::Dialog | Qt::FramelessWindowHint);
    add_contact_dialog_->setWindowModality(Qt::WindowModal);
    add_user_search_->clear();
    add_users_->set_users({});
    add_users_view_->hide();
    add_users_status_->setText(connection_available_ ? QStringLiteral("输入用户名后按 Enter 或点击搜索")
                                                     : QStringLiteral("当前离线，连接恢复后可搜索用户"));
    add_contact_dialog_->adjustSize();
    add_contact_dialog_->open();
    add_user_search_->setFocus();
}

void chat_widget::filter_contacts(QString const& query)
{
    friends_list_->set_query(query);
    if (contacts_->rowCount() > 0)
    {
        contacts_status_->setText(friends_list_->rowCount() == 0 ? QStringLiteral("未找到好友") : QString{});
    }
    // With an empty model, the request result owns loading, error or empty feedback.
    contacts_status_->setVisible(!contacts_status_->text().isEmpty());
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
        add_users_status_->setText(QStringLiteral("输入用户名后按 Enter 查找"));
        return;
    }

    add_users_status_->setText(QStringLiteral("正在搜索…"));
    add_users_->set_users({});
    add_users_view_->hide();
    emit add_contact_search_requested(query, ++add_search_request_);
}

void chat_widget::select_contact(QModelIndex const& index)
{
    if (index.data(friend_list_model::header_role).toBool())
    {
        friends_list_->toggle(index);
        return;
    }
    if (!contacts_dialog_->isVisible() || !connection_available_)
    {
        return;
    }
    auto const* item = contacts_->user_at(friends_list_->source_index(index));
    if (!item || !is_contact(item->id))
    {
        return;
    }
    auto const user = item->id;
    auto const username = item->username;
    contacts_dialog_->accept();
    open_chat(user, username);
}

void chat_widget::refresh_theme()
{
    main_menu_button_->setIcon(svg_icon(u"menu", themed("text_secondary"), QSize(22, 22)));
    contacts_add_button_->setIcon(svg_icon(u"user-plus", themed("text_secondary"), QSize(20, 20)));
    chats_action_->setIcon(svg_icon(u"chat", themed("text"), QSize(20, 20)));
    contacts_action_->setIcon(svg_icon(u"contacts", themed("text"), QSize(20, 20)));
    contacts_requests_icon_->setPixmap(svg_icon(u"user-plus", themed("text_secondary"), QSize(22, 22)).pixmap(22, 22));
    contacts_requests_arrow_->setPixmap(svg_icon(u"chevron-right", themed("text_secondary"), QSize(16, 16)).pixmap(16, 16));
    cancel_reply_button_->setIcon(svg_icon(QStringLiteral("close"), themed("text"), QSize(20, 20)));
    send_button_->setIcon(composer_send_icon());
    history_down_->setIcon(svg_icon(u"chevron-down", themed("text"), QSize(20, 20)));
    attachment_button_->setIcon(svg_icon(u"attach", themed("text"), QSize(22, 22)));
    screenshot_button_->setIcon(svg_icon(u"screenshot", themed("text"), QSize(22, 22)));
    emoji_button_->setIcon(svg_icon(u"emoji", themed("text"), QSize(22, 22)));
    history_button_->setIcon(svg_icon(u"history", themed("text"), QSize(22, 22)));
    for (auto* action : findChildren<QAction*>())
    {
        if (action->objectName() == QStringLiteral("addFriendAction"))
        {
            action->setIcon(svg_icon(u"user-plus", themed("text"), QSize(20, 20)));
        }
        else if (action->objectName() == QStringLiteral("showSettingsAction"))
        {
            action->setIcon(svg_icon(u"settings", themed("text"), QSize(20, 20)));
        }
        else if (action->objectName() == QStringLiteral("createGroupAction") || action->objectName() == QStringLiteral("joinGroupAction"))
        {
            action->setIcon(svg_icon(u"groups", themed("text"), QSize(20, 20)));
        }
    }
    self_profile_action_->setIcon(avatar_icon(self_username_, 24, avatars_.image(self_user_)));
    if (active_conversation_ > 0)
    {
        update_chat_header(active_username_);
    }
    update_request_actions();
    for (auto* view : findChildren<QAbstractItemView*>())
    {
        view->viewport()->update();
    }
}

void chat_widget::update_request_actions()
{
    add_users_view_->viewport()->update();
    auto enabled = [this](QListWidget* list)
    {
        auto const* item = list->currentItem();
        return connection_available_ && item && !pending_friend_actions_.contains(item->data(Qt::UserRole).toLongLong());
    };
    incoming_accept_button_->setEnabled(enabled(incoming_friends_));
    incoming_reject_button_->setEnabled(enabled(incoming_friends_));
    outgoing_cancel_button_->setEnabled(enabled(outgoing_friends_));
}

// action: the current relationship it advances, or -1 to reject an incoming request.
void chat_widget::request_friend_action(qint64 user, QString const& username, int action)
{
    if (!connection_available_ || user <= 0 || pending_friend_actions_.contains(user))
    {
        return;
    }
    auto const state = friend_state(user);
    if (action >= 0 && action != static_cast<int>(state))
    {
        return;
    }
    if (action < 0 && state != chat::friendship_state::incoming_pending)
    {
        return;
    }
    pending_friend_actions_.insert(user, {action, username});
    if (action < 0)
    {
        emit friend_request_respond_requested(user, false);
    }
    else if (state == chat::friendship_state::incoming_pending)
    {
        emit friend_request_respond_requested(user, true);
    }
    else if (state == chat::friendship_state::outgoing_pending)
    {
        emit friend_request_cancel_requested(user);
    }
    else
    {
        emit contact_add_requested(user);
    }
    update_request_actions();
    emit friendship_updated();
}

void chat_widget::set_folders(conversation_folder_list folders) { conversations_->set_folders(std::move(folders)); }
void chat_widget::toggle_folder(qint64 folder)
{
    auto collapsed = conversations_->collapsed();
    if (!collapsed.remove(folder))
    {
        collapsed.insert(folder);
    }
    QStringList ids;
    for (auto id : collapsed)
    {
        ids.append(QString::number(id));
    }
    conversations_->set_collapsed(std::move(collapsed));
    QSettings().setValue(folder_settings_key_, ids);
    filter_conversations();
    conversations_view_->viewport()->update();
}
void chat_widget::edit_folder_name(qint64 folder, QString current)
{
    bool ok = false;
    auto name = prompt_text(this, QStringLiteral("会话分组"), QStringLiteral("分组名称（最多 32 个字符）"), current, ok).trimmed();
    if (!ok || name.isEmpty())
    {
        return;
    }
    if (folder)
    {
        emit rename_conversation_folder(folder, name);
    }
    else
    {
        emit create_conversation_folder(name);
    }
}
void chat_widget::folder_menu(QPoint point, QModelIndex const& index)
{
    auto id = index.data(conversation_model::folder_role).toLongLong();
    auto name = index.data().toString();
    QMenu menu(this);
    resume_reading_after_menu(menu);
    menu.addAction(QStringLiteral("全部标为已读"), this, [this, id] { emit mark_read_scope(0, id); });
    menu.addSeparator();
    add_list_actions(menu);
    if (id)
    {
        menu.addAction(QStringLiteral("重命名"), this, [this, id, name] { edit_folder_name(id, name); });
        menu.addAction(QStringLiteral("上移"), this, [this, id] { emit reorder_conversation_folder(id, true); })
            ->setEnabled(conversations_->folders().front().first != id);
        menu.addAction(QStringLiteral("下移"), this, [this, id] { emit reorder_conversation_folder(id, false); })
            ->setEnabled(conversations_->folders().back().first != id);
        menu.addSeparator();
        menu.addAction(QStringLiteral("删除分组"), this, [this, id] { emit delete_conversation_folder(id); });
    }
    menu.exec(conversations_view_->viewport()->mapToGlobal(point));
}

void chat_widget::filter_conversations()
{
    auto const query = conversation_search_->text().trimmed();
    int shown = 0;
    for (int row = 0; row < conversations_->rowCount(); ++row)
    {
        auto const index = conversations_->index(row, 0);
        if (index.data(conversation_model::folder_header_role).toBool())
        {
            conversations_view_->setRowHidden(row, !query.isEmpty());
            continue;
        }
        auto const matched = (query.isEmpty() && (conversations_->folders().isEmpty() ||
                                                  !conversations_->collapsed().contains(index.data(conversation_model::folder_role).toLongLong()))) ||
                             (!query.isEmpty() && (index.data(Qt::DisplayRole).toString().contains(query, Qt::CaseInsensitive) ||
                                                   index.data(conversation_model::username_role).toString().contains(query, Qt::CaseInsensitive) ||
                                                   index.data(conversation_model::last_text_role).toString().contains(query, Qt::CaseInsensitive)));
        auto const visible = matched && !conversations_->conversation_at(index)->hidden;
        conversations_view_->setRowHidden(row, !visible);
        shown += visible;
    }
    if (conversations_->rowCount() > 0)
    {
        set_list_status(!query.isEmpty() && shown == 0 ? QStringLiteral("没有匹配的聊天") : QString{});
    }
}

void chat_widget::update_main_menu()
{
    auto now = QDateTime::currentMSecsSinceEpoch();
    qint64 nearest = 0;
    quint64 total = 0;
    qint64 requests = incoming_requests_.size();
    for (int row = 0; row < conversations_->rowCount(); ++row)
    {
        auto const index = conversations_->index(row, 0);
        auto const* item = conversations_->conversation_at(index);
        if (!item)
        {
            continue;
        }
        requests += item->join_requests;
        if (item->hidden)
        {
            continue;
        }
        if (item->notifications == chat::notification_level::muted && item->mute_until > now && (nearest == 0 || item->mute_until < nearest))
        {
            nearest = item->mute_until;
        }
        if (!index.data(conversation_model::muted_role).toBool())
        {
            total += index.data(conversation_model::unread_role).toULongLong();
        }
    }
    if (nearest)
    {
        mute_expiry_timer_->start(static_cast<int>(std::min<qint64>(nearest - now, std::numeric_limits<int>::max())));
    }
    else
    {
        mute_expiry_timer_->stop();
    }
    chats_action_->setText(total > 0 ? QStringLiteral("聊天 · %1 条未读").arg(total) : QStringLiteral("聊天"));
    contacts_action_->setText(requests > 0 ? QStringLiteral("好友 · %1 个待处理申请").arg(requests) : QStringLiteral("好友"));
    chats_action_->setChecked(true);
    auto const count = requests;
    main_menu_badge_->setText(count > 99 ? QStringLiteral("99+") : QString::number(count));
    main_menu_badge_->setVisible(count > 0);
    auto const width = std::max(16, main_menu_badge_->sizeHint().width());
    main_menu_badge_->setGeometry(main_menu_button_->width() - width, 0, width, 16);
    auto const description = QStringLiteral("主菜单 · %1 条未读消息 · %2 个待处理申请").arg(total).arg(requests);
    main_menu_button_->setToolTip(description);
    main_menu_button_->setAccessibleDescription(description);
}

void chat_widget::show_emoji_picker()
{
    if (!can_send())
    {
        return;
    }
    static QStringList const emoji{
        QStringLiteral("😀"), QStringLiteral("😄"), QStringLiteral("😁"), QStringLiteral("😆"), QStringLiteral("😅"), QStringLiteral("😂"),
        QStringLiteral("🤣"), QStringLiteral("😊"), QStringLiteral("🙂"), QStringLiteral("😉"), QStringLiteral("😍"), QStringLiteral("🥰"),
        QStringLiteral("😘"), QStringLiteral("😋"), QStringLiteral("😜"), QStringLiteral("🤔"), QStringLiteral("🤗"), QStringLiteral("🤭"),
        QStringLiteral("😐"), QStringLiteral("🙄"), QStringLiteral("😏"), QStringLiteral("😴"), QStringLiteral("😮"), QStringLiteral("😳"),
        QStringLiteral("🥺"), QStringLiteral("😢"), QStringLiteral("😭"), QStringLiteral("😤"), QStringLiteral("😡"), QStringLiteral("😱"),
        QStringLiteral("😎"), QStringLiteral("🤓"), QStringLiteral("👍"), QStringLiteral("👎"), QStringLiteral("👌"), QStringLiteral("✌️"),
        QStringLiteral("👏"), QStringLiteral("🙏"), QStringLiteral("💪"), QStringLiteral("🤝"), QStringLiteral("❤️"),  QStringLiteral("💔"),
        QStringLiteral("🔥"), QStringLiteral("🎉"), QStringLiteral("✨"), QStringLiteral("🌹"), QStringLiteral("☕"), QStringLiteral("🍻")};
    auto* picker = new QFrame(this, Qt::Popup);
    picker->setObjectName(QStringLiteral("emojiPicker"));
    picker->setAttribute(Qt::WA_DeleteOnClose);
    auto* grid = new QGridLayout(picker);
    grid->setContentsMargins(8, 8, 8, 8);
    grid->setSpacing(2);
    constexpr int columns = 8;
    auto font = emoji_input_font(picker->font());
    font.setPixelSize(22);
    for (int i = 0; i < emoji.size(); ++i)
    {
        auto* button = new QToolButton(picker);
        button->setObjectName(QStringLiteral("emojiChoice"));
        button->setText(emoji[i]);
        button->setAccessibleName(emoji[i]);
        button->setFont(font);
        button->setFixedSize(38, 38);
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        connect(button,
                &QToolButton::clicked,
                picker,
                [this, picker, text = emoji[i]]
                {
                    if (can_send())
                    {
                        message_edit_->insertPlainText(text);
                        message_edit_->setFocus();
                    }
                    picker->close();
                });
        grid->addWidget(button, i / columns, i % columns);
    }
    picker->adjustSize();
    // Opens above the button, right-aligned with it, like the emoji panels of Telegram and WeChat.
    auto const anchor = emoji_button_->mapToGlobal(QPoint(emoji_button_->width(), 0));
    picker->move(anchor.x() - picker->width(), anchor.y() - picker->height() - 6);
    picker->show();
}

void chat_widget::show_loading_status()
{
    set_message_status({});
    loading_status_timer_->start();
}

void chat_widget::select_add_user(QModelIndex const& index)
{
    if (!add_contact_dialog_->isVisible() || !connection_available_)
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
        contacts_dialog_->reject();
        add_contact_dialog_->reject();
        emit direct_conversation_requested(user, std::move(username));
    }
}

QString chat_widget::friend_remark(qint64 user) const
{
    for (int i = 0; i < contacts_->rowCount(); ++i)
    {
        auto const* person = contacts_->user_at(contacts_->index(i, 0));
        if (person->id == user)
        {
            return person->remark;
        }
    }
    return {};
}

bool chat_widget::is_contact(qint64 user) const
{
    for (int row = 0; row < contacts_->rowCount(); ++row)
    {
        if (contacts_->user_at(contacts_->index(row, 0))->id == user)
        {
            return true;
        }
    }
    return false;
}

void chat_widget::open_conversation(conversation_data conversation)
{
    auto const user = conversation.id;
    if (user <= 0)
    {
        return;
    }
    if (user != active_conversation_)
    {
        close_poll_results();
    }
    auto const existing = conversations_->index_for_conversation(user);
    auto const* listed = conversations_->conversation_at(existing);
    if (connection_available_ && (conversation.marked_unread || (listed && listed->marked_unread)))
    {
        emit unread_mark_requested(user, false);
    }
    if (!existing.isValid())
    {
        auto items = conversations_->items();
        items.push_back(conversation);
        conversations_->set_conversations(std::move(items));
    }
    auto username = conversation.username;
    active_peer_ = conversation.user;
    active_group_ = conversation.group;

    active_username_ = std::move(username);

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
        update_chat_header(active_username_);
        update_compose_state();
        if (!messages_loaded_ && !messages_loading_)
        {
            set_messages_loading(true);
            show_loading_status();
            if (connection_available_)
            {
                request_entry_history();
                emit conversation_selected(active_conversation_, active_group_);
            }
        }
        return;
    }

    close_poll_results();
    flush_draft();
    stop_typing();
    typing_users_.clear();
    update_chat_status();
    images_.discard_queued();
    if (active_conversation_ && messages_loaded_)
    {
        auto position = capture_reading_position();
        if (position.message)
        {
            reading_positions_.insert(active_conversation_, position);
        }
        else
        {
            reading_positions_.remove(active_conversation_);
        }
    }
    history_target_.reset();
    reply_returns_.clear();
    active_conversation_ = user;
    ++conversation_visit_;
    observed_tail_ = requested_read_ = 0;
    messages_loaded_ = false;
    tail_loaded_ = false;
    update_reading_navigation();
    update_chat_header(active_username_);
    apply_draft();
    message_search_action_->setEnabled(connection_available_);
    history_button_->setEnabled(connection_available_);
    set_message_selection(false);
    messages_->reset(active_conversation_, active_group_);
    load_draft_reply();
    show_outgoing();
    update_pinned_message();
    set_messages_loading(true);
    history_exhausted_ = false;
    show_loading_status();
    update_compose_state();
    if (connection_available_)
    {
        request_entry_history();
        emit conversation_selected(active_conversation_, active_group_);
    }
}

void chat_widget::hide_conversation(qint64 conversation) { leave_conversation(conversation); }
bool chat_widget::visible_message(message_data& message) const
{
    auto floor = history_floor(message.conversation);
    if (message.reply.id <= floor)
    {
        message.reply = {};
    }
    return !message.acknowledgement_only && message.id > floor;
}
void chat_widget::discard_history(qint64 conversation, qint64 floor)
{
    if (floor <= history_floor(conversation))
    {
        return;
    }
    history_floors_.insert(conversation, floor);
    if (reading_positions_.value(conversation).message <= floor)
    {
        reading_positions_.remove(conversation);
    }
    emit history_discarded(conversation, floor);
    conversations_->discard_through(conversation, floor);
    images_.remove(conversation, 1, floor);
    for (auto& item : outgoing_)
    {
        if (item.conversation == conversation && item.reply.id > 0 && item.reply.id <= floor)
        {
            item.reply.username.clear();
            item.reply.text.clear();
        }
    }
    discard_draft_reply(conversation, floor);
    if (conversation != active_conversation_)
    {
        return;
    }
    reply_returns_.removeIf([floor](const reading_position& p) { return p.message && p.message <= floor; });
    preserve_reading(
        [&]
        {
            if (messages_->first_message_id() > 0 && messages_->first_message_id() <= floor)
            {
                history_exhausted_ = true;
            }
            messages_->discard_through(floor);
            if (messages_->unread_boundary() <= floor)
            {
                messages_->set_unread_boundary(0);
            }
        });
    if (observed_tail_ <= floor)
    {
        observed_tail_ = 0;
    }
    if (!messages_->last_message_id())
    {
        request_newer_messages();
    }
    update_reading_navigation();
    update_pinned_message();
    show_outgoing();
}
void chat_widget::invalidate_draft_replies(QList<qint64> const& conversations)
{
    for (auto const id : conversations)
    {
        discard_draft_reply(id, std::numeric_limits<qint64>::max());
    }
}

void chat_widget::close_conversation(qint64 conversation)
{
    discard_draft_reply(conversation, std::numeric_limits<qint64>::max());
    drop_outgoing(conversation);
    leave_conversation(conversation);
}

void chat_widget::leave_conversation(qint64 conversation)
{
    reading_positions_.remove(conversation);
    conversations_->remove_conversation(conversation);
    if (conversations_->rowCount() == 0)
    {
        set_list_status(QStringLiteral("暂无会话"));
    }
    filter_conversations();
    update_main_menu();
    if (conversation != active_conversation_)
    {
        images_.remove(conversation);
        return;
    }
    close_poll_results();
    flush_draft();
    stop_typing();
    typing_users_.clear();
    update_chat_status();
    active_conversation_ = 0;
    history_target_.reset();
    reply_returns_.clear();
    ++conversation_visit_;
    observed_tail_ = requested_read_ = 0;
    tail_loaded_ = false;
    update_reading_navigation();
    update_pinned_message();
    active_peer_ = 0;
    active_group_ = false;
    active_username_.clear();
    messages_loaded_ = false;
    set_messages_loading(false);
    history_exhausted_ = false;
    reset_reply();
    set_message_selection(false);
    messages_->reset(0);
    images_.remove(conversation);
    conversations_view_->setCurrentIndex({});
    conversations_view_->clearSelection();
    update_chat_header({});
    message_edit_->clear();
    message_edit_->setEnabled(false);
    send_button_->setEnabled(false);
    message_search_action_->setEnabled(false);
    history_button_->setEnabled(false);
    attachment_button_->setEnabled(false);
    set_message_status({});
    update_compose_state();
}

void chat_widget::update_pinned_message()
{
    auto const item = conversation(active_conversation_);
    auto const visible = active_group_ && item && item->pinned_message.id > 0;
    pinned_message_button_->parentWidget()->setVisible(visible);
    pinned_message_button_->setVisible(visible);
    unpin_message_button_->setVisible(visible && messages_->can_manage_group());
    pinned_message_button_->setEnabled(connection_available_);
    unpin_message_button_->setEnabled(connection_available_);
    if (visible)
    {
        auto const summary =
            QStringLiteral("置顶消息 · %1：%2").arg(item->pinned_message.username, grapheme_prefix(item->pinned_message.text.simplified(), 80));
        pinned_message_button_->setText(QString(summary).replace(QLatin1Char('&'), QStringLiteral("&&")));
        pinned_message_button_->setToolTip(summary + QStringLiteral("\n点击定位到这条消息。"));
    }
}

void chat_widget::set_members(qint64 conversation, QList<member_data> members, QString const& error)
{
    if (conversation != active_conversation_ || !error.isEmpty())
    {
        return;
    }
    for (auto const& member : members)
    {
        avatars_.observe(member.id, member.avatar);
    }
    messages_->set_members(members);
    update_pinned_message();
    for (auto it = typing_users_.begin(); it != typing_users_.end();)
    {
        auto const present = std::any_of(members.begin(), members.end(), [id = it.key()](auto const& member) { return member.id == id; });
        it = present ? std::next(it) : typing_users_.erase(it);
    }
    update_chat_status();
}

void chat_widget::show_read_details(qint64 conversation, qint64 message)
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("readDetailsDialog"));
    dialog.setWindowTitle(QStringLiteral("已读成员"));
    dialog.resize(chat_theme::dialog_small_width, 360);
    auto* layout = new QVBoxLayout(&dialog);
    frame_dialog(dialog);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("readMembersList"));
    list->setAccessibleName(QStringLiteral("已读成员"));
    list->setItemDelegate(new user_delegate(list));
    list->setUniformItemSizes(true);
    list->setFrameShape(QFrame::NoFrame);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setMouseTracking(true);
    layout->addWidget(list);
    auto refresh = [this, &dialog, conversation, message, list]
    {
        const auto row = messages_->message_index(message);
        if (active_conversation_ != conversation || !row.isValid() || row.data(message_model::deleted_role).toBool())
        {
            dialog.reject();
            return false;
        }
        auto const members = messages_->read_members(message);
        auto const selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : 0;
        auto const scroll = list->verticalScrollBar()->value();
        list->clear();
        for (auto const& member : members)
        {
            auto* item = new QListWidgetItem(member.display_name(), list);
            item->setData(Qt::UserRole, member.id);
            item->setData(Qt::StatusTipRole, QStringLiteral("已读"));
            item->setData(Qt::DecorationRole, avatars_.image(member.id));
            item->setToolTip(member.username);
            if (member.id == selected)
            {
                list->setCurrentItem(item);
            }
        }
        list->doItemsLayout();
        list->verticalScrollBar()->setValue(scroll);
        return true;
    };
    connect(messages_, &message_model::readers_changed, &dialog, refresh);
    connect(messages_,
            &QAbstractItemModel::dataChanged,
            &dialog,
            [this, message, refresh](QModelIndex const& first, QModelIndex const& last, QList<int> const& roles)
            {
                const auto target = messages_->message_index(message);
                if (roles.isEmpty() && target.row() >= first.row() && target.row() <= last.row())
                {
                    refresh();
                }
            });
    connect(messages_, &QAbstractItemModel::modelReset, &dialog, refresh);
    connect(messages_, &QAbstractItemModel::rowsRemoved, &dialog, refresh);
    connect(&avatars_, &avatar_cache::changed, &dialog, refresh);
    if (!refresh())
    {
        return;
    }
    dialog.exec();
}

std::optional<conversation_data> chat_widget::conversation(qint64 id) const
{
    if (auto const* item = conversations_->conversation_at(conversations_->index_for_conversation(id)))
    {
        return *item;
    }
    return std::nullopt;
}

void chat_widget::set_notifications(qint64 conversation, chat::notification_level level, qint64 until)
{
    conversations_->set_notifications(conversation, level, until);
}
void chat_widget::set_conversation_pinned(qint64 conversation, bool pinned) { conversations_->set_pinned(conversation, pinned); }

void chat_widget::create_group()
{
    if (!connection_available_)
    {
        return;
    }
    create_group_dialog dialog(this);
    auto refresh = [this, &dialog] { dialog.set_contacts(*contacts_, self_user_, avatars_); };
    connect(contacts_, &QAbstractItemModel::modelReset, &dialog, refresh);
    refresh();
    if (dialog.exec() != QDialog::Accepted || !connection_available_)
    {
        return;
    }
    auto members = dialog.selected_members();
    members.removeIf([this](qint64 user) { return !is_contact(user); });
    if (!members.empty())
    {
        emit group_create_requested(dialog.group_title(), std::move(members));
    }
}

void chat_widget::request_older_messages()
{
    if (!connection_available_ || active_conversation_ <= 0 || !messages_loaded_ || messages_loading_ || history_exhausted_)
    {
        return;
    }

    auto const before = messages_->first_message_id();
    if (before <= 0)
    {
        history_exhausted_ = true;
        return;
    }

    set_messages_loading(true);
    emit history_requested(active_conversation_, history_page::older, before);
}

void chat_widget::update_pending_text(qint64 conversation,
                                      qint64 client_id,
                                      QString text,
                                      qint64 reply,
                                      qint64 created,
                                      bool sending,
                                      QString rejection,
                                      bool unavailable,
                                      chat::text_options options)
{
    auto item = std::ranges::find(outgoing_, client_id, &outgoing::client_id);
    bool const added = item == outgoing_.end();
    if (added)
    {
        outgoing entry;
        entry.client_id = client_id;
        entry.conversation = conversation;
        entry.text = std::move(text);
        entry.options = options;
        entry.created = created;
        entry.reply = conversation == active_conversation_ ? quote(reply) : quoted_message_data{};
        entry.reply.id = reply;
        outgoing_.push_back(std::move(entry));
        item = std::prev(outgoing_.end());
    }
    if (!reply)
    {
        item->reply = {};
    }
    item->failed = !rejection.isEmpty();
    item->unavailable = unavailable;
    item->waiting = !sending && !item->failed;
    item->error = std::move(rejection);
    if (conversation == active_conversation_)
    {
        show_outgoing();
    }
}

void chat_widget::discard_pending_text(qint64 client_id)
{
    if (outgoing_.removeIf([client_id](outgoing const& item) { return item.path.isEmpty() && item.client_id == client_id; }) > 0)
    {
        show_outgoing();
    }
}

void chat_widget::reconcile_pending_texts(QList<qint64> client_ids)
{
    QSet<qint64> const restored(client_ids.begin(), client_ids.end());
    if (outgoing_.removeIf([&restored](outgoing const& item) { return item.path.isEmpty() && !restored.contains(item.client_id); }) > 0)
    {
        show_outgoing();
    }
}

void chat_widget::choose_attachment(qint64 conversation, bool group_only, QWidget* parent)
{
    auto paths = QFileDialog::getOpenFileNames(parent, group_only ? QStringLiteral("只上传到群文件") : QStringLiteral("发送文件或图片"));
    if (!paths.isEmpty())
    {
        send_files(conversation, paths, 0, group_only);
    }
}
QList<chat_widget::upload_row> chat_widget::group_uploads(qint64 room) const
{
    QList<upload_row> result;
    for (auto const& item : outgoing_)
    {
        if (item.conversation == room && item.group_only)
        {
            result.push_back({item.client_id, item.filename, item.size, item.error});
        }
    }
    return result;
}

void chat_widget::add_upload_actions(QMenu& menu, qint64 id)
{
    auto item = std::ranges::find(outgoing_, id, &outgoing::client_id);
    if (item == outgoing_.end() || item->path.isEmpty())
    {
        return;
    }
    auto room = conversation(item->conversation);
    auto paused = item->failed;
    auto* toggle = menu.addAction(paused ? QStringLiteral("继续上传") : QStringLiteral("暂停上传"));
    toggle->setEnabled(connection_available_ && (!paused || (!item->unavailable && room && !room->speech_blocked())));
    connect(toggle,
            &QAction::triggered,
            this,
            [this, id, paused]
            {
                auto item = std::ranges::find(outgoing_, id, &outgoing::client_id);
                if (!connection_available_ || item == outgoing_.end() || item->failed != paused)
                {
                    return;
                }
                if (!paused)
                {
                    emit attachment_interrupt_requested(id, false, item->path);
                    return;
                }
                auto room = conversation(item->conversation);
                if (!room || room->speech_blocked() || item->unavailable)
                {
                    return;
                }
                item->failed = false;
                item->error = QStringLiteral("正在校验文件…");
                show_outgoing();
                submit(*item);
            });
    auto* cancel = menu.addAction(QStringLiteral("取消上传"));
    cancel->setEnabled(connection_available_);
    connect(cancel,
            &QAction::triggered,
            this,
            [this, id]
            {
                auto item = std::ranges::find(outgoing_, id, &outgoing::client_id);
                if (connection_available_ && item != outgoing_.end())
                {
                    emit attachment_interrupt_requested(id, true, item->path);
                }
            });
}

void chat_widget::finish_group_file(qint64 id, QString error, bool unavailable)
{
    if (error.isEmpty())
    {
        attachment_cancelled(id);
        return;
    }
    auto item = std::ranges::find(outgoing_, id, &outgoing::client_id);
    if (item != outgoing_.end())
    {
        item->failed = true;
        if (!item->unavailable)
        {
            item->unavailable = unavailable;
            item->error = error;
        }
        emit uploads_changed();
    }
}
void chat_widget::send_files(qint64 target, QStringList const& paths, qint64 clipboard_id, bool group_only)
{
    auto const room = conversation(target);
    if (!connection_available_ || !room || room->speech_blocked())
    {
        set_message_status(room && room->speech_blocked() ? QStringLiteral("群聊禁言，暂时无法发布文件")
                                                          : QStringLiteral("当前会话暂时无法发送文件"));
        return;
    }
    bool const use_reply = !group_only && target == active_conversation_;
    auto const reply = use_reply ? quote(reply_to_) : quoted_message_data{};
    QStringList errors;
    for (auto const& path : paths)
    {
        QFileInfo const file(path);
        QString error;
        if (!file.isFile() || !file.isReadable())
        {
            error = QStringLiteral("文件不存在、不是普通文件或无法读取");
        }
        else if (file.size() > chat::max_attachment_size)
        {
            error = QStringLiteral("文件不能超过 %1 GiB").arg(chat::max_attachment_size / (1024LL * 1024 * 1024));
        }
        if (!error.isEmpty())
        {
            errors.push_back(QStringLiteral("%1：%2").arg(file.fileName(), error));
            if (clipboard_id != 0)
            {
                remove_clipboard_image(path, clipboard_id);
            }
            continue;
        }
        outgoing item;
        item.client_id = clipboard_id != 0 ? clipboard_id : chat::client::new_client_id();
        item.conversation = target;
        item.filename = file.fileName();
        item.path = file.absoluteFilePath();
        item.size = file.size();
        item.error = QStringLiteral("正在校验文件…");
        item.group_only = group_only;
        item.reply = reply;
        queue_outgoing(std::move(item));
    }
    if (use_reply && errors.size() != paths.size() && reply_to_ != 0)
    {
        reset_reply();
        draft_edited();
    }
    if (!errors.isEmpty())
    {
        set_message_status(errors.join(u'\n'));
    }
}

void chat_widget::queue_outgoing(outgoing item)
{
    item.created = QDateTime::currentMSecsSinceEpoch();
    outgoing_.push_back(std::move(item));
    if (outgoing_.back().group_only || outgoing_.back().conversation != active_conversation_)
    {
        emit uploads_changed();
        submit(outgoing_.back());
        return;
    }
    stop_typing();
    return_to_latest();
    show_outgoing();
    update_compose_state();
    submit(outgoing_.back());
}

void chat_widget::submit(outgoing const& item)
{
    emit attachment_send_requested(item.conversation, item.path, item.reply.id, item.client_id, item.group_only);
}

void chat_widget::show_outgoing()
{
    emit uploads_changed();
    if (!tail_loaded_)
    {
        messages_->set_pending({});
        return;
    }
    QList<message_model::pending_message> pending;
    for (auto const& item : outgoing_)
    {
        if (item.group_only || item.conversation != active_conversation_)
        {
            continue;
        }
        message_data message;
        message.conversation = item.conversation;
        message.from = self_user_;
        message.username = self_username_;
        message.timestamp = item.created;
        message.text = item.text;
        message.markdown = item.options.markdown;
        message.mention_all = item.options.mention_all;
        message.reply = item.path.isEmpty() && item.reply.id > 0 ? quote(item.reply.id) : item.reply;
        message.client_id = item.client_id;
        if (!item.filename.isEmpty())
        {
            message.attachment = attachment_data{item.filename, QStringLiteral("application/octet-stream"), item.size, {}};
        }
        pending.push_back({
            .message = std::move(message),
            .failed = item.failed,
            .error = item.error,
            .waiting = item.waiting,
        });
    }
    preserve_reading([&] { messages_->set_pending(std::move(pending)); });
}

void chat_widget::settle(message_data const& message)
{
    if (message.from != self_user_ || message.client_id <= 0)
    {
        return;
    }
    if (outgoing_.removeIf([&](outgoing const& item) { return item.client_id == message.client_id; }) > 0 &&
        message.conversation == active_conversation_)
    {
        show_outgoing();
    }
}

void chat_widget::drop_outgoing(qint64 conversation)
{
    for (auto& item : outgoing_)
    {
        if (item.conversation == conversation && !item.path.isEmpty())
        {
            item.failed = true;
            item.unavailable = true;
            item.error = QStringLiteral("会话不可用，上传已暂停");
            if (connection_available_)
            {
                emit attachment_interrupt_requested(item.client_id, false, item.path);
            }
        }
    }
}

quoted_message_data chat_widget::quote(qint64 message) const
{
    if (message <= 0)
    {
        return {};
    }
    if (auto index = messages_->message_index(message); index.isValid())
    {
        auto const revision = index.data(message_model::edited_at_role).toLongLong();
        auto const deleted = index.data(message_model::deleted_role).toBool();
        if (!deleted && reply_snapshot_.id == message && reply_snapshot_.edited_at >= revision)
        {
            return reply_snapshot_;
        }
        return {message,
                index.data(message_model::sender_name_role).toString(),
                grapheme_prefix(index.data(message_model::attachment_name_role).toString().isEmpty()
                                    ? index.data(message_model::text_role).toString()
                                    : index.data(message_model::attachment_name_role).toString(),
                                160),
                revision,
                deleted,
                index.data(message_model::from_role).toLongLong()};
    }
    return reply_snapshot_.id == message ? reply_snapshot_ : quoted_message_data{message, {}, {}, 0, false};
}

void chat_widget::show_outgoing_menu(QModelIndex const& index, QPoint position)
{
    auto const client_id = index.data(message_model::client_id_role).toLongLong();
    auto const failed = index.data(message_model::send_state_role).toInt() == message_model::failed;
    auto const found = std::ranges::find(outgoing_, client_id, &outgoing::client_id);
    if (found == outgoing_.end())
    {
        return;
    }
    auto const visit = conversation_visit_;
    auto const has_reply = found->reply.id > 0;
    QMenu menu(this);
    resume_reading_after_menu(menu);
    auto* copy = menu.addAction(QStringLiteral("复制"));
    // Sending again needs the connection; discarding is local and works offline too.
    bool const attachment = !found->path.isEmpty();
    if (attachment)
    {
        add_upload_actions(menu, client_id);
    }
    auto* retry = !attachment && failed && can_send() ? menu.addAction(QStringLiteral("重新发送")) : nullptr;
    auto const* retry_plain = attachment && failed && can_send() && has_reply ? menu.addAction(QStringLiteral("不带回复重新发送")) : nullptr;
    auto* discard = !attachment && (failed || found->waiting) ? menu.addAction(QStringLiteral("删除待发消息")) : nullptr;
    auto* selected = menu.exec(messages_view_->viewport()->mapToGlobal(position));
    auto item = std::ranges::find(outgoing_, client_id, &outgoing::client_id);
    if (!selected || item == outgoing_.end() || visit != conversation_visit_)
    {
        return;
    }
    if (selected == copy)
    {
        QGuiApplication::clipboard()->setText(item->filename.isEmpty() ? item->text : item->filename);
        return;
    }
    if (!attachment)
    {
        if (selected == discard && (item->failed || item->waiting))
        {
            emit pending_text_action_requested(client_id, true);
        }
        else if (selected == retry && item->failed && can_send())
        {
            emit pending_text_action_requested(client_id, false);
        }
        return;
    }
    if (!item->failed)
    {
        return;
    }
    if ((selected != retry && selected != retry_plain) || !can_send() || item->conversation != active_conversation_)
    {
        return;
    }
    // The same client_id: whatever reached the server before is not stored twice.
    item->failed = false;
    item->error.clear();
    if (selected == retry_plain)
    {
        item->reply = {};
    }
    show_outgoing();
    submit(*item);
}

void chat_widget::show_user_details(qint64 user, QString const& username, QWidget* profile_parent)
{
    if (user <= 0 || username.isEmpty())
    {
        return;
    }

    QDialog dialog(profile_parent                     ? profile_parent
                   : add_contact_dialog_->isVisible() ? add_contact_dialog_
                   : contacts_dialog_->isVisible()    ? contacts_dialog_
                                                      : static_cast<QWidget*>(this));
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
    close_button->setIcon(svg_icon(QStringLiteral("close"), themed("text"), QSize(22, 22)));
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
    connect(&avatars_,
            &avatar_cache::changed,
            &dialog,
            [this, avatar, user, username](qint64 changed)
            {
                if (changed == user)
                {
                    avatar->setPixmap(avatar_icon(username, 104, avatars_.image(user)).pixmap(104, 104));
                }
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
    auto make_action = [header](QString text, QStringView icon, QColor const& color)
    {
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
    auto* message_button = make_action(QStringLiteral("消息"), QStringLiteral("chat"), themed("on_accent"));
    message_button->setProperty("primary", true);
    auto* copy_username_button = make_action(QStringLiteral("复制用户名"), QStringLiteral("copy"), themed("accent"));
    connect(&theme_manager::instance(),
            &theme_manager::changed,
            &dialog,
            [this, close_button, message_button, copy_username_button, avatar, user, username]
            {
                close_button->setIcon(svg_icon(u"close", themed("text"), QSize(22, 22)));
                message_button->setIcon(svg_icon(u"chat", themed("on_accent"), QSize(20, 20)));
                copy_username_button->setIcon(svg_icon(u"copy", themed("accent"), QSize(20, 20)));
                avatar->setPixmap(avatar_icon(username, 104, avatars_.image(user)).pixmap(104, 104));
            });
    actions->addWidget(message_button);
    actions->addWidget(copy_username_button);
    actions->addStretch();
    header_layout->addLayout(actions);
    layout->addWidget(header);

    auto* section_separator = new QFrame(&dialog);
    section_separator->setObjectName(QStringLiteral("profileSectionSeparator"));
    section_separator->setFixedHeight(1);
    layout->addWidget(section_separator);

    auto* info = new QFrame(&dialog);
    info->setObjectName(QStringLiteral("profileInfoSection"));
    auto* info_layout = new QVBoxLayout(info);
    info_layout->setContentsMargins(24, 16, 24, 24);
    info_layout->setSpacing(12);

    auto* account = new QLabel(header);
    account->hide();
    account->setTextFormat(Qt::PlainText);
    account->setAlignment(Qt::AlignCenter);
    header_layout->insertWidget(header_layout->indexOf(name) + 1, account);
    auto* signature = new QLabel(info);
    signature->setObjectName(QStringLiteral("profileSignatureText"));
    signature->setTextFormat(Qt::PlainText);
    signature->setWordWrap(true);
    info_layout->addWidget(signature);
    auto* edit_profile = new QPushButton(QStringLiteral("编辑昵称与签名"), info);
    edit_profile->setObjectName(QStringLiteral("editUserProfile"));
    edit_profile->setVisible(user == self_user_);
    edit_profile->setEnabled(false);
    info_layout->addWidget(edit_profile);
    connect(this,
            &chat_widget::profile_received,
            &dialog,
            [this, user, account, name, signature, edit_profile](qint64 id, QString login, QString nick, QString text)
            {
                if (id != user)
                {
                    return;
                }
                account->setText(login);
                name->setText(user_display_name(login, nick, friend_remark(id)));
                account->setVisible(name->text() != login);
                signature->setText(text);
                edit_profile->setProperty("nickname", nick);
                edit_profile->setEnabled(connection_available_);
            });
    connect(contacts_,
            &QAbstractItemModel::modelReset,
            &dialog,
            [this, name, account, edit_profile, user]
            {
                if (!account->text().isEmpty())
                {
                    name->setText(user_display_name(account->text(), edit_profile->property("nickname").toString(), friend_remark(user)));
                    account->setVisible(name->text() != account->text());
                }
            });
    connect(copy_username_button, &QToolButton::clicked, &dialog, [account] { QGuiApplication::clipboard()->setText(account->text()); });
    connect(edit_profile,
            &QPushButton::clicked,
            &dialog,
            [this, &dialog, edit_profile, signature]
            {
                QDialog editor(&dialog);
                editor.setWindowTitle(QStringLiteral("编辑个人资料"));
                QVBoxLayout layout(&editor);
                frame_dialog(editor, false);
                QLineEdit nick(edit_profile->property("nickname").toString(), &editor);
                QLineEdit text(signature->text(), &editor);
                nick.setObjectName(QStringLiteral("profileNickname"));
                text.setObjectName(QStringLiteral("profileSignature"));
                layout.addWidget(new QLabel(QStringLiteral("昵称（最多 32 个字符）"), &editor));
                layout.addWidget(&nick);
                layout.addWidget(new QLabel(QStringLiteral("个性签名（最多 128 个字符）"), &editor));
                layout.addWidget(&text);
                QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &editor);
                layout.addWidget(&buttons);
                connect(&buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
                connect(&buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
                if (editor.exec() == QDialog::Accepted)
                {
                    emit profile_edit_requested(nick.text(), text.text());
                }
            });
    if (user != self_user_)
    {
        connect(this,
                &chat_widget::profile_access_changed,
                &dialog,
                [this, user, signature]
                {
                    signature->clear();
                    emit profile_requested(user);
                });
    }
    emit profile_requested(user);
    setup_profile_friendship(dialog, info_layout, relationship, message_button, user, username);
    if (user == self_user_)
    {
        setup_profile_avatar(dialog, info_layout, user);
        auto* bookmarks = new QPushButton(QStringLiteral("消息收藏"), info);
        bookmarks->setObjectName(QStringLiteral("profileBookmarks"));
        info_layout->addWidget(bookmarks);
        connect(bookmarks, &QPushButton::clicked, &dialog, [this, &dialog] { emit profile_bookmarks_requested(&dialog); });
        auto* devices = new QPushButton(QStringLiteral("账号安全与登录设备"), info);
        info_layout->addWidget(devices);
        connect(devices, &QPushButton::clicked, &dialog, [this, &dialog] { emit profile_devices_requested(&dialog); });
        auto* logout = new QPushButton(QStringLiteral("退出登录"), info);
        logout->setObjectName(QStringLiteral("profileLogoutButton"));
        info_layout->addWidget(logout);
        connect(logout, &QPushButton::clicked, &dialog, [this, &dialog] { emit logout_requested(&dialog); });
    }
    layout->addWidget(info);

    connect(close_button, &QToolButton::clicked, &dialog, &QDialog::reject);

    dialog.exec();
}

void chat_widget::setup_profile_friendship(
    QDialog& dialog, QVBoxLayout* info_layout, QLabel* relationship, QToolButton* message_button, qint64 user, QString const& username)
{
    auto* info = info_layout->parentWidget();
    QPushButton* remove_contact_button = nullptr;
    if (user != self_user_)
    {
        auto* remove_button = new QPushButton(QStringLiteral("删除好友"), info);
        remove_contact_button = remove_button;
        remove_button->setObjectName(QStringLiteral("removeContactButton"));
        remove_button->setEnabled(connection_available_);
        info_layout->addWidget(remove_button);
        connect(remove_button,
                &QPushButton::clicked,
                &dialog,
                [this, &dialog, user, username]
                {
                    if (!confirm_action(&dialog,
                                        QStringLiteral("删除好友"),
                                        QStringLiteral("删除与 %1 的好友关系？双方将无法继续发送新消息，聊天记录和群成员资格会保留。").arg(username),
                                        QStringLiteral("删除好友")))
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
    auto update_contact = [this, user, relationship, message_button, remove_contact_button, reject_request, cancel_request]
    {
        auto const state = friend_state(user);
        if (user == self_user_)
        {
            relationship->setText(QStringLiteral("我的账号"));
        }
        else if (state == chat::friendship_state::accepted)
        {
            auto const presence = presence_.value(user);
            auto const text = presence_text(presence.online, presence.last_seen);
            relationship->setText(text.isEmpty() ? QStringLiteral("已是好友") : text);
        }
        else
        {
            relationship->setText(state == chat::friendship_state::incoming_pending   ? QStringLiteral("对方向你发送了好友申请")
                                  : state == chat::friendship_state::outgoing_pending ? QStringLiteral("已发出申请，等待对方确认")
                                                                                      : QStringLiteral("添加好友后可发送消息"));
        }
        message_button->setVisible(user != self_user_);
        message_button->setText(state == chat::friendship_state::accepted           ? QStringLiteral("消息")
                                : state == chat::friendship_state::incoming_pending ? QStringLiteral("接受申请")
                                : state == chat::friendship_state::outgoing_pending ? QStringLiteral("等待验证")
                                                                                    : QStringLiteral("添加好友"));
        auto const pending = pending_friend_actions_.contains(user);
        message_button->setEnabled(connection_available_ && !pending && state != chat::friendship_state::outgoing_pending);
        if (remove_contact_button)
        {
            remove_contact_button->setVisible(state == chat::friendship_state::accepted);
            remove_contact_button->setEnabled(connection_available_);
        }
        reject_request->setVisible(user != self_user_ && state == chat::friendship_state::incoming_pending);
        cancel_request->setVisible(user != self_user_ && state == chat::friendship_state::outgoing_pending);
        reject_request->setEnabled(connection_available_ && !pending);
        cancel_request->setEnabled(connection_available_ && !pending);
    };
    update_contact();
    connect(contacts_, &QAbstractItemModel::modelReset, &dialog, update_contact);
    connect(contacts_, &QAbstractItemModel::dataChanged, &dialog, update_contact);
    connect(this, &chat_widget::friendship_updated, &dialog, update_contact);
    connect(reject_request,
            &QPushButton::clicked,
            &dialog,
            [this, user, username, reject_request, message_button]
            {
                if (!connection_available_ || friend_state(user) != chat::friendship_state::incoming_pending)
                {
                    return;
                }
                reject_request->setEnabled(false);
                message_button->setEnabled(false);
                request_friend_action(user, username, -1);
            });
    connect(cancel_request,
            &QPushButton::clicked,
            &dialog,
            [this, user, username, cancel_request]
            {
                if (!connection_available_ || friend_state(user) != chat::friendship_state::outgoing_pending)
                {
                    return;
                }
                cancel_request->setEnabled(false);
                request_friend_action(user, username, static_cast<int>(chat::friendship_state::outgoing_pending));
            });
    connect(this,
            &chat_widget::contact_add_finished,
            &dialog,
            [user, contact_status, update_contact](qint64 changed, QString error)
            {
                if (changed != user)
                {
                    return;
                }
                contact_status->setText(error);
                if (!error.isEmpty())
                {
                    update_contact();
                }
            });
    if (user == self_user_)
    {
        message_button->hide();
        // Relationship status only concerns other people; an empty row would only cost height.
        contact_status->hide();
    }
    connect(message_button,
            &QToolButton::clicked,
            &dialog,
            [this, &dialog, user, username, message_button, contact_status]
            {
                if (!connection_available_ || user == self_user_)
                {
                    return;
                }
                if (is_contact(user))
                {
                    dialog.accept();
                    open_chat(user, username);
                }
                else if (friend_state(user) != chat::friendship_state::outgoing_pending)
                {
                    message_button->setEnabled(false);
                    message_button->setText(QStringLiteral("正在处理…"));
                    contact_status->clear();
                    request_friend_action(user, username, static_cast<int>(friend_state(user)));
                }
            });
}

void chat_widget::setup_profile_avatar(QDialog& dialog, QVBoxLayout* info_layout, qint64 user)
{
    auto* info = info_layout->parentWidget();
    auto* change = new QPushButton(QStringLiteral("更换头像"), info);
    change->setObjectName(QStringLiteral("changeAvatarButton"));
    change->setProperty("primary", true);
    auto* remove = new QPushButton(QStringLiteral("移除头像"), info);
    remove->setObjectName(QStringLiteral("removeAvatarButton"));
    auto* status = new QLabel(info);
    status->setObjectName(QStringLiteral("avatarUploadStatus"));
    status->setWordWrap(true);
    info_layout->addWidget(change);
    info_layout->addWidget(remove);
    info_layout->addWidget(status);
    auto update = [this, user, change, remove, status]
    {
        change->setEnabled(connection_available_ && !avatar_updating_);
        remove->setEnabled(connection_available_ && !avatar_updating_ && avatars_.state(user).present);
        if (avatar_updating_)
        {
            status->setText(QStringLiteral("正在更新头像…"));
        }
        status->setVisible(!status->text().isEmpty());
    };
    update();
    connect(&avatars_, &avatar_cache::changed, &dialog, [update](qint64) { update(); });
    connect(this,
            &chat_widget::avatar_update_finished,
            &dialog,
            [status, update](QString error)
            {
                status->setText(error);
                update();
            });
    connect(change,
            &QPushButton::clicked,
            &dialog,
            [this, &dialog, status, update]
            {
                auto bytes = choose_avatar(&dialog);
                if (!bytes)
                {
                    status->setText(bytes.error());
                    status->show();
                    return;
                }
                if (bytes->isEmpty())
                {
                    return;
                }
                avatar_updating_ = true;
                update();
                emit avatar_set_requested(std::move(*bytes));
            });
    connect(remove,
            &QPushButton::clicked,
            &dialog,
            [this, update]
            {
                avatar_updating_ = true;
                update();
                emit avatar_clear_requested();
            });
}

void chat_widget::set_message_status(QString message)
{
    message_status_->setText(std::move(message));
    message_status_->setVisible(active_conversation_ > 0 && !message_status_->text().isEmpty());
}

void chat_widget::update_chat_header(QString const& username)
{
    auto const active = active_conversation_ > 0;
    chat_headers_->setVisible(active);
    empty_conversation_hint_->setVisible(!active);
    connection_status_->setVisible(active && !connection_status_->text().isEmpty());
    if (!active)
    {
        return;
    }
    auto const item = conversation(active_conversation_);
    const auto name = item ? item->display_name() : username;
    chat_name_->setText(name);
    chat_name_->setToolTip(name);
    chat_title_->setAccessibleName(name);
    chat_title_->setToolTip(chat_title_->accessibleName());
    const auto image = avatars_.image(active_group_ ? -active_conversation_ : active_peer_);
    chat_title_->setIcon(avatar_icon(active_group_ ? name : username, 38, image, active_group_ ? active_conversation_ : 0));
    chat_title_->setEnabled(true);
    update_chat_status();
}

void chat_widget::finish_avatar_update(QString error)
{
    avatar_updating_ = false;
    emit avatar_update_finished(std::move(error));
}
