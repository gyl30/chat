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
    return {top.data(message_model::id_role).toLongLong(), messages_view_->visualRect(top).top()};
}

void chat_widget::restore_reading_position(reading_position position)
{
    if (!position.message)
    {
        if (tail_loaded_)
        {
            messages_view_->scrollToBottom();
        }
        return;
    }
    auto top = messages_->message_index(position.message);
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
    if (changing_layout_ || !messages_ready())
    {
        change();
        return;
    }
    QScopedValueRollback guard(changing_layout_, true);
    const auto position = capture_reading_position();
    change();
    messages_view_->doItemsLayout();
    restore_reading_position(position);
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
    return connection_available_ && messages_ready() && messages_view_->isVisible() && window()->isActiveWindow() && !window()->isMinimized() &&
           !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
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

void chat_widget::request_position(reading_position position)
{
    target_position_ = position;
    set_messages_loading(true);
    emit history_requested(active_conversation_, history_page::around, position.message);
}

void chat_widget::request_entry_history()
{
    auto position = reading_positions_.take(active_conversation_);
    if (position.message > history_floor(active_conversation_))
    {
        request_position(position);
        return;
    }
    set_messages_loading(true);
    emit history_requested(active_conversation_, history_page::unread);
}

void chat_widget::begin_history_refresh()
{
    set_messages_loading(true);
    update_reading_navigation();
    if (target_position_)
    {
        request_position(*target_position_);
        return;
    }
    const bool at_end = messages_loaded_ && !capture_reading_position().message;
    // Until the server answers, a live delivery may be separated from these rows by a gap.
    tail_loaded_ = false;
    if (at_end)
    {
        emit history_requested(active_conversation_, history_page::latest);
        return;
    }
    emit history_requested(active_conversation_,
                           messages_loaded_ ? history_page::refresh : history_page::unread,
                           std::max<qint64>(0, messages_->first_message_id() - 1));
}

void chat_widget::update_reading_navigation()
{
    if (!history_down_)
    {
        return;
    }
    auto* viewport = messages_view_->viewport();
    auto const* scroll = messages_view_->verticalScrollBar();
    bool const away = !tail_loaded_ || scroll->value() < scroll->maximum();
    auto const unread = conversation_unread();
    history_down_->setText(unread ? QStringLiteral("↓  %1").arg(unread) : QStringLiteral("↓"));
    history_down_->setToolTip(unread ? QStringLiteral("前往未读消息") : QStringLiteral("返回最新消息"));
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
        return_to_latest();
        return;
    }
    messages_->set_unread_boundary(index.data(message_model::id_role).toLongLong());
    messages_view_->doItemsLayout();
    scroll->setValue(scroll->value() + messages_view_->visualRect(index).top() - std::min(64, messages_view_->viewport()->height() / 5));
}

void chat_widget::navigate_down()
{
    auto const read = effective_read_position();
    auto const next = messages_->next_incoming(read);
    if (next.isValid() && messages_view_->visualRect(next).top() < messages_view_->viewport()->height())
    {
        return_to_latest();
    }
    else if ((!history_exhausted_ && read < messages_->first_message_id()) || (!next.isValid() && !tail_loaded_))
    {
        // The read frontier lies outside the known interval. Ask for an anchored page.
        set_messages_loading(true);
        messages_->set_unread_boundary(0);
        emit history_requested(active_conversation_, history_page::unread);
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
    messages_->set_unread_boundary(0);
    target_position_.reset();
    if (tail_loaded_ && !messages_loading_)
    {
        messages_view_->doItemsLayout();
        messages_view_->scrollToBottom();
        mark_visible_messages();
    }
    else
    {
        set_messages_loading(true);
        emit history_requested(active_conversation_, history_page::latest);
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
