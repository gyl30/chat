#include "chat_widget.hpp"
#include "emoji_segments.hpp"
#include "icons.hpp"
#include "presence.hpp"
#include "theme.hpp"
#include "theme_manager.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <QAction>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QDateTime>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <chat/client.hpp>

class attachment_menu final : public QMenu
{
   public:
    attachment_menu(QToolButton& button, std::function<bool()> available) : QMenu(&button), button_(button), available_(std::move(available))
    {
        setObjectName(QStringLiteral("attachmentMenu"));
        setAccessibleName(QStringLiteral("附件"));
        setMouseTracking(true);
        button_.installEventFilter(this);
        open_delay_.setSingleShot(true);
        open_delay_.setInterval(200);
        close_delay_.setSingleShot(true);
        close_delay_.setInterval(150);
        connect(&open_delay_, &QTimer::timeout, this,
                [this]
                {
                    if (button_.isActiveWindow() && anchor().contains(QCursor::pos()))
                    {
                        show_at_button();
                    }
                });
        connect(&close_delay_, &QTimer::timeout, this,
                [this]
                {
                    if (!contains_pointer())
                    {
                        hide();
                    }
                });
        connect(this, &QMenu::aboutToHide, this,
                [this]
                {
                    close_delay_.stop();
                    hover_blocked_ = anchor().contains(QCursor::pos());
                });
    }

    void show_at_button()
    {
        open_delay_.stop();
        if (isVisible() || !available_() || !button_.isVisible())
        {
            return;
        }
        auto const area = button_.screen()->availableGeometry();
        auto const rect = anchor();
        auto const size = sizeHint();
        auto const x = std::clamp(rect.left(), area.left(), std::max(area.left(), area.right() + 1 - size.width()));
        auto const top = rect.top() - size.height();
        auto const bottom = rect.bottom() + 1;
        auto const y = top >= area.top() ? top : std::min(bottom, std::max(area.top(), area.bottom() + 1 - size.height()));
        popup(QPoint(x, y));
    }

    void cancel()
    {
        open_delay_.stop();
        close_delay_.stop();
        hide();
    }

   private:
    QRect anchor() const { return QRect(button_.mapToGlobal(QPoint{}), button_.size()); }

    bool contains_pointer() const
    {
        auto const point = QCursor::pos();
        return anchor().contains(point) || frameGeometry().contains(point);
    }

    void track_pointer()
    {
        if (!anchor().contains(QCursor::pos()))
        {
            hover_blocked_ = false;
            open_delay_.stop();
        }
        if (!isVisible() || contains_pointer())
        {
            close_delay_.stop();
        }
        else if (!close_delay_.isActive())
        {
            close_delay_.start();
        }
    }

    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object != &button_)
        {
            return QMenu::eventFilter(object, event);
        }
        switch (event->type())
        {
            case QEvent::Enter:
                if (!hover_blocked_ && !isVisible() && available_())
                {
                    open_delay_.start();
                }
                [[fallthrough]];
            case QEvent::Leave:
                track_pointer();
                break;
            case QEvent::Hide:
            case QEvent::EnabledChange:
                if (!button_.isVisible() || !button_.isEnabled())
                {
                    cancel();
                }
                break;
            case QEvent::KeyPress:
            {
                auto const key = static_cast<QKeyEvent*>(event)->key();
                if ((key == Qt::Key_Return || key == Qt::Key_Enter) && button_.isEnabled())
                {
                    button_.click();
                    return true;
                }
                break;
            }
            default:
                break;
        }
        return false;
    }

    bool event(QEvent* event) override
    {
        switch (event->type())
        {
            case QEvent::Enter:
            case QEvent::Leave:
            case QEvent::MouseMove:
                track_pointer();
                break;
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
                if (anchor().contains(static_cast<QMouseEvent*>(event)->globalPosition().toPoint()))
                {
                    return true;
                }
                break;
            default:
                break;
        }
        return QMenu::event(event);
    }

    QToolButton& button_;
    std::function<bool()> available_;
    QTimer open_delay_;
    QTimer close_delay_;
    bool hover_blocked_ = false;
};

