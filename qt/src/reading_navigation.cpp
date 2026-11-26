#include "chat_widget.hpp"
#include "conversation_model.hpp"
#include "message_model.hpp"
#include <QApplication>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QTimer>

chat_widget::reading_position chat_widget::capture_reading_position() const
{
    auto* bar = messages_view_->verticalScrollBar();
    if (tail_loaded_ && bar->value() == bar->maximum())
    {
        return {};
    }
    auto top = messages_view_->indexAt({1, 0});
    const auto id = top.data(message_model::id_role).toLongLong();
    return {id, messages_view_->visualRect(top).top(), id ? 0 : top.data(message_model::client_id_role).toLongLong()};
}

void chat_widget::restore_reading_position(reading_position position)
{
    if (!position.message && !position.client_id)
    {
        if (tail_loaded_)
        {
            messages_view_->scrollToBottom();
        }
        return;
    }
    auto top = position.client_id ? messages_->outgoing_index(position.client_id) : messages_->message_index(position.message);
    if (top.isValid())
    {
        auto* scroll = messages_view_->verticalScrollBar();
        if (position.offset)
        {
            scroll->setValue(scroll->value() + messages_view_->visualRect(top).top() - *position.offset);
        }
        else
        {
            messages_view_->setCurrentIndex(top);
            messages_view_->scrollTo(top, QAbstractItemView::PositionAtCenter);
        }
    }
}

void chat_widget::preserve_reading(const std::function<void()>& change)
{
    if (changing_layout_ || !messages_loaded_)
    {
        change();
        return;
    }
    {
        QScopedValueRollback guard(changing_layout_, true);
        auto position = capture_reading_position();
        const auto top = messages_view_->indexAt({1, 0});
        const QPersistentModelIndex next = position.client_id ? top.siblingAtRow(top.row() + 1) : QModelIndex{};
        change();
        messages_view_->doItemsLayout();
        if (position.client_id && next.isValid() && !messages_->outgoing_index(position.client_id).isValid())
        {
            position.client_id = next.data(message_model::client_id_role).toLongLong();
        }
        restore_reading_position(position);
    }
    mark_visible_messages();
    update_reading_navigation();
}

bool chat_widget::messages_ready() const { return messages_loaded_ && !messages_loading_; }

bool chat_widget::viewing_latest() const
{
    auto const* scroll = messages_view_->verticalScrollBar();
    return reading_enabled() && tail_loaded_ && scroll->value() == scroll->maximum();
}

bool chat_widget::reading_enabled() const
{
    return !poll_page_ && connection_available_ && messages_ready() && messages_view_->isVisible() && window()->isActiveWindow() &&
           !window()->isMinimized() && !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
}

void chat_widget::set_messages_loading(bool loading)
{
    messages_loading_ = loading;
    if (loading)
    {
        messages_->begin_page();
    }
    else
    {
        messages_->end_page();
    }
}

void chat_widget::request_navigation(history_target target)
{
    history_target_ = target;
    if (connection_available_)
    {
        set_messages_loading(true);
        update_reading_navigation();
        emit history_requested(active_conversation_, target.page, target.position.message);
    }
}

void chat_widget::request_position(reading_position position) { request_navigation({history_page::around, position}); }

void chat_widget::request_entry_history()
{
    auto position = reading_positions_.take(active_conversation_);
    if (position.message > history_floor(active_conversation_))
    {
        request_position(position);
        return;
    }
    request_navigation({history_page::unread});
}

void chat_widget::begin_history_refresh()
{
    const auto* scroll = messages_view_->verticalScrollBar();
    const bool at_end = messages_loaded_ && tail_loaded_ && scroll->value() == scroll->maximum();
    // The old tail cannot prove continuity across a lost connection or a refresh.
    tail_loaded_ = false;
    if (history_target_)
    {
        if (history_target_->page == history_page::around && history_target_->position.message <= history_floor(active_conversation_))
        {
            request_entry_history();
        }
        else
        {
            request_navigation(*history_target_);
        }
        return;
    }
    if (!messages_loaded_)
    {
        request_entry_history();
        return;
    }
    if (at_end)
    {
        request_navigation({history_page::latest});
        return;
    }
    set_messages_loading(true);
    update_reading_navigation();
    emit history_requested(active_conversation_, history_page::refresh, std::max<qint64>(0, messages_->first_message_id() - 1));
}

void chat_widget::update_reading_navigation()
{
    if (!history_down_)
    {
        return;
    }
    auto* viewport = messages_view_->viewport();
    auto const* scroll = messages_view_->verticalScrollBar();
    const auto unread = conversation_unread();
    const bool returning = !reply_returns_.isEmpty();
    const auto gap = scroll->maximum() - scroll->value();
    const bool away = returning || !tail_loaded_ || gap > (unread ? 0 : viewport->height() / 2);
    history_down_->setText(!returning && unread ? QStringLiteral("↓  %1").arg(unread) : QStringLiteral("↓"));
    history_down_->setToolTip(returning ? QStringLiteral("返回上次阅读位置")
                              : unread  ? QStringLiteral("前往未读消息")
                                        : QStringLiteral("返回最新消息"));
    history_down_->setAccessibleName(history_down_->toolTip());
    history_down_->adjustSize();
    history_down_->move(viewport->width() - history_down_->width() - 14, viewport->height() - history_down_->height() - 12);
    history_down_->setVisible(active_conversation_ > 0 && messages_loaded_ && away);
    history_down_->setEnabled(connection_available_ && !messages_loading_);
}

