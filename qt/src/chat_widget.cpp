#include "chat_widget.hpp"
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
#include <QSortFilterProxyModel>
#include <QScrollBar>
#include <QSize>
#include <QSizePolicy>
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

class chat_title_button final : public QPushButton
{
   public:
    using QPushButton::QPushButton;

   protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        auto const contents = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
        auto const icon_width = option.icon.isNull() ? 0 : option.iconSize.width() + 4;
        option.text = fontMetrics().elidedText(option.text, Qt::ElideRight, std::max(0, contents.width() - icon_width));
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_PushButton, option);
    }
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

QHBoxLayout* make_contact_header(QDialog* dialog, QString title)
{
    auto* header = new QHBoxLayout;
    header->setContentsMargins(18, 12, 12, 4);
    auto* label = new QLabel(std::move(title), dialog);
    label->setObjectName(QStringLiteral("dialogTitle"));
    header->addWidget(label, 1);
    auto* close = new QToolButton(dialog);
    close->setObjectName(QStringLiteral("contactDialogCloseButton"));
    close->setAccessibleName(QStringLiteral("关闭") + dialog->windowTitle());
    close->setToolTip(close->accessibleName());
    close->setFixedSize(40, 40);
    close->setIcon(svg_icon(u"close", themed("text_secondary"), QSize(20, 20)));
    close->setCursor(Qt::PointingHandCursor);
    QObject::connect(close, &QToolButton::clicked, dialog, &QDialog::reject);
    QObject::connect(&theme_manager::instance(),
                     &theme_manager::changed,
                     close,
                     [close] { close->setIcon(svg_icon(u"close", themed("text_secondary"), QSize(20, 20))); });
    header->addWidget(close);
    return header;
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

    auto* conversation_panel = create_sidebar();
    auto* chat_panel = create_chat_panel();
    layout->addWidget(conversation_panel);
    layout->addWidget(make_separator(this));
    layout->addWidget(chat_panel, 1);
    connect_chat_controls();
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
    auto* conversation_header_layout = new QHBoxLayout(conversation_header);
    conversation_header_layout->setContentsMargins(12, 16, 12, 8);
    conversation_header_layout->setSpacing(6);
    main_menu_button_ = new QToolButton(conversation_header);
    main_menu_button_->setObjectName(QStringLiteral("mainMenuButton"));
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
    self_profile_action_ = main_menu->addAction(QStringLiteral("我的资料"));
    self_profile_action_->setObjectName(QStringLiteral("selfProfileAction"));
    chats_action_ = main_menu->addAction(QStringLiteral("聊天"));
    chats_action_->setObjectName(QStringLiteral("showChatsAction"));
    chats_action_->setIcon(svg_icon(u"chat", themed("text"), QSize(20, 20)));
    contacts_action_ = main_menu->addAction(QStringLiteral("联系人"));
    contacts_action_->setObjectName(QStringLiteral("showContactsAction"));
    contacts_action_->setIcon(svg_icon(u"contacts", themed("text"), QSize(20, 20)));
    chats_action_->setCheckable(true);
    main_menu->addAction(QStringLiteral("新建会话分组"),
                         this,
                         [this]
                         {
                             if (connection_available_)
                             {
                                 edit_folder_name(0);
                             }
                         });
    main_menu->addAction(QStringLiteral("所有会话标为已读"),
                         this,
                         [this]
                         {
                             if (connection_available_)
                             {
                                 emit mark_read_scope(0, -1);
                             }
                         });
    auto* search_messages = main_menu->addAction(QStringLiteral("全局消息搜索"));
    connect(search_messages, &QAction::triggered, this, [this] { emit global_search_requested(); });
    auto* bookmarks = main_menu->addAction(QStringLiteral("消息收藏"));
    connect(bookmarks, &QAction::triggered, this, &chat_widget::bookmarks_requested);
    auto* mentions = main_menu->addAction(QStringLiteral("提及我"));
    mentions->setObjectName(QStringLiteral("mentionsMeAction"));
    connect(mentions, &QAction::triggered, this, [this] { emit global_search_requested(true); });
    main_menu->addSeparator();
    auto* add_friend = main_menu->addAction(QStringLiteral("添加联系人"));
    add_friend->setObjectName(QStringLiteral("addFriendAction"));
    add_friend->setIcon(svg_icon(u"user-plus", themed("text"), QSize(20, 20)));
    auto* create_group = main_menu->addAction(QStringLiteral("新建群组"));
    create_group->setObjectName(QStringLiteral("createGroupAction"));
    create_group->setIcon(svg_icon(u"groups", themed("text"), QSize(20, 20)));
    auto* join_group = main_menu->addAction(QStringLiteral("加入群组"));
    join_group->setObjectName(QStringLiteral("joinGroupAction"));
    join_group->setIcon(svg_icon(u"groups", themed("text"), QSize(20, 20)));
    main_menu->addSeparator();
    auto* unavailable_texts = main_menu->addMenu(QStringLiteral("未显示会话的本地内容"));
    unavailable_texts->setObjectName(QStringLiteral("unavailableLocalMessages"));
    connect(main_menu,
            &QMenu::aboutToShow,
            this,
            [this, unavailable_texts]
            {
                unavailable_texts->clear();
                for (auto it = drafts_.cbegin(); it != drafts_.cend(); ++it)
                {
                    if (it->text.isEmpty() || conversation(it.key()))
                    {
                        continue;
                    }
                    auto* actions = unavailable_texts->addMenu(QStringLiteral("草稿 · ") + grapheme_prefix(it->text, 40));
                    auto const id = it.key();
                    connect(actions->addAction(QStringLiteral("复制")),
                            &QAction::triggered,
                            this,
                            [this, id] { QGuiApplication::clipboard()->setText(drafts_.value(id).text); });
                    connect(actions->addAction(QStringLiteral("删除本地草稿")),
                            &QAction::triggered,
                            this,
                            [this, id]
                            {
                                drafts_.remove(id);
                                emit draft_save_requested(self_user_, id, chat::client::new_client_id(), 0, {}, {});
                            });
                }
                for (auto const& item : outgoing_)
                {
                    if (!item.path.isEmpty() || (!item.failed && !item.waiting) || conversation(item.conversation))
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
                    auto* discard = actions->addAction(QStringLiteral("删除本地待发送记录"));
                    connect(discard, &QAction::triggered, this, [this, id] { emit pending_text_action_requested(id, true); });
                }
                unavailable_texts->menuAction()->setVisible(!unavailable_texts->isEmpty());
            });
    auto* transfers = main_menu->addMenu(QStringLiteral("附件上传"));
    connect(transfers,
            &QMenu::aboutToShow,
            this,
            [this, transfers]
            {
                transfers->clear();
                for (auto const& item : outgoing_)
                {
                    if (item.path.isEmpty())
                    {
                        continue;
                    }
                    auto const name = conversations_->index_for_conversation(item.conversation).data(Qt::DisplayRole).toString();
                    auto const destination = name.isEmpty() ? QStringLiteral("会话 #%1").arg(item.conversation) : name;
                    auto const target = conversation(item.conversation);
                    auto const muted = target && target->speech_blocked();
                    auto const state = muted && !item.unavailable ? QStringLiteral("群聊禁言，上传已暂停") : item.error;
                    auto* actions =
                        transfers->addMenu(destination + QStringLiteral(" · ") + (item.group_only ? QStringLiteral("群文件 · ") : QString{}) +
                                           item.filename + QStringLiteral(" · ") + state);
                    auto id = item.client_id;
                    auto* toggle = actions->addAction(item.failed ? QStringLiteral("继续上传") : QStringLiteral("暂停上传"));
                    // A hidden conversation has no sidebar row. The resume RPC still checks membership and speech.
                    toggle->setEnabled(connection_available_ && (!item.failed || (!item.unavailable && !muted)));
                    connect(toggle,
                            &QAction::triggered,
                            this,
                            [this, id]
                            {
                                auto it = std::ranges::find(outgoing_, id, &outgoing::client_id);
                                if (it == outgoing_.end())
                                {
                                    return;
                                }
                                if (it->failed)
                                {
                                    it->failed = false;
                                    it->error = QStringLiteral("正在校验文件…");
                                    show_outgoing();
                                    submit(*it);
                                }
                                else
                                {
                                    emit attachment_interrupt_requested(id, false, it->path);
                                }
                            });
                    auto* cancel = actions->addAction(QStringLiteral("取消上传"));
                    cancel->setEnabled(connection_available_);
                    connect(cancel, &QAction::triggered, this, [this, id, path = item.path] { emit attachment_interrupt_requested(id, true, path); });
                }
                if (transfers->isEmpty())
                {
                    transfers->addAction(QStringLiteral("没有未完成的上传"))->setEnabled(false);
                }
            });
    auto* downloads = main_menu->addAction(QStringLiteral("下载任务"));
    connect(downloads, &QAction::triggered, this, &chat_widget::download_tasks_requested);
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
    conversation_search_->setAccessibleName(QStringLiteral("搜索聊天"));
    auto* conversation_search_icon =
        conversation_search_->addAction(svg_icon(u"search", themed("text_muted"), QSize(16, 16)), QLineEdit::LeadingPosition);
    connect(&theme_manager::instance(),
            &theme_manager::changed,
            conversation_search_,
            [conversation_search_icon] { conversation_search_icon->setIcon(svg_icon(u"search", themed("text_muted"), QSize(16, 16))); });
    conversation_header_layout->addWidget(conversation_search_, 1);
    conversation_layout->addWidget(conversation_header);

    auto* conversations_page = new QWidget(conversation_panel);
    auto* conversations_layout = new QVBoxLayout(conversations_page);
    conversations_layout->setContentsMargins(0, 0, 0, 0);
    conversations_layout->setSpacing(0);
    conversations_status_ = new QLabel(conversations_page);
    conversations_status_->setObjectName(QStringLiteral("subtleText"));
    conversations_status_->setContentsMargins(18, 0, 14, 6);
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
                input->setPlaceholderText(QStringLiteral("例如 K7QM-3XWP"));
                input->setAccessibleName(QStringLiteral("邀请码"));
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

void chat_widget::show_conversation_menu(QPoint point)
{
    auto const ix = conversations_view_->indexAt(point);
    if (connection_available_ && ix.data(conversation_model::folder_header_role).toBool())
    {
        folder_menu(point, ix);
        return;
    }
    auto const* item = conversations_->conversation_at(ix);
    if (!connection_available_ || !item)
    {
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
    auto* levels = menu.addMenu(QStringLiteral("通知"));
    auto add_level = [&, conversation](QString title, chat::notification_level level, qint64 duration)
    {
        auto* action = levels->addAction(
            title,
            this,
            [this, conversation, level, duration]
            { emit notification_level_requested(conversation, level, duration ? QDateTime::currentMSecsSinceEpoch() + duration : 0); });
        auto effective =
            item->notifications == chat::notification_level::muted && !item->muted() ? chat::notification_level::all : item->notifications;
        action->setCheckable(true);
        action->setChecked(effective == level && (level != chat::notification_level::muted || (!duration && !item->mute_until)));
    };
    add_level(QStringLiteral("全部通知"), chat::notification_level::all, 0);
    add_level(item->group ? QStringLiteral("仅 @我") : QStringLiteral("仅直接发给我"), chat::notification_level::mentions, 0);
    add_level(QStringLiteral("免打扰 1 小时"), chat::notification_level::muted, 3600000);
    add_level(QStringLiteral("免打扰 8 小时"), chat::notification_level::muted, 28800000);
    add_level(QStringLiteral("永久免打扰"), chat::notification_level::muted, 0);
    if (item->muted() && item->mute_until > 0)
    {
        levels->addAction(QStringLiteral("静音至 %1").arg(QDateTime::fromMSecsSinceEpoch(item->mute_until).toString("MM-dd HH:mm")))
            ->setEnabled(false);
    }
    menu.addAction(QStringLiteral("标为已读"), this, [this, conversation] { emit mark_read_scope(conversation, -1); });
    auto const mark = !item->marked_unread;
    menu.addAction(mark ? QStringLiteral("标记未读") : QStringLiteral("清除未读标记"),
                   this,
                   [this, conversation, mark] { emit unread_mark_requested(conversation, mark); });
    menu.addAction(QStringLiteral("隐藏会话"), [this, conversation] { emit history_view_requested(conversation, true); });
    menu.addAction(QStringLiteral("清空聊天记录"), [this, conversation] { emit history_view_requested(conversation, false); });
    menu.exec(conversations_view_->viewport()->mapToGlobal(point));
}

namespace
{
class contact_filter final : public QSortFilterProxyModel
{
   public:
    explicit contact_filter(QComboBox& group) : QSortFilterProxyModel(&group), group_(group) {}

   protected:
    bool filterAcceptsRow(int row, QModelIndex const& parent) const override
    {
        return (group_.currentIndex() <= 0 ||
                sourceModel()->index(row, 0, parent).data(user_model::friend_group_role).toString() == group_.currentData().toString()) &&
               QSortFilterProxyModel::filterAcceptsRow(row, parent);
    }

   private:
    QComboBox& group_;
};
}    // namespace

void chat_widget::set_friend_groups(QStringList const& groups)
{
    auto selected = friend_groups_->currentData();
    {
        QSignalBlocker block(friend_groups_);
        friend_groups_->clear();
        friend_groups_->addItem(QStringLiteral("全部好友"));
        friend_groups_->addItem(QStringLiteral("我的好友"), QStringLiteral(""));
        for (auto const& name : groups)
        {
            friend_groups_->addItem(name, name);
        }
        friend_groups_->setCurrentIndex(std::max(0, friend_groups_->findData(selected)));
    }
    contacts_filter_->invalidate();
}

void chat_widget::setup_contacts_dialog()
{
    contacts_dialog_ = new QDialog(this);
    contacts_dialog_->setObjectName(QStringLiteral("contactsDialog"));
    contacts_dialog_->setWindowTitle(QStringLiteral("联系人"));
    contacts_dialog_->setAccessibleName(QStringLiteral("联系人"));
    contacts_dialog_->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    contacts_dialog_->setWindowModality(Qt::WindowModal);
    contacts_dialog_->resize(420, 480);
    contacts_dialog_->setMinimumSize(360, 240);
    auto* contacts_layout = new QVBoxLayout(contacts_dialog_);
    contacts_layout->setContentsMargins(0, 0, 0, 0);
    contacts_layout->setSpacing(0);
    auto* contacts_header = make_contact_header(contacts_dialog_, QStringLiteral("联系人"));
    contacts_title_ = contacts_dialog_->findChild<QLabel*>(QStringLiteral("dialogTitle"));
    contacts_title_->setObjectName(QStringLiteral("contactsTitle"));
    contacts_back_button_ = new QToolButton(contacts_dialog_);
    contacts_back_button_->setObjectName(QStringLiteral("contactsBackButton"));
    contacts_back_button_->setAccessibleName(QStringLiteral("返回联系人"));
    contacts_back_button_->setToolTip(QStringLiteral("返回联系人"));
    contacts_back_button_->setFixedSize(40, 40);
    contacts_back_button_->setIcon(svg_icon(u"back", themed("text_secondary"), QSize(20, 20)));
    contacts_back_button_->hide();
    contacts_header->insertWidget(0, contacts_back_button_);
    contacts_header->setSpacing(8);
    contacts_layout->addLayout(contacts_header);
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
    connect(contacts_back_button_, &QToolButton::clicked, this, [this] { show_contacts_home(); });
    connect(contacts_dialog_, &QDialog::finished, this, [this] { show_contacts_home(); });
    contact_search_ = new QLineEdit(contacts_dialog_);
    contact_search_->setObjectName(QStringLiteral("userSearchEdit"));
    contact_search_->setPlaceholderText(QStringLiteral("搜索联系人"));
    contact_search_->setAccessibleName(QStringLiteral("搜索联系人"));
    contact_search_->setToolTip(QStringLiteral("搜索已确认的好友，按 Enter 选择结果"));
    contact_search_->setClearButtonEnabled(false);
    contact_search_->setProperty("header", true);
    auto* contact_search_icon = contact_search_->addAction(svg_icon(u"search", themed("text_muted"), QSize(16, 16)), QLineEdit::LeadingPosition);
    connect(&theme_manager::instance(),
            &theme_manager::changed,
            contact_search_,
            [contact_search_icon] { contact_search_icon->setIcon(svg_icon(u"search", themed("text_muted"), QSize(16, 16))); });
    auto* contact_search_layout = new QHBoxLayout;
    contact_search_layout->setContentsMargins(12, 0, 12, 8);
    contact_search_layout->addWidget(contact_search_, 1);
    contacts_add_button_ = new QToolButton(contacts_dialog_);
    contacts_add_button_->setObjectName(QStringLiteral("contactsAddButton"));
    contacts_add_button_->setAccessibleName(QStringLiteral("添加联系人"));
    contacts_add_button_->setToolTip(QStringLiteral("添加联系人"));
    contacts_add_button_->setFixedSize(40, 40);
    contacts_add_button_->setCursor(Qt::PointingHandCursor);
    contacts_add_button_->setIcon(svg_icon(u"user-plus", themed("text_secondary"), QSize(20, 20)));
    contact_search_layout->addWidget(contacts_add_button_);
    connect(contacts_add_button_, &QToolButton::clicked, this, [this] { show_add_contact_dialog(); });
    contacts_home_layout->addLayout(contact_search_layout);
    contacts_requests_button_ = new QPushButton(contacts_home);
    contacts_requests_button_->setAutoDefault(false);
    contacts_requests_button_->setObjectName(QStringLiteral("contactsRequestsButton"));
    contacts_requests_button_->setAccessibleName(QStringLiteral("好友申请"));
    contacts_requests_button_->setToolTip(QStringLiteral("查看收到和发出的好友申请"));
    contacts_requests_button_->setFixedHeight(64);
    contacts_requests_button_->setCursor(Qt::PointingHandCursor);
    auto* requests_entry_layout = new QHBoxLayout(contacts_requests_button_);
    requests_entry_layout->setContentsMargins(18, 8, 16, 8);
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
    contacts_requests_button_->hide();
    contacts_home_layout->addWidget(contacts_requests_button_);
    connect(contacts_requests_button_, &QPushButton::clicked, this, [this] { show_friend_requests(); });
    contacts_status_ = new QLabel(QStringLiteral("暂无联系人，可从主菜单“添加联系人”发起好友申请"), contacts_dialog_);
    contacts_status_->setObjectName(QStringLiteral("contactsStatus"));
    contacts_status_->setContentsMargins(18, 8, 14, 8);
    contacts_status_->setWordWrap(true);
    contacts_home_layout->addWidget(contacts_status_);

    contacts_ = new user_model(this, &avatars_);
    friend_groups_ = new QComboBox(contacts_home);
    friend_groups_->setObjectName(QStringLiteral("friendGroupFilter"));
    auto* group_row = new QHBoxLayout;
    group_row->addWidget(friend_groups_, 1);
    auto* manage = new QPushButton(QStringLiteral("管理分组"), contacts_home);
    group_row->addWidget(manage);
    contacts_home_layout->addLayout(group_row);
    contacts_filter_ = new contact_filter(*friend_groups_);
    connect(friend_groups_,
            &QComboBox::currentIndexChanged,
            this,
            [this]
            {
                contacts_filter_->invalidate();
                filter_contacts(contact_search_->text());
            });
    set_friend_groups({});
    connect(manage,
            &QPushButton::clicked,
            this,
            [this, manage]
            {
                if (!connection_available_)
                {
                    return;
                }
                auto group = friend_groups_->currentData().toString();
                QMenu menu(contacts_dialog_);
                auto* create = menu.addAction(QStringLiteral("新建分组"));
                auto* rename = menu.addAction(QStringLiteral("重命名分组"));
                auto* remove = menu.addAction(QStringLiteral("删除分组"));
                rename->setEnabled(!group.isEmpty());
                remove->setEnabled(!group.isEmpty());
                auto* action = menu.exec(manage->mapToGlobal(QPoint(0, manage->height())));
                if (!action)
                {
                    return;
                }
                if (action == remove)
                {
                    if (confirm_action(
                            contacts_dialog_, QStringLiteral("删除分组"), QStringLiteral("该组好友将移回「我的好友」。"), QStringLiteral("删除")))
                    {
                        emit delete_friend_group_requested(group);
                    }
                    return;
                }
                bool ok = false;
                auto name = QInputDialog::getText(contacts_dialog_,
                                                  action->text(),
                                                  QStringLiteral("分组名称（最多 32 个字符）"),
                                                  QLineEdit::Normal,
                                                  action == rename ? group : QString{},
                                                  &ok)
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
            });
    contacts_filter_->setSourceModel(contacts_);
    contacts_filter_->setFilterRole(user_model::search_role);
    contacts_filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    contacts_filter_->setSortRole(user_model::username_role);
    contacts_filter_->setSortLocaleAware(true);
    contacts_filter_->sort(0);
    contacts_view_ = new QListView(contacts_dialog_);
    contacts_view_->setObjectName(QStringLiteral("userList"));
    contacts_view_->setAccessibleName(QStringLiteral("联系人列表"));
    contacts_view_->setModel(contacts_filter_);
    contacts_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(contacts_view_,
            &QWidget::customContextMenuRequested,
            this,
            [this](QPoint point)
            {
                auto const* item = contacts_->user_at(contacts_filter_->mapToSource(contacts_view_->indexAt(point)));
                if (!item || !connection_available_)
                {
                    return;
                }
                auto user = item->id;
                auto current = item->remark;
                QMenu menu(contacts_dialog_);
                auto* remark = menu.addAction(QStringLiteral("编辑备注"));
                auto* move = menu.addMenu(QStringLiteral("移动到分组"));
                for (int i = 1; i < friend_groups_->count(); ++i)
                {
                    auto* group = move->addAction(friend_groups_->itemText(i));
                    group->setData(friend_groups_->itemData(i));
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
                auto value = QInputDialog::getText(contacts_dialog_,
                                                   QStringLiteral("编辑备注"),
                                                   QStringLiteral("备注（最多 32 个字符，留空恢复用户名）"),
                                                   QLineEdit::Normal,
                                                   current,
                                                   &ok);
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
    auto* contacts_delegate = new user_delegate(contacts_view_);
    contacts_view_->setItemDelegate(contacts_delegate);
    contacts_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    contacts_view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    contacts_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    contacts_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    contacts_view_->setMouseTracking(true);
    contacts_view_->verticalScrollBar()->setSingleStep(24);
    contacts_home_layout->addWidget(contacts_view_, 1);
    connect(contact_search_,
            &QLineEdit::returnPressed,
            this,
            [this]
            {
                if (contacts_filter_->rowCount() == 0)
                {
                    return;
                }
                auto index = contacts_view_->currentIndex();
                if (!index.isValid())
                {
                    index = contacts_filter_->index(0, 0);
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
                auto const* user = contacts_->user_at(contacts_filter_->mapToSource(index));
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
    add_contact_dialog_->setWindowTitle(QStringLiteral("添加联系人"));
    add_contact_dialog_->setAccessibleName(QStringLiteral("添加联系人"));
    add_contact_dialog_->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    add_contact_dialog_->setWindowModality(Qt::WindowModal);
    add_contact_dialog_->setMinimumWidth(420);
    auto* add_contacts_layout = new QVBoxLayout(add_contact_dialog_);
    add_contacts_layout->setContentsMargins(0, 0, 0, 0);
    add_contacts_layout->setSpacing(12);
    add_contacts_layout->addLayout(make_contact_header(add_contact_dialog_, QStringLiteral("添加联系人")));
    add_user_search_ = new QLineEdit(add_contact_dialog_);
    add_user_search_->setObjectName(QStringLiteral("userSearchEdit"));
    add_user_search_->setPlaceholderText(QStringLiteral("搜索用户名或昵称"));
    add_user_search_->setAccessibleName(QStringLiteral("搜索用户"));
    add_user_search_->setClearButtonEnabled(false);
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
    auto* add_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, add_contact_dialog_);
    add_buttons->setContentsMargins(18, 0, 18, 16);
    add_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    add_buttons->button(QDialogButtonBox::Cancel)->setIcon({});
    add_search_button_ = add_buttons->addButton(QStringLiteral("搜索"), QDialogButtonBox::ActionRole);
    add_search_button_->setObjectName(QStringLiteral("addContactSearchButton"));
    add_search_button_->setProperty("primary", true);
    add_search_button_->setDefault(true);
    add_buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    add_search_button_->setEnabled(false);
    add_contacts_layout->addWidget(add_buttons);
    connect(add_buttons, &QDialogButtonBox::rejected, add_contact_dialog_, &QDialog::reject);
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
    return chat_panel;
}

void chat_widget::setup_chat_header(QVBoxLayout* chat_layout)
{
    auto* chat_panel = chat_layout->parentWidget();
    auto* header = new QFrame(chat_panel);
    header->setObjectName(QStringLiteral("chatHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(16, 5, 12, 5);
    header_layout->setSpacing(8);

    auto* chat_identity = new QWidget(header);
    auto* chat_identity_layout = new QVBoxLayout(chat_identity);
    chat_identity_layout->setContentsMargins(0, 0, 0, 0);
    chat_identity_layout->setSpacing(0);
    chat_title_ = new chat_title_button(QStringLiteral("聊天"), chat_identity);
    chat_title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    header_layout->addWidget(chat_identity, 1);
    unread_start_button_ = new QPushButton(QStringLiteral("查看本次未读起点"), header);
    unread_start_button_->setObjectName(QStringLiteral("entryUnreadButton"));
    unread_start_button_->setToolTip(QStringLiteral("查看进入会话前的第一条未读消息，不改变当前已读位置"));
    latest_button_ = new QPushButton(QStringLiteral("回到最新"), header);
    latest_button_->setObjectName(QStringLiteral("returnToLatestButton"));
    for (auto* button : {unread_start_button_, latest_button_})
    {
        button->setAutoDefault(false);
        button->hide();
    }
    header_layout->addWidget(unread_start_button_);
    header_layout->addWidget(latest_button_);
    connect(unread_start_button_, &QPushButton::clicked, this, [this] { emit message_context_requested(active_conversation_, unread_start_); });
    connect(latest_button_, &QPushButton::clicked, this, &chat_widget::return_to_latest);

    // Keyword search is a shortcut (Ctrl+F) rather than a header icon, as in WeChat and QQ;
    // the composer's chat history window also searches.
    message_search_action_ = new QAction(QStringLiteral("搜索消息"), this);
    message_search_action_->setObjectName(QStringLiteral("messageSearchAction"));
    message_search_action_->setShortcut(QKeySequence::Find);
    message_search_action_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    message_search_action_->setEnabled(false);
    addAction(message_search_action_);
    chat_more_button_ = new QToolButton(header);
    chat_more_button_->setObjectName(QStringLiteral("chatMoreButton"));
    chat_more_button_->setToolTip(QStringLiteral("聊天信息"));
    chat_more_button_->setAccessibleName(QStringLiteral("聊天信息"));
    chat_more_button_->setIcon(svg_icon(u"more", themed("text"), QSize(20, 20)));
    chat_more_button_->setIconSize(QSize(20, 20));
    chat_more_button_->setFixedSize(36, 36);
    chat_more_button_->setCursor(Qt::PointingHandCursor);
    chat_more_button_->setEnabled(false);
    header_layout->addWidget(chat_more_button_);
    // The header icons replace the old text button and lead to the same chat or group details.
    connect(chat_more_button_,
            &QToolButton::clicked,
            this,
            [this]
            {
                if (chat_title_->isEnabled())
                {
                    chat_title_->click();
                }
            });
    connect(message_search_action_,
            &QAction::triggered,
            this,
            [this] { emit message_search_requested(active_conversation_, self_user_, active_group_, active_username_, {}); });
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
    connect(pinned_message_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto const item = conversation(active_conversation_);
                if (item && item->pinned_message.id > 0)
                {
                    locate_message(active_conversation_, item->pinned_message.id);
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

    auto* chat_line = new QFrame(chat_panel);
    chat_line->setFrameShape(QFrame::HLine);
    chat_line->setObjectName(QStringLiteral("horizontalSeparator"));
    chat_layout->addWidget(chat_line);

    message_status_ = new QLabel(QStringLiteral("选择一个会话开始聊天"), chat_panel);
    message_status_->setObjectName(QStringLiteral("subtleText"));
    message_status_->setTextFormat(Qt::PlainText);
    message_status_->setAlignment(Qt::AlignCenter);
    message_status_->setContentsMargins(0, 6, 0, 6);
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
    messages_view_ = new QListView(chat_panel);
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
    connect(messages_view_->verticalScrollBar(), &QScrollBar::rangeChanged, this, &chat_widget::update_reading_navigation);

    selection_bar_ = new QWidget(chat_panel);
    auto* selection = new QHBoxLayout(selection_bar_);
    selection_count_ = new QLabel(selection_bar_);
    selection_count_->setObjectName(QStringLiteral("selectedMessageCount"));
    selection->addWidget(selection_count_, 1);
    selection_copy_ = new QPushButton(QStringLiteral("复制"), selection_bar_);
    selection_forward_ = new QPushButton(QStringLiteral("转发"), selection_bar_);
    auto* cancel = new QPushButton(QStringLiteral("退出多选"), selection_bar_);
    for (auto* button : {selection_copy_, selection_forward_, cancel})
    {
        button->installEventFilter(this);
        selection->addWidget(button);
    }
    connect(messages_view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &chat_widget::update_message_selection);
    connect(messages_, &QAbstractItemModel::modelReset, this, &chat_widget::update_message_selection);
    connect(messages_,
            &QAbstractItemModel::dataChanged,
            this,
            [this](QModelIndex const&, QModelIndex const&, QList<int> const& roles)
            {
                // Retraction changes which indexes are selectable without emitting selectionChanged.
                if (messages_view_->selectionMode() == QAbstractItemView::ExtendedSelection && roles.isEmpty())
                {
                    update_message_selection();
                }
            });
    connect(selection_copy_, &QPushButton::clicked, this, &chat_widget::copy_selected_messages);
    connect(selection_forward_,
            &QPushButton::clicked,
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
    connect(cancel, &QPushButton::clicked, this, [this] { set_message_selection(false); });
    update_message_selection();
    selection_bar_->hide();
    chat_layout->addWidget(selection_bar_);
    reply_bar_ = new QWidget(chat_panel);
    auto* reply_layout = new QHBoxLayout(reply_bar_);
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
    connect(messages_delegate,
            &message_delegate::avatar_clicked,
            this,
            [this](QModelIndex const& index)
            { show_user_details(index.data(message_model::from_role).toLongLong(), index.data(message_model::sender_name_role).toString()); });
    connect(messages_delegate,
            &message_delegate::read_details_clicked,
            this,
            [this](QModelIndex const& index)
            {
                if (index.data(message_model::read_count_role).isValid())
                {
                    show_read_details(active_conversation_, index.data(message_model::id_role).toLongLong());
                }
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
    auto* input_separator = new QFrame(chat_panel);
    input_separator->setFrameShape(QFrame::HLine);
    input_separator->setObjectName(QStringLiteral("horizontalSeparator"));
    chat_layout->addWidget(input_separator);
    // The composer is drawn as a rounded card, which already separates it from the history.
    input_separator->hide();

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
    input_bar->setMinimumHeight(chat_theme::compose_height);
    auto* input_layout = new QHBoxLayout(input_bar);
    input_layout->setContentsMargins(8, 5, 8, 5);
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
    message_edit_->viewport()->setAcceptDrops(true);
    message_edit_->viewport()->installEventFilter(this);
    message_edit_->setEnabled(false);
    new emoji_highlighter(message_edit_);
    input_layout->addWidget(message_edit_, 1, Qt::AlignVCenter);
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
    // Telegram's arrangement: attach on the left of the text, the other actions on its right.
    input_layout->insertWidget(0, attachment_button_, 0, Qt::AlignBottom);
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
    input_layout->addWidget(screenshot_button_, 0, Qt::AlignBottom);
    connect(screenshot_button_,
            &QToolButton::clicked,
            this,
            [this]
            {
                auto const recipient = active_conversation_;
                auto const account = self_user_;
                screenshot_dialog dialog(this);
                if (dialog.exec() == QDialog::Accepted && active_conversation_ == recipient && self_user_ == account && can_send())
                {
                    send_image(dialog.selected_image());
                }
            });
    mention_button_ = new QToolButton(input_bar);
    mention_button_->setText(QStringLiteral("@"));
    mention_button_->setObjectName(QStringLiteral("mentionMemberButton"));
    mention_button_->setToolTip(QStringLiteral("提及群成员"));
    mention_button_->setFixedSize(chat_theme::compose_button_width, chat_theme::compose_button_height);
    input_layout->addWidget(mention_button_, 0, Qt::AlignBottom);
    auto* candidates = new QMenu(mention_button_);
    mention_button_->setMenu(candidates);
    mention_button_->setPopupMode(QToolButton::InstantPopup);
    connect(candidates,
            &QMenu::aboutToShow,
            this,
            [this, candidates]
            {
                candidates->clear();
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
    markdown_button_ = new QToolButton(input_bar);
    markdown_button_->setObjectName(QStringLiteral("markdownButton"));
    markdown_button_->setText(QStringLiteral("MD"));
    markdown_button_->setToolTip(QStringLiteral("使用 Markdown 格式发送"));
    markdown_button_->setCheckable(true);
    input_layout->addWidget(markdown_button_, 0, Qt::AlignBottom);
    connect(markdown_button_, &QToolButton::toggled, this, &chat_widget::draft_edited);
    emoji_button_ = make_composer_button(QStringLiteral("emojiButton"), u"emoji", QStringLiteral("表情"));
    history_button_ = make_composer_button(QStringLiteral("chatHistoryButton"), u"history", QStringLiteral("聊天记录"));
    input_layout->addWidget(emoji_button_, 0, Qt::AlignBottom);
    input_layout->addWidget(history_button_, 0, Qt::AlignBottom);
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
    send_button_->setIcon(svg_icon(QStringLiteral("send"), themed("on_accent"), QSize(20, 20)));
    send_button_->setIconSize(QSize(20, 20));
    send_button_->setToolTip(QStringLiteral("发送消息（Enter）"));
    send_button_->setAccessibleName(QStringLiteral("发送消息"));
    send_button_->setFixedSize(chat_theme::compose_button_width, chat_theme::compose_button_height);
    send_button_->setEnabled(false);
    send_button_->setCursor(Qt::PointingHandCursor);
    input_layout->addWidget(send_button_, 0, Qt::AlignBottom);
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
    messages_view_->clearSelection();
    messages_view_->setSelectionMode(enabled ? QAbstractItemView::ExtendedSelection : QAbstractItemView::NoSelection);
    selection_bar_->setVisible(enabled);
}
void chat_widget::update_message_selection()
{
    auto const count = messages_view_->selectionModel()->selectedIndexes().size();
    selection_count_->setText(QStringLiteral("已选 %1 条").arg(count));
    selection_copy_->setEnabled(count != 0);
    selection_forward_->setEnabled(count != 0 && connection_available_);
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
void chat_widget::choose_forward_target(QList<QPersistentModelIndex> const& rows)
{
    if (!connection_available_ || rows.isEmpty())
    {
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("选择转发目标"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* targets = new QListWidget(&dialog);
    layout->addWidget(targets);
    for (int row = 0; row < conversations_->rowCount(); ++row)
    {
        auto const ix = conversations_->index(row, 0);
        auto const* room = conversations_->conversation_at(ix);
        if (room && !room->speech_blocked())
        {
            auto* item = new QListWidgetItem(ix.data().toString(), targets);
            item->setData(Qt::UserRole, room->id);
        }
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("转发"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(targets, &QListWidget::currentRowChanged, &dialog, [buttons](int row) { buttons->button(QDialogButtonBox::Ok)->setEnabled(row >= 0); });
    connect(targets, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    auto const owner = self_user_;
    auto const visit = conversation_visit_;
    if (dialog.exec() != QDialog::Accepted || !targets->currentItem() || !connection_available_ || owner != self_user_ ||
        visit != conversation_visit_)
    {
        return;
    }
    auto const target = targets->currentItem()->data(Qt::UserRole).toLongLong();
    if (auto room = conversation(target); !room || room->speech_blocked())
    {
        set_error(QStringLiteral("转发目标当前不可用"));
        return;
    }

    // Cleared rows lose their persistent index; withdrawn rows lose selectability.
    QList<qint64> messages;
    for (auto const& row : rows)
    {
        if (row.isValid() && row.flags().testFlag(Qt::ItemIsSelectable))
        {
            messages.append(row.data(message_model::id_role).toLongLong());
        }
    }
    if (messages.isEmpty())
    {
        set_error(QStringLiteral("所选消息已不可转发"));
        return;
    }
    emit forward_requested(target, std::move(messages));
    set_message_selection(false);
}

void chat_widget::show_message_menu(QPoint position)
{
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
    auto* copy = !index.data(message_model::text_role).toString().isEmpty() ? menu.addAction(QStringLiteral("复制")) : nullptr;
    QStyleOptionViewItem option;
    option.initFrom(messages_view_);
    option.rect = messages_view_->visualRect(index);
    auto const url =
        messages_view_->selectionMode() == QAbstractItemView::ExtendedSelection ? QUrl{} : message_delegate::link_at(option, index, position);
    auto const code = message_delegate::code_at(option, index, position);
    auto* copy_code = code.isEmpty() ? nullptr : menu.addAction(QStringLiteral("复制代码"));
    auto* open_link = url.isEmpty() ? nullptr : menu.addAction(QStringLiteral("打开链接"));
    auto* copy_link = url.isEmpty() ? nullptr : menu.addAction(QStringLiteral("复制链接"));
    auto* forward = menu.addAction(QStringLiteral("转发"));
    auto* bookmark = menu.addAction(QStringLiteral("正在查询收藏状态…"));
    bookmark->setEnabled(false);
    emit bookmark_menu_requested(message_id, bookmark);
    auto* multi = menu.addAction(QStringLiteral("多选"));
    auto* reply = can_send() ? menu.addAction(QStringLiteral("回复")) : nullptr;
    auto const current = conversation(active_conversation_);
    auto const unpin = current && current->pinned_message.id == message_id;
    auto* group_pin = messages_->can_manage_group() ? menu.addAction(unpin ? QStringLiteral("取消置顶消息") : QStringLiteral("置顶消息")) : nullptr;
    auto* picker = can_send() ? menu.addMenu(QStringLiteral("表情回应")) : nullptr;
    auto const own_reaction = index.data(message_model::own_reaction_role).toString();
    auto const target_index = QPersistentModelIndex(index);
    if (!picker && !own_reaction.isEmpty())
    {
        auto const group = active_conversation_;
        menu.addAction(QStringLiteral("取消表态"),
                       [this, group, target_index]
                       {
                           if (connection_available_ && active_conversation_ == group && target_index.isValid())
                           {
                               emit reaction_requested(group, target_index.data(message_model::id_role).toLongLong(), {});
                           }
                       });
    }
    auto const conversation = active_conversation_;
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
                [this, target_index, text, conversation]
                {
                    if (can_send() && conversation == active_conversation_ && target_index.isValid() &&
                        !target_index.data(message_model::deleted_role).toBool())
                    {
                        emit reaction_requested(conversation,
                                                target_index.data(message_model::id_role).toLongLong(),
                                                text == target_index.data(message_model::own_reaction_role).toString() ? QString{} : text);
                    }
                });
    }
    auto* readers = index.data(message_model::read_count_role).isValid() ? menu.addAction(QStringLiteral("已读详情")) : nullptr;
    auto const filename = index.data(message_model::attachment_name_role).toString();
    auto const key = index.data(message_model::attachment_key_role).toString();
    if (!key.isEmpty() && index.data(message_model::outgoing_role).toBool() && connection_available_)
    {
        auto* groups = menu.addMenu(QStringLiteral("加入群文件"));
        for (int row = 0; row < conversations_->rowCount(); ++row)
        {
            auto const* group = conversations_->conversation_at(conversations_->index(row, 0));
            if (group && group->group && !group->speech_blocked())
            {
                groups->addAction(group->username, this, [this, key, id = group->id] { emit add_file_requested(id, key); });
            }
        }
        groups->setEnabled(!groups->isEmpty());
    }
    auto* download = !filename.isEmpty() ? menu.addAction(QStringLiteral("下载文件")) : nullptr;
    auto* preview = index.data(message_model::attachment_type_role).toString().startsWith(QStringLiteral("image/"))
                        ? menu.addAction(QStringLiteral("查看图片"))
                        : nullptr;
    auto* edit = can_send() && filename.isEmpty() && index.data(message_model::forwarded_from_role).toString().isEmpty() &&
                         index.data(message_model::outgoing_role).toBool()
                     ? menu.addAction(QStringLiteral("编辑"))
                     : nullptr;
    auto* remove = index.data(message_model::outgoing_role).toBool() ? menu.addAction(QStringLiteral("删除")) : nullptr;
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
    if (conversation != active_conversation_ || message_id <= history_floor(conversation) || !target_index.isValid() ||
        target_index.data(message_model::deleted_role).toBool())
    {
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
    if (selected == forward)
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
        if (connection_available_ && conversation == active_conversation_ && messages_->can_manage_group())
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
                this, QStringLiteral("删除消息"), QStringLiteral("删除后所有成员均显示“消息已删除”。确认删除？"), QStringLiteral("删除")) &&
            connection_available_ && conversation == active_conversation_)
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
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }
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
                update_typing_label();
            });
    typing_expiry->start();
    connect(messages_view_->verticalScrollBar(),
            &QScrollBar::valueChanged,
            this,
            [this](int value)
            {
                load_visible_images();
                update_reading_navigation();
                mark_visible_messages();
                if (value == messages_view_->verticalScrollBar()->minimum())
                {
                    request_older_messages();
                }
            });
    main_menu_button_->installEventFilter(this);
    contacts_dialog_->installEventFilter(this);
    contacts_back_button_->installEventFilter(this);
    contacts_requests_button_->installEventFilter(this);
    for (auto* button : {incoming_accept_button_, incoming_reject_button_, outgoing_cancel_button_})
    {
        button->installEventFilter(this);
    }
}

void chat_widget::set_user(QString const& username, qint64 user, QString const& server)
{
    flush_draft();
    contacts_feedback_->clear();
    contacts_feedback_->hide();
    contacts_dialog_->reject();
    add_contact_dialog_->reject();
    ++add_search_request_;
    stop_typing();
    typing_users_.clear();
    update_typing_label();
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
    draft_timer_->stop();
    drafts_.clear();
    outgoing_.clear();
    saving_text_.reset();
    active_peer_ = 0;
    active_group_ = false;
    reset_reply();
    active_conversation_ = 0;
    ++conversation_visit_;
    unread_start_ = 0;
    unread_pending_ = timeline_current_ = jump_to_latest_ = false;
    withdrawn_before_unread_.clear();
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
    set_friend_groups({});
    pending_friend_actions_.clear();
    set_friend_requests({}, {}, {});
    contacts_filter_->setFilterFixedString(QString{});
    contact_search_->clear();
    contacts_status_->setText(QStringLiteral("暂无联系人，可从主菜单“添加联系人”发起好友申请"));
    add_users_->set_users({});
    add_user_search_->clear();
    add_users_status_->setText(QStringLiteral("输入用户名后按 Enter 查找"));
    show_conversations_section();
    chat_title_->setText(QStringLiteral("聊天"));
    chat_title_->setToolTip({});
    chat_title_->setIcon(QIcon{});
    chat_title_->setEnabled(false);
    chat_presence_->clear();
    chat_presence_->hide();
    set_message_status(QStringLiteral("选择一个会话开始聊天"));
    message_edit_->clear();
    message_edit_->setEnabled(false);
    send_button_->setEnabled(false);
    message_search_action_->setEnabled(false);
    history_button_->setEnabled(false);
    attachment_button_->setEnabled(false);
    chat_more_button_->setEnabled(false);
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
    // The scroll areas deliver drag events to their viewport children.
    if ((object == input_bar_ || object->parent() == messages_view_ || object->parent() == message_edit_) && handle_file_drop(event))
    {
        return true;
    }
    if (object == message_edit_ && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
    {
        input_bar_->setProperty("editorFocused", event->type() == QEvent::FocusIn);
        input_bar_->style()->unpolish(input_bar_);
        input_bar_->style()->polish(input_bar_);
        input_bar_->update();
        if (event->type() == QEvent::FocusOut && static_cast<QFocusEvent*>(event)->reason() == Qt::PopupFocusReason)
        {
            // Hook the editor's own standard menu, not Chat menus that already have a close handler.
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu && (menu->parentWidget() == message_edit_ || menu->parentWidget() == message_edit_->viewport()))
            {
                resume_reading_after_menu(*menu);
            }
        }
    }
    if ((object == contacts_requests_button_ || object == contacts_back_button_ || object == incoming_accept_button_ ||
         object == incoming_reject_button_ || object == outgoing_cancel_button_) &&
        event->type() == QEvent::KeyPress)
    {
        auto const* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        {
            static_cast<QAbstractButton*>(object)->click();
            return true;
        }
    }
    if (object == contacts_dialog_ && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape &&
        contacts_pages_->currentIndex() == 1)
    {
        show_contacts_home();
        return true;
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
        if (key->modifiers() == Qt::NoModifier && (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown))
        {
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
        auto* scroll = messages_view_->verticalScrollBar();
        if (event->type() == QEvent::Resize && scroll->value() == scroll->maximum())
        {
            auto const position = scroll->value();
            auto const conversation = active_conversation_;
            QTimer::singleShot(0,
                               messages_view_,
                               [this, position, conversation]
                               {
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
    contacts_status_->setText(QStringLiteral("正在加载联系人…"));
    contacts_status_->show();
    update_request_actions();
    set_list_status(QStringLiteral("正在加载…"));
    if (active_conversation_ == 0)
    {
        set_message_status(QStringLiteral("正在加载会话…"));
    }
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
    set_list_status(message);
    if (active_conversation_ == 0)
    {
        set_message_status(std::move(message));
    }
}

// The status row takes space only while it has something to say.
void chat_widget::set_list_status(QString text)
{
    conversations_status_->setText(std::move(text));
    conversations_status_->setVisible(!conversations_status_->text().isEmpty());
}

void chat_widget::set_connection_available(bool available)
{
    if (connection_available_ == available)
    {
        return;
    }
    if (!available)
    {
        stop_typing();
        set_presences({});
        typing_users_.clear();
        update_typing_label();
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
        messages_->set_read_positions({});
    }
    for (auto* action : main_menu_button_->menu()->actions())
    {
        if (action->objectName() == QStringLiteral("addFriendAction") || action->objectName() == QStringLiteral("createGroupAction") ||
            action->objectName() == QStringLiteral("joinGroupAction"))
        {
            action->setEnabled(available);
        }
    }
    messages_loading_ = available && active_conversation_ > 0;
    if (!available)
    {
        timeline_current_ = false;
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
        if (active_conversation_ == 0)
        {
            set_message_status(QStringLiteral("暂无会话，可从主菜单添加联系人或新建群组"));
        }
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
            active_member_count_ = item->member_count;
            update_chat_header(item->username);
        }
        return;
    }

    // Opening a conversation reports it read; only the user's own choice may do that.
    conversations_view_->setCurrentIndex({});
    conversations_view_->clearSelection();
    set_message_status(QStringLiteral("选择一个会话开始聊天"));
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
    contacts_status_->setText(contacts_->rowCount() == 0 ? QStringLiteral("暂无联系人，可从主菜单“添加联系人”发起好友申请") : QString{});
    if (selected > 0)
    {
        for (int row = 0; row < contacts_filter_->rowCount(); ++row)
        {
            auto const index = contacts_filter_->index(row, 0);
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
    update_chat_presence();
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
    add_users_view_->setFixedHeight(std::min(add_users_->rowCount(), 6) * chat_theme::dialog_row_height);
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
        list->setMaximumHeight(list->count() * chat_theme::dialog_row_height);
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
    friend_requests_status_->clear();
    friend_requests_status_->hide();
    auto const incoming_count = incoming_requests_.size();
    auto const outgoing_count = outgoing_requests_.size();
    auto const has_requests = incoming_count > 0 || outgoing_count > 0;
    contacts_requests_button_->setVisible(has_requests);
    contacts_requests_badge_->setText(incoming_count > 99 ? QStringLiteral("99+") : QString::number(incoming_count));
    contacts_requests_badge_->setVisible(incoming_count > 0);
    contacts_requests_summary_->setText(incoming_count > 0 ? outgoing_count > 0
                                                                 ? QStringLiteral("%1 条待处理 · %2 条已发送").arg(incoming_count).arg(outgoing_count)
                                                                 : QStringLiteral("%1 条待处理").arg(incoming_count)
                                                           : QStringLiteral("%1 条已发送，等待确认").arg(outgoing_count));
    contacts_requests_button_->setAccessibleDescription(contacts_requests_summary_->text());
    if (!has_requests && pending_friend_actions_.isEmpty() && contacts_pages_->currentIndex() == 1)
    {
        show_contacts_home();
    }
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
                               bool older,
                               bool recovering,
                               bool has_more,
                               std::optional<qint64> first_unread)
{
    if (user != active_conversation_)
    {
        return;
    }

    bool unread_unavailable = false;
    if (!older && !recovering && unread_pending_)
    {
        unread_pending_ = false;
        if (first_unread && *first_unread > history_floor(user) && !withdrawn_before_unread_.contains(*first_unread))
        {
            unread_start_ = *first_unread;
        }
        unread_unavailable = first_unread && !unread_start_;
        withdrawn_before_unread_.clear();
    }
    for (auto const& message : messages)
    {
        settle(message);
    }
    auto* scroll = messages_view_->verticalScrollBar();
    auto const old_maximum = scroll->maximum();
    auto const old_value = scroll->value();
    for (auto it = messages.begin(); it != messages.end();)
    {
        if (visible_message(*it))
        {
            ++it;
        }
        else
        {
            it = messages.erase(it);
        }
    }
    messages_->merge_messages(std::move(messages));
    messages_->set_read_positions(std::move(positions));
    messages_loading_ = recovering && has_more;
    if (!older)
    {
        timeline_current_ = !messages_loading_;
    }
    if (!recovering)
    {
        history_exhausted_ = history_exhausted_ || !has_more;
    }

    if (!older)
    {
        messages_loaded_ = true;
        set_message_status(unread_unavailable           ? QStringLiteral("本次未读起点已不可用")
                           : messages_->rowCount() == 0 ? QStringLiteral("暂无消息")
                                                        : QString{});
        bool const follow = !recovering || old_value == old_maximum || (timeline_current_ && jump_to_latest_);
        if (timeline_current_)
        {
            jump_to_latest_ = false;
        }
        if (follow)
        {
            QTimer::singleShot(0,
                               messages_view_,
                               [this, visit = conversation_visit_]
                               {
                                   if (visit == conversation_visit_)
                                   {
                                       messages_view_->scrollToBottom();
                                       mark_visible_messages();
                                   }
                               });
        }
        update_reading_navigation();
        return;
    }

    set_message_status({});
    QTimer::singleShot(0,
                       messages_view_,
                       [this, scroll, old_maximum, old_value, visit = conversation_visit_]
                       {
                           if (visit == conversation_visit_)
                           {
                               scroll->setValue(old_value + scroll->maximum() - old_maximum);
                           }
                       });
    update_reading_navigation();
}

void chat_widget::locate_message(qint64 conversation, qint64 message)
{
    if (message <= 0)
    {
        return;
    }
    if (conversation == active_conversation_ && message > history_floor(conversation))
    {
        for (int row = 0; row < messages_->rowCount(); ++row)
        {
            auto const index = messages_->index(row, 0);
            if (index.data(message_model::id_role).toLongLong() == message && !index.data(message_model::deleted_role).toBool())
            {
                messages_view_->setCurrentIndex(index);
                messages_view_->scrollTo(index, QAbstractItemView::PositionAtCenter);
                return;
            }
        }
    }
    emit message_context_requested(conversation, message);
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
    settle(message);
    if (!visible_message(message) || user != active_conversation_)
    {
        return;
    }

    auto* scroll = messages_view_->verticalScrollBar();
    auto const position = scroll->value();
    auto const at_bottom = timeline_current_ && position == scroll->maximum();
    if (messages_->add_message(std::move(message)))
    {
        set_message_status({});
        if (at_bottom)
        {
            QTimer::singleShot(0,
                               messages_view_,
                               [this, user, position]
                               {
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
    if (message.deleted)
    {
        if (message.conversation == active_conversation_ && unread_pending_)
        {
            withdrawn_before_unread_.insert(message.id);
        }
        invalidate_unread_start(message.conversation, message.id);
        discard_draft_reply(message.conversation, message.id, true);
    }
    if (!visible_message(message))
    {
        return;
    }
    messages_->update_message(message);
    if (message.conversation != active_conversation_)
    {
        return;
    }
    if (reply_to_ == message.id)
    {
        show_reply({message.id, message.username, grapheme_prefix(message.text, 160), message.edited_at, message.deleted, message.from});
    }
}

bool chat_widget::set_reactions(qint64 conversation, qint64 message, qint64 revision, QList<reaction_data> reactions)
{
    return conversation != active_conversation_ || messages_->set_reactions(message, revision, std::move(reactions));
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
    set_message_error(conversation, QStringLiteral("%1。可在附件上传菜单继续或取消。").arg(error));
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
        messages_loading_ = false;
        jump_to_latest_ = false;
        update_reading_navigation();
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
    contacts_title_->setText(QStringLiteral("联系人"));
    contacts_back_button_->hide();
    if (contacts_dialog_->isVisible())
    {
        contact_search_->setFocus();
    }
}

void chat_widget::show_friend_requests()
{
    if (incoming_requests_.isEmpty() && outgoing_requests_.isEmpty())
    {
        return;
    }
    contacts_pages_->setCurrentIndex(1);
    contacts_title_->setText(QStringLiteral("好友申请"));
    contacts_back_button_->show();
    contacts_dialog_->resize(420, 480);
    for (auto* list : {incoming_friends_, outgoing_friends_})
    {
        if (list->count() > 0 && !list->currentItem())
        {
            list->setCurrentRow(0);
        }
    }
    contacts_back_button_->setFocus();
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
    contacts_dialog_->resize(
        420, std::clamp(120 + contacts_->rowCount() * chat_theme::dialog_row_height + (contacts_requests_button_->isHidden() ? 0 : 64), 240, 480));
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
    contacts_filter_->setFilterFixedString(query);
    if (contacts_->rowCount() > 0)
    {
        contacts_status_->setText(contacts_filter_->rowCount() == 0 ? QStringLiteral("没有匹配的联系人") : QString{});
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
    if (!contacts_dialog_->isVisible() || !connection_available_)
    {
        return;
    }
    auto const* item = contacts_->user_at(contacts_filter_->mapToSource(index));
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
    contacts_back_button_->setIcon(svg_icon(u"back", themed("text_secondary"), QSize(20, 20)));
    contacts_requests_icon_->setPixmap(svg_icon(u"user-plus", themed("text_secondary"), QSize(22, 22)).pixmap(22, 22));
    contacts_requests_arrow_->setPixmap(svg_icon(u"chevron-right", themed("text_secondary"), QSize(16, 16)).pixmap(16, 16));
    cancel_reply_button_->setIcon(svg_icon(QStringLiteral("close"), themed("text"), QSize(20, 20)));
    send_button_->setIcon(svg_icon(QStringLiteral("send"), themed("on_accent"), QSize(20, 20)));
    chat_more_button_->setIcon(svg_icon(u"more", themed("text"), QSize(20, 20)));
    attachment_button_->setIcon(svg_icon(u"attach", themed("text"), QSize(22, 22)));
    screenshot_button_->setIcon(svg_icon(u"screenshot", themed("text"), QSize(22, 22)));
    emoji_button_->setIcon(svg_icon(u"emoji", themed("text"), QSize(22, 22)));
    history_button_->setIcon(svg_icon(u"history", themed("text"), QSize(22, 22)));
    for (auto* action : main_menu_button_->menu()->actions())
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
    auto name = QInputDialog::getText(this, QStringLiteral("会话分组"), QStringLiteral("分组名称（最多 32 个字符）"), QLineEdit::Normal, current, &ok)
                    .trimmed();
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
    menu.addAction(QStringLiteral("全部标为已读"), this, [this, id] { emit mark_read_scope(0, id); });
    menu.addAction(QStringLiteral("新建分组"), this, [this] { edit_folder_name(0); });
    if (id)
    {
        menu.addAction(QStringLiteral("重命名"), this, [this, id, name] { edit_folder_name(id, name); });
        menu.addAction(QStringLiteral("上移"), this, [this, id] { emit reorder_conversation_folder(id, true); })
            ->setEnabled(conversations_->folders().front().first != id);
        menu.addAction(QStringLiteral("下移"), this, [this, id] { emit reorder_conversation_folder(id, false); })
            ->setEnabled(conversations_->folders().back().first != id);
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
        conversations_view_->setRowHidden(row, !matched);
        shown += matched;
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
    for (int row = 0; row < conversations_->rowCount(); ++row)
    {
        auto const index = conversations_->index(row, 0);
        auto const* item = conversations_->conversation_at(index);
        if (!item)
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
    auto const incoming = incoming_requests_.size();
    chats_action_->setText(total > 0 ? QStringLiteral("聊天 · %1 条未读").arg(total) : QStringLiteral("聊天"));
    contacts_action_->setText(incoming > 0 ? QStringLiteral("联系人 · %1 个好友申请").arg(incoming) : QStringLiteral("联系人"));
    chats_action_->setChecked(true);
    auto const count = incoming > 0 ? quint64(incoming) : total;
    main_menu_badge_->setText(count > 99 ? QStringLiteral("99+") : QString::number(count));
    main_menu_badge_->setVisible(count > 0);
    auto const width = std::max(16, main_menu_badge_->sizeHint().width());
    main_menu_badge_->setGeometry(main_menu_button_->width() - width, 0, width, 16);
    auto const description = QStringLiteral("主菜单 · %1 条未读消息 · %2 个好友申请").arg(total).arg(incoming);
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

bool chat_widget::can_queue_text() const
{
    auto const item = conversation(active_conversation_);
    return item && !item->speech_blocked();
}

bool chat_widget::can_send() const { return connection_available_ && can_queue_text(); }

void chat_widget::update_compose_state()
{
    input_bar_->setVisible(active_conversation_ > 0);
    auto const item = conversation(active_conversation_);
    auto const blocked = item && item->speech_blocked();
    auto const allowed = connection_available_ && item && !blocked;
    message_edit_->setEnabled(item.has_value());
    speech_expiry_->stop();
    if (blocked)
    {
        speech_notice_->setText(item->group_muted ? QStringLiteral("群聊已开启全员禁言")
                                : item->speech_until < 0
                                    ? QStringLiteral("你已被永久禁言")
                                    : QStringLiteral("你已被禁言至 %1")
                                          .arg(QDateTime::fromMSecsSinceEpoch(item->speech_until).toString(QStringLiteral("MM-dd HH:mm:ss"))));
        if (!item->group_muted && item->speech_until > 0)
        {
            auto const delay = std::clamp<qint64>(item->speech_until - item->server_now(), 1, std::numeric_limits<int>::max());
            speech_expiry_->start(static_cast<int>(delay));
        }
    }
    speech_notice_->setVisible(blocked);
    send_button_->setEnabled(item && !blocked && !saving_text_ && !message_edit_->toPlainText().isEmpty() && (!reply_to_ || !reply_bar_->isHidden()));
    attachment_button_->setEnabled(allowed);
    screenshot_button_->setEnabled(allowed);
    mention_button_->setVisible(active_group_);
    mention_button_->setEnabled(allowed);
    emoji_button_->setEnabled(allowed);
    message_edit_->setPlaceholderText(QStringLiteral("输入消息…"));
    if (!allowed)
    {
        typing_idle_timer_->stop();
    }
}

void chat_widget::open_conversation(conversation_data conversation)
{
    auto const user = conversation.id;
    if (user <= 0)
    {
        return;
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
    active_member_count_ = conversation.member_count;

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
            messages_loading_ = true;
            show_loading_status();
            if (connection_available_)
            {
                emit conversation_selected(active_conversation_, active_group_);
            }
        }
        return;
    }

    flush_draft();
    stop_typing();
    typing_users_.clear();
    update_typing_label();
    images_.discard_queued();
    active_conversation_ = user;
    ++conversation_visit_;
    unread_start_ = 0;
    unread_pending_ = true;
    withdrawn_before_unread_.clear();
    timeline_current_ = jump_to_latest_ = false;
    update_reading_navigation();
    update_chat_header(active_username_);
    apply_draft();
    message_search_action_->setEnabled(connection_available_);
    history_button_->setEnabled(connection_available_);
    chat_more_button_->setEnabled(true);
    set_message_selection(false);
    messages_->reset(active_conversation_, active_group_);
    load_draft_reply();
    show_outgoing();
    update_pinned_message();
    messages_loaded_ = false;
    messages_loading_ = true;
    history_exhausted_ = false;
    show_loading_status();
    update_compose_state();
    if (connection_available_)
    {
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
    if (unread_start_ > 0 && unread_start_ <= floor)
    {
        invalidate_unread_start(conversation, unread_start_);
    }
    messages_->discard_through(floor);
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
    bool const lost_start = conversation == active_conversation_ && unread_start_ > 0;
    leave_conversation(conversation);
    if (lost_start)
    {
        set_message_status(QStringLiteral("本次未读起点已不可用"));
    }
}

void chat_widget::leave_conversation(qint64 conversation)
{
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
    flush_draft();
    stop_typing();
    typing_users_.clear();
    update_typing_label();
    active_conversation_ = 0;
    ++conversation_visit_;
    unread_start_ = 0;
    unread_pending_ = timeline_current_ = jump_to_latest_ = false;
    withdrawn_before_unread_.clear();
    update_reading_navigation();
    update_pinned_message();
    active_peer_ = 0;
    active_group_ = false;
    active_member_count_ = 0;
    active_username_.clear();
    messages_loaded_ = false;
    messages_loading_ = false;
    history_exhausted_ = false;
    reset_reply();
    set_message_selection(false);
    messages_->reset(0);
    images_.remove(conversation);
    conversations_view_->setCurrentIndex({});
    conversations_view_->clearSelection();
    chat_title_->setText(QStringLiteral("聊天"));
    chat_title_->setToolTip({});
    chat_title_->setIcon({});
    chat_title_->setEnabled(false);
    chat_presence_->clear();
    chat_presence_->hide();
    message_edit_->clear();
    message_edit_->setEnabled(false);
    send_button_->setEnabled(false);
    message_search_action_->setEnabled(false);
    history_button_->setEnabled(false);
    attachment_button_->setEnabled(false);
    chat_more_button_->setEnabled(false);
    set_message_status(conversations_->rowCount() == 0 ? QStringLiteral("暂无会话，可从主菜单添加联系人或新建群组")
                                                       : QStringLiteral("选择一个会话开始聊天"));
    update_compose_state();
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
    if (active_group_)
    {
        active_member_count_ = members.size();
        update_chat_presence();
    }
    update_pinned_message();
    for (auto it = typing_users_.begin(); it != typing_users_.end();)
    {
        auto const present = std::any_of(members.begin(), members.end(), [id = it.key()](auto const& member) { return member.id == id; });
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
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
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
    auto refresh = [this, &dialog, conversation, message, count, list]
    {
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
    connect(messages_, &QAbstractItemModel::dataChanged, &dialog, refresh);
    connect(messages_, &QAbstractItemModel::modelReset, &dialog, refresh);
    connect(messages_, &QAbstractItemModel::rowsRemoved, &dialog, refresh);
    connect(&avatars_, &avatar_cache::changed, &dialog, refresh);
    if (!refresh())
    {
        return;
    }
    dialog.exec();
}

std::optional<qint64> chat_widget::recovery_cursor() const
{
    return messages_loaded_ ? std::optional<qint64>(std::max<qint64>(0, messages_->first_message_id() - 1)) : std::nullopt;
}

bool chat_widget::messages_ready() const { return messages_loaded_ && !messages_loading_; }

bool chat_widget::viewing_latest() const
{
    auto const* scroll = messages_view_->verticalScrollBar();
    return connection_available_ && timeline_current_ && messages_ready() && messages_view_->isVisible() && window()->isActiveWindow() &&
           !window()->isMinimized() && !QApplication::activeModalWidget() && !QApplication::activePopupWidget() &&
           scroll->value() == scroll->maximum();
}

void chat_widget::begin_history_refresh()
{
    timeline_current_ = false;
    messages_loading_ = true;
    update_reading_navigation();
}

void chat_widget::update_reading_navigation()
{
    unread_start_button_->setVisible(unread_start_ > 0);
    unread_start_button_->setEnabled(connection_available_);
    auto const* scroll = messages_view_->verticalScrollBar();
    latest_button_->setVisible(active_conversation_ > 0 && (messages_loaded_ || !messages_loading_) &&
                               (!timeline_current_ || scroll->value() != scroll->maximum()));
    latest_button_->setEnabled(connection_available_);
}

void chat_widget::invalidate_unread_start(qint64 conversation, qint64 anchor)
{
    if (conversation == active_conversation_ && anchor > 0 && anchor == unread_start_)
    {
        unread_start_ = 0;
        update_reading_navigation();
        set_message_status(QStringLiteral("本次未读起点已不可用"));
    }
}

void chat_widget::return_to_latest()
{
    if (timeline_current_)
    {
        messages_view_->scrollToBottom();
        mark_visible_messages();
    }
    else
    {
        jump_to_latest_ = true;
        if (!messages_loading_)
        {
            begin_history_refresh();
            emit latest_messages_requested(active_conversation_);
        }
    }
}

void chat_widget::resume_reading_after_menu(QMenu& menu)
{
    connect(&menu, &QMenu::aboutToHide, this, [this] { QTimer::singleShot(0, this, &chat_widget::mark_visible_messages); });
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
    if (!connection_available_ || active_conversation_ <= 0 || !messages_loaded_ || !timeline_current_ || messages_loading_ || history_exhausted_)
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
    auto text = message_edit_->toPlainText();
    if (!can_queue_text() || saving_text_ || text.isEmpty() || (reply_to_ && reply_bar_->isHidden()))
    {
        return;
    }
    flush_draft();
    auto const id = chat::client::new_client_id();
    auto const revision = drafts_.value(active_conversation_).revision;
    saving_text_ = text_save{id, active_conversation_, revision};
    update_compose_state();
    emit send_message_requested(active_conversation_, std::move(text), reply_to_, id, revision, drafts_.value(active_conversation_).options);
}

void chat_widget::draft_edited()
{
    if (active_conversation_ > 0 && self_user_ > 0)
    {
        drafts_.insert(active_conversation_,
                       {message_edit_->toPlainText(), reply_to_, chat::client::new_client_id(), {markdown_button_->isChecked()}});
        draft_timer_->start();
    }
}

void chat_widget::flush_draft()
{
    if (!draft_timer_->isActive())
    {
        return;
    }
    draft_timer_->stop();
    save_active_draft();
}

void chat_widget::save_active_draft()
{
    auto const value = drafts_.value(active_conversation_);
    if (value.revision && active_conversation_ > 0 && self_user_ > 0)
    {
        emit draft_save_requested(self_user_, active_conversation_, value.revision, value.reply, value.text, value.options);
    }
}

void chat_widget::apply_draft()
{
    auto const value = drafts_.value(active_conversation_);
    QSignalBlocker format_blocker(markdown_button_);
    markdown_button_->setChecked(value.options.markdown);
    reset_reply(value.reply);
    QSignalBlocker const blocker(message_edit_);
    message_edit_->setPlainText(value.text);
    message_edit_->moveCursor(QTextCursor::End);
}

void chat_widget::restore_drafts(qint64 user, std::vector<chat::compose_draft> const& values)
{
    if (user != self_user_)
    {
        return;
    }
    for (auto const& value : values)
    {
        if (!drafts_.contains(value.conversation))
        {
            drafts_.insert(value.conversation, {QString::fromStdString(value.text), value.reply, value.revision, value.options});
            if (value.conversation == active_conversation_)
            {
                apply_draft();
                load_draft_reply();
                update_compose_state();
            }
        }
    }
}

void chat_widget::load_draft_reply()
{
    if (!reply_to_ || active_conversation_ <= 0 || !reply_bar_->isHidden())
    {
        return;
    }
    if (reply_to_ <= history_floor(active_conversation_))
    {
        discard_draft_reply(active_conversation_, reply_to_);
    }
    else if (connection_available_)
    {
        emit draft_reply_requested(active_conversation_, reply_to_, ++draft_reply_request_);
    }
}

void chat_widget::validate_draft_reply(qint64 conversation, qint64 anchor, qint64 revision, message_data const& message, bool unavailable)
{
    if (conversation != active_conversation_ || reply_to_ != anchor || draft_reply_request_ != revision)
    {
        return;
    }
    if (unavailable || anchor <= history_floor(conversation))
    {
        discard_draft_reply(conversation, anchor, true);
        return;
    }
    show_reply({message.id, message.username, grapheme_prefix(message.text, 160), message.edited_at, message.deleted, message.from});
}

void chat_widget::reset_reply(qint64 reply)
{
    ++draft_reply_request_;
    reply_to_ = reply;
    reply_snapshot_ = {};
    reply_bar_->hide();
}

void chat_widget::show_reply(quoted_message_data reply)
{
    if (!reply.id || reply.id != reply_to_ || reply.deleted)
    {
        return;
    }
    auto const existing = quote(reply.id);
    if (existing.deleted)
    {
        discard_draft_reply(active_conversation_, reply.id, true);
        return;
    }
    if (existing.edited_at > reply.edited_at)
    {
        reply = existing;
    }
    if (reply_snapshot_.id == reply.id && reply_snapshot_.edited_at >= reply.edited_at)
    {
        return;
    }
    reply_snapshot_ = std::move(reply);
    reply_preview_->setText(QStringLiteral("回复 %1：%2").arg(reply_snapshot_.username, grapheme_prefix(reply_snapshot_.text, 80)));
    reply_bar_->show();
    update_compose_state();
}

void chat_widget::discard_draft_reply(qint64 conversation, qint64 through, bool exact)
{
    auto value = drafts_.find(conversation);
    if (value == drafts_.end() || !value->reply || (exact ? value->reply != through : value->reply > through))
    {
        return;
    }
    value->reply = 0;
    value->revision = chat::client::new_client_id();
    if (conversation == active_conversation_)
    {
        reset_reply();
        draft_timer_->stop();
        update_compose_state();
    }
    emit draft_save_requested(self_user_, conversation, value->revision, 0, value->text, value->options);
}

void chat_widget::finish_text_save(qint64 client_id, QString error)
{
    if (!saving_text_ || saving_text_->client_id != client_id)
    {
        return;
    }
    auto const saved = *saving_text_;
    saving_text_.reset();
    if (!error.isEmpty())
    {
        set_error(std::move(error));
    }
    else if (drafts_.value(saved.draft_conversation).revision == saved.revision)
    {
        drafts_.remove(saved.draft_conversation);
        if (saved.draft_conversation == active_conversation_)
        {
            stop_typing();
            apply_draft();
        }
    }
    update_compose_state();
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
        auto const* scroll = messages_view_->verticalScrollBar();
        bool const follow = added && scroll->value() == scroll->maximum();
        show_outgoing();
        if (follow)
        {
            QTimer::singleShot(0, messages_view_, [this] { messages_view_->scrollToBottom(); });
        }
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
        submit(outgoing_.back());
        return;
    }
    stop_typing();
    // Like a new message: followed at the bottom, while someone reading history keeps their place.
    auto* scroll = messages_view_->verticalScrollBar();
    auto const at_bottom = scroll->value() == scroll->maximum();
    show_outgoing();
    update_compose_state();
    if (at_bottom)
    {
        QTimer::singleShot(0, messages_view_, [this] { messages_view_->scrollToBottom(); });
    }
    submit(outgoing_.back());
}

void chat_widget::submit(outgoing const& item)
{
    emit attachment_send_requested(item.conversation, item.path, item.reply.id, item.client_id, item.group_only);
}

void chat_widget::show_outgoing()
{
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
    messages_->set_pending(std::move(pending));
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
    for (int row = 0; row < messages_->rowCount(); ++row)
    {
        auto const index = messages_->index(row, 0);
        if (index.data(message_model::id_role).toLongLong() != message)
        {
            continue;
        }
        auto const revision = index.data(message_model::edited_at_role).toLongLong();
        auto const deleted = index.data(message_model::deleted_role).toBool();
        if (!deleted && reply_snapshot_.id == message && reply_snapshot_.edited_at >= revision)
        {
            return reply_snapshot_;
        }
        return {message,
                index.data(message_model::sender_name_role).toString(),
                grapheme_prefix(index.data(message_model::text_role).toString(), 160),
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
    auto const text = found->filename.isEmpty() ? found->text : found->filename;
    auto const has_reply = found->reply.id > 0;
    QMenu menu(this);
    resume_reading_after_menu(menu);
    auto* copy = menu.addAction(QStringLiteral("复制"));
    // Sending again needs the connection; discarding is local and works offline too.
    bool const attachment = !found->path.isEmpty();
    auto* pause = attachment && !failed ? menu.addAction(QStringLiteral("暂停上传")) : nullptr;
    auto* retry = failed && can_send() ? menu.addAction(attachment ? QStringLiteral("继续上传") : QStringLiteral("重新发送")) : nullptr;
    auto const* retry_plain = attachment && failed && can_send() && has_reply ? menu.addAction(QStringLiteral("不带回复重新发送")) : nullptr;
    auto* discard = attachment ? (connection_available_ ? menu.addAction(QStringLiteral("取消上传")) : nullptr)
                               : (failed || found->waiting ? menu.addAction(QStringLiteral("删除本地待发送记录")) : nullptr);
    auto* selected = menu.exec(messages_view_->viewport()->mapToGlobal(position));
    if (selected == copy)
    {
        QGuiApplication::clipboard()->setText(text);
        return;
    }
    // Act on the send as it is now: it may have been confirmed or dropped while the menu was open.
    auto item = std::ranges::find(outgoing_, client_id, &outgoing::client_id);
    if (!selected || item == outgoing_.end())
    {
        return;
    }
    if (attachment && (selected == pause || selected == discard))
    {
        emit attachment_interrupt_requested(client_id, selected == discard, item->path);
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

void chat_widget::stop_typing()
{
    if (typing_idle_timer_->isActive())
    {
        typing_idle_timer_->stop();
        if (can_send())
        {
            emit typing_requested(active_conversation_, false);
        }
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
    typing_label_->setText(names.isEmpty() ? QString{}
                                           : names.mid(0, 3).join(QStringLiteral("、")) +
                                                 (names.size() > 3 ? QStringLiteral("等人正在输入…") : QStringLiteral("正在输入…")));
    typing_label_->setVisible(!names.isEmpty());
}

void chat_widget::set_control_enter_send(bool enabled)
{
    control_enter_send_ = enabled;
    message_edit_->setAccessibleDescription(enabled ? QStringLiteral("Ctrl+Enter 发送；Enter 或 Shift+Enter 换行")
                                                    : QStringLiteral("Enter 发送；Shift+Enter 换行"));
    send_button_->setToolTip(enabled ? QStringLiteral("发送（Ctrl+Enter）") : QStringLiteral("发送（Enter）"));
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
    section_separator->setFixedHeight(10);
    layout->addWidget(section_separator);

    auto* info = new QFrame(&dialog);
    info->setObjectName(QStringLiteral("profileInfoSection"));
    auto* info_layout = new QVBoxLayout(info);
    info_layout->setContentsMargins(24, 16, 24, 24);
    info_layout->setSpacing(12);

    auto* account = new QLabel(header);
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
        auto* remove_button = new QPushButton(QStringLiteral("移除联系人"), info);
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
                                        QStringLiteral("移除联系人"),
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
    message_status_->setVisible(!message_status_->text().isEmpty());
}

void chat_widget::update_chat_header(QString const& username)
{
    auto const item = conversation(active_conversation_);
    chat_title_->setText(item ? item->display_name() : username);
    chat_title_->setToolTip(chat_title_->text());
    chat_title_->setIcon(avatar_icon(username, 38, avatars_.image(active_group_ ? -active_conversation_ : active_peer_)));
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
        chat_presence_->setText(QStringLiteral("%1 名成员").arg(active_member_count_));
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

    chat_presence_->setObjectName(presence.online ? QStringLiteral("chatPresenceOnline") : QStringLiteral("chatPresence"));
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