void chat_widget::setup_attachment_menu()
{
    attachment_menu_ = new attachment_menu(*attachment_button_, [this] { return active_group_ && can_send(); });
    style_menu_icons(*attachment_menu_, chat_theme::menu_icon_size);
    auto* file = attachment_menu_->addAction(QStringLiteral("文件"), this,
                                             [this]
                                             {
                                                 if (can_send())
                                                 {
                                                     choose_attachment(active_conversation_);
                                                 }
                                             });
    auto* poll = attachment_menu_->addAction(QStringLiteral("投票"), this,
                                             [this]
                                             {
                                                 if (active_group_ && can_send())
                                                 {
                                                     emit create_poll_requested(active_conversation_);
                                                 }
                                             });
    poll->setObjectName(QStringLiteral("createPollAction"));
    auto update_icons = [file, poll]
    {
        file->setIcon(svg_icon(u"attach", themed("text_secondary"), {chat_theme::menu_icon_size, chat_theme::menu_icon_size}));
        poll->setIcon(svg_icon(u"poll", themed("text_secondary"), {chat_theme::menu_icon_size, chat_theme::menu_icon_size}));
    };
    update_icons();
    connect(&theme_manager::instance(), &theme_manager::changed, attachment_menu_, update_icons);
    resume_reading_after_menu(*attachment_menu_);
    connect(attachment_button_, &QToolButton::clicked, this,
            [this]
            {
                if (active_group_)
                {
                    attachment_menu_->show_at_button();
                }
                else if (can_send())
                {
                    choose_attachment(active_conversation_);
                }
            });
}

void chat_widget::close_attachment_menu() { attachment_menu_->cancel(); }

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
    attachment_button_->setToolTip(active_group_ ? QStringLiteral("附件") : QStringLiteral("选择文件"));
    if (!active_group_ || !allowed)
    {
        close_attachment_menu();
    }
    emit poll_creation_availability(active_conversation_, active_group_ && allowed);
    mention_button_->setVisible(active_group_);
    mention_button_->setEnabled(allowed);
    emoji_button_->setEnabled(allowed);
    message_edit_->setPlaceholderText(QStringLiteral("输入消息…"));
    if (!allowed)
    {
        typing_idle_timer_->stop();
    }
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
                       {message_edit_->toPlainText(),
                        reply_to_,
                        chat::client::new_client_id(),
                        {markdown_button_->isChecked(), mention_all_button_->isChecked()}});
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
    QSignalBlocker all_blocker(mention_all_button_);
    mention_all_button_->setChecked(value.options.mention_all);
    mention_all_button_->setVisible(value.options.mention_all);
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
    else
    {
        if (active_conversation_ == saved.draft_conversation)
        {
            return_to_latest();
        }
        if (drafts_.value(saved.draft_conversation).revision != saved.revision)
        {
            update_compose_state();
            return;
        }
        drafts_.remove(saved.draft_conversation);
        if (saved.draft_conversation == active_conversation_)
        {
            stop_typing();
            apply_draft();
        }
    }
    update_compose_state();
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
    update_chat_status();
}

void chat_widget::update_chat_status()
{
    if (!active_conversation_)
    {
        return;
    }
    QStringList names;
    for (auto const& user : typing_users_)
    {
        names.push_back(user.username);
    }
    QString text;
    if (!names.isEmpty())
    {
        names.sort();
        text = !active_group_ ? QStringLiteral("正在输入…")
                              : names.mid(0, 3).join(QStringLiteral("、")) +
                                    (names.size() > 3 ? QStringLiteral("等人正在输入…") : QStringLiteral("正在输入…"));
    }
    else if (active_group_)
    {
        if (const auto room = conversation(active_conversation_))
        {
            text = QStringLiteral("%1 位成员").arg(room->member_count);
        }
    }
    else if (const auto peer = presence_.constFind(active_peer_); peer != presence_.cend())
    {
        text = presence_text(peer->online, peer->last_seen);
    }
    chat_status_->setText(text);
    chat_status_->setVisible(!text.isEmpty());
}

void chat_widget::set_control_enter_send(bool enabled)
{
    control_enter_send_ = enabled;
    message_edit_->setAccessibleDescription(enabled ? QStringLiteral("Ctrl+Enter 发送；Enter 或 Shift+Enter 换行")
                                                    : QStringLiteral("Enter 发送；Shift+Enter 换行"));
    message_edit_->setToolTip(message_edit_->accessibleDescription());
    send_button_->setToolTip(enabled ? QStringLiteral("发送（Ctrl+Enter）") : QStringLiteral("发送（Enter）"));
}