quint64 chat_widget::conversation_unread() const
{
    auto const& rooms = conversations_->items();
    auto room = std::ranges::find(rooms, active_conversation_, &conversation_data::id);
    return room != rooms.end() ? room->unread : 0;
}

qint64 chat_widget::effective_read_position() const { return std::max(requested_read_, messages_->read_position(self_user_)); }

void chat_widget::scroll_to_unread(QModelIndex const& index)
{
    auto* scroll = messages_view_->verticalScrollBar();
    // Measure against the bottom-aligned timeline before reserving space for a divider.
    if (tail_loaded_ && messages_view_->visualRect(index).top() >= scroll->maximum() - scroll->value())
    {
        messages_->set_unread_boundary(0);
        messages_view_->scrollToBottom();
        return;
    }
    messages_->set_unread_boundary(index.data(message_model::id_role).toLongLong());
    messages_view_->doItemsLayout();
    scroll->setValue(scroll->value() + messages_view_->visualRect(index).top() - std::min(64, messages_view_->viewport()->height() / 5));
}

void chat_widget::jump_to_message(qint64 id)
{
    if (id <= history_floor(active_conversation_))
    {
        return;
    }
    const auto target = messages_->message_index(id);
    if (target.data(message_model::deleted_role).toBool())
    {
        return;
    }
    if (!target.isValid() && !connection_available_)
    {
        set_message_status(QStringLiteral("重新连接后可以查看这条消息"));
        return;
    }
    reply_returns_.push_back(capture_reading_position());
    if (target.isValid() && !history_target_)
    {
        restore_reading_position({id, std::nullopt});
    }
    else
    {
        request_position({id, std::nullopt});
    }
    update_reading_navigation();
}

void chat_widget::navigate_down()
{
    if (!reply_returns_.isEmpty())
    {
        const auto origin = reply_returns_.takeLast();
        if (origin.client_id && messages_->outgoing_index(origin.client_id).isValid())
        {
            restore_reading_position(origin);
        }
        else if (!origin.message)
        {
            return_to_latest();
        }
        else if (!history_target_ && messages_->message_index(origin.message).isValid())
        {
            restore_reading_position(origin);
        }
        else
        {
            request_position(origin);
        }
        update_reading_navigation();
        return;
    }
    auto const read = effective_read_position();
    auto const next = messages_->next_incoming(read);
    if (next.isValid() && messages_view_->visualRect(next).top() < messages_view_->viewport()->height())
    {
        return_to_latest();
    }
    else if ((!history_exhausted_ && read < messages_->first_message_id()) || (!next.isValid() && !tail_loaded_))
    {
        // The read frontier lies outside the known interval. Ask for an anchored page.
        request_navigation({history_page::unread});
        preserve_reading([&] { messages_->set_unread_boundary(0); });
    }
    else if (next.isValid())
    {
        scroll_to_unread(next);
        mark_visible_messages();
    }
    else
    {
        return_to_latest();
    }
}

void chat_widget::request_newer_messages()
{
    if (connection_available_ && messages_ready() && !tail_loaded_)
    {
        set_messages_loading(true);
        emit history_requested(
            active_conversation_, history_page::newer, std::max(messages_->last_message_id(), history_floor(active_conversation_)));
    }
}

void chat_widget::return_to_latest()
{
    reply_returns_.clear();
    if (tail_loaded_ && !messages_loading_ && connection_available_)
    {
        history_target_.reset();
        {
            QScopedValueRollback guard(changing_layout_, true);
            messages_->set_unread_boundary(0);
            messages_view_->doItemsLayout();
            messages_view_->scrollToBottom();
        }
        mark_visible_messages();
        update_reading_navigation();
    }
    else
    {
        request_navigation({history_page::latest});
        preserve_reading([&] { messages_->set_unread_boundary(0); });
    }
}

void chat_widget::resume_reading_after_menu(QMenu& menu)
{
    connect(&menu, &QMenu::aboutToHide, this, [this] { QTimer::singleShot(0, this, &chat_widget::mark_visible_messages); });
}

void chat_widget::mark_visible_messages()
{
    if (changing_layout_ || !reading_enabled())
    {
        return;
    }
    auto const height = messages_view_->viewport()->height();
    auto bottom = messages_view_->indexAt(QPoint(1, height - 1));
    auto const floor = effective_read_position();
    for (int row = bottom.isValid() ? bottom.row() : messages_->rowCount() - 1; row >= 0; --row)
    {
        auto index = messages_->index(row, 0);
        auto rect = messages_view_->visualRect(index);
        if (rect.bottom() < 0)
        {
            break;
        }
        auto const id = index.data(message_model::id_role).toLongLong();
        if (id > 0 && id <= floor)
        {
            break;
        }
        if (id > floor && rect.bottom() < height && !index.data(message_model::outgoing_role).toBool() &&
            !index.data(message_model::deleted_role).toBool())
        {
            requested_read_ = id;
            emit read_requested(active_conversation_, id);
            break;
        }
    }
}

void chat_widget::read_failed(qint64 conversation)
{
    if (conversation == active_conversation_)
    {
        requested_read_ = 0;
    }
}
