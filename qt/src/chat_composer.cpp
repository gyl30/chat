#include "chat_widget.hpp"
#include "emoji_segments.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <QAction>
#include <QDateTime>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <chat/client.hpp>

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
    create_poll_action_->setVisible(active_group_ && allowed);
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
    message_edit_->setToolTip(message_edit_->accessibleDescription());
    send_button_->setToolTip(enabled ? QStringLiteral("发送（Ctrl+Enter）") : QStringLiteral("发送（Enter）"));
}
