#include "app.hpp"
#include "file_io.hpp"

#include <chat/attachment.hpp>
#include <chat/error_text.hpp>
#include <chat/reaction.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <utility>

namespace chat::tui
{
void app::compose_changed()
{
    assert_ui();
    if (!data.composing || !data.can_send() || !client_) { stop_typing(); return; }
    if (data.draft.empty()) { stop_typing(); return; }
    auto const now = clock::now();
    typing_stop_at_ = now + std::chrono::seconds(3);
    if (!typing_sent_ || now - typing_last_ >= std::chrono::seconds(2))
    {
        typing_sent_ = true;
        typing_last_ = now;
        client_->set_typing(data.active, true, callback([](auto) {}));
    }
    schedule();
}

void app::stop_composing()
{
    assert_ui();
    if (data.composing && before_input_change) { before_input_change(); }
    stop_typing();
    data.composing = false;
}

void app::sync_focus()
{
    assert_ui();
    // Overlays and other pages take the keys; an open, writable conversation types directly.
    bool const input = data.view == page::conversation && !dialog && !command_mode && !menu_open && !palette_open && !data.selecting && data.can_send();
    if (input && !data.composing) { data.composing = true; }
    else if (!input && data.composing) { stop_composing(); }
}

void app::send()
{
    assert_ui();
    if ((data.editing && saving_edit_.contains(data.active)) || !writable()) { return; }
    if (data.draft.empty()) { data.status = "消息不能为空"; return; }
    if (!data.editing)
    {
        // The message takes its text and reply with it; the composer is free at once.
        outgoing_message item;
        item.client_id = chat::client::new_client_id();
        item.conversation = data.active;
        item.text = std::exchange(data.draft, {});
        item.reply = std::exchange(data.reply, {});
        data.outgoing.push_back(std::move(item));
        stop_typing();
        follow_outgoing();
        submit(data.outgoing.back());
        return;
    }
    auto const conversation = data.active;
    auto const view = view_;
    auto const draft = data.draft;
    auto const editing = data.editing;
    chat::message const* target = nullptr;
    for (auto const* values : {&data.messages, &data.search_results})
    {
        auto found = std::ranges::find(*values, editing, &chat::message::id);
        if (found != values->end()) { target = &*found; break; }
    }
    if (!target || target->from != data.self.id || target->deleted || target->attachment)
    { data.status = "只能编辑自己未删除的文字消息"; return; }
    saving_edit_.insert(conversation);
    client_->edit_message(conversation, editing, draft, callback([this, conversation, view, draft, editing](auto value) {
        saving_edit_.erase(conversation);
        if (value && conversation == data.active && data.editing == editing && data.draft == draft) { cancel_edit(); }
        if (view != view_ || conversation != data.active)
        { if (value) { conversations(); } return; }
        if (!value) { error(value.error()); return; }
        data.apply_message(std::move(*value));
        notify("消息已编辑");
        history();
        conversations();
    }));
}

void app::follow_outgoing()
{
    // The view follows what was just sent, so its state stays in sight.
    data.at_latest = true;
    data.message_selected = static_cast<int>(data.messages.size() + data.active_outgoing().size()) - 1;
}

void app::submit(outgoing_message const& item)
{
    auto const reply = item.reply ? std::optional(item.reply->id) : std::nullopt;
    auto done = callback([this, client_id = item.client_id, conversation = item.conversation](std::expected<chat::message, chat::error> value) {
        if (!value && value.error().kind == error_kind::rpc && value.error().code == chat::conversation_unavailable)
        {
            drop_outgoing([conversation](std::int64_t id) { return id == conversation; });
            return;
        }
        finish_outgoing(client_id, std::move(value));
    });
    if (item.file) { client_->send_attachment(item.conversation, item.text, *item.file, std::move(done), reply, item.client_id); }
    else { client_->send_message(item.conversation, item.text, std::move(done), reply, item.client_id); }
}

void app::finish_outgoing(std::int64_t client_id, std::expected<chat::message, chat::error> value)
{
    assert_ui();
    auto found = std::ranges::find(data.outgoing, client_id, &outgoing_message::client_id);
    // Already settled by a history page that brought the stored message, or discarded.
    if (found == data.outgoing.end())
    {
        if (value) { data.apply_message(std::move(*value)); }
        return;
    }
    if (!value)
    {
        // A lost connection leaves it sending: it is sent again after the next sign-in.
        if (value.error().kind == error_kind::transport) { return; }
        found->failed = true;
        found->error = chat::error_text(value.error());
        if (found->conversation == data.active) { data.status = "发送失败：" + found->error + "，选中该消息按 Enter 处理"; }
        return;
    }
    bool const attachment = found->file.has_value();
    auto const conversation = value->conversation;
    data.apply_message(std::move(*value));
    conversations();
    if (conversation != data.active) { return; }
    notify(attachment ? "附件已发送" : "消息已发送");
    mark_visible_read();
}

void app::drop_outgoing(std::function<bool(std::int64_t conversation)> const& gone)
{
    auto const dropped = std::erase_if(data.outgoing, [&](outgoing_message const& item) { return gone(item.conversation); });
    if (dropped) { data.status = std::to_string(dropped) + " 条未发送的消息已丢弃：会话已不可访问"; }
}

void app::cancel_edit()
{
    assert_ui();
    if (!data.editing) { return; }
    data.editing = 0;
    data.draft = saved_draft(data.active);
    stop_typing();
}

void app::mark_visible_read()
{
    // Decided once the current batch is applied and the layout for the current size is known.
    read_check = true;
}

void app::check_read()
{
    assert_ui();
    read_check = false;
    if (!client_ || data.link != connection::online || data.view != page::conversation || !data.at_latest ||
        state::layout(viewport_width, viewport_height) == layout_mode::too_small || dialog || command_mode || menu_open || palette_open || history_rows < 1 ||
        history_busy_ || data.messages.empty()) { return; }
    auto const conversation = data.active;
    auto const view = view_;
    auto const latest = data.messages.back().id;
    if (latest <= marked_read_) { return; }
    marked_read_ = latest;
    client_->mark_read(conversation, latest, callback([this, conversation, view, latest](auto value) {
        if (conversation != data.active || view != view_) { return; }
        if (!value)
        {
            if (marked_read_ == latest) { marked_read_ = 0; }
            error(value.error());
            return;
        }
        data.apply_read(conversation, data.self.id, *value);
        if (data.at_latest && !data.messages.empty() && *value >= data.messages.back().id)
        {
            if (auto* current = data.active_conversation()) { current->unread = 0; }
        }
    }));
}

void app::select_message(int delta)
{
    assert_ui();
    data.move_message(delta);
    mark_visible_read();
}

void app::incoming(chat::message value)
{
    assert_ui();
    data.apply_message(std::move(value));
    conversations();
    mark_visible_read();
}

void app::updated(chat::message value)
{
    assert_ui();
    if (value.deleted && value.id == data.editing) { cancel_edit(); }
    data.apply_message(std::move(value));
    conversations();
}

void app::message_command(std::string const& name, std::string argument)
{
    assert_ui();
    if (name == "search")
    {
        if (!online() || data.active == 0) { return; }
        auto const view = view_;
        auto const conversation = data.active;
        auto action = [this, view, conversation](std::string query) {
            if (view != view_ || conversation != data.active) { return; }
            search(std::move(query));
        };
        if (argument.empty()) { ask("搜索当前会话", {}, std::move(action)); }
        else { action(std::move(argument)); }
        return;
    }
    if (name == "compose")
    {
        if (writable()) { navigate(page::conversation); data.selecting = false; data.composing = true; }
        return;
    }
    if (name == "latest")
    {
        data.at_latest = true;
        data.message_selected = std::max(0, static_cast<int>(data.messages.size() + data.active_outgoing().size()) - 1);
        older_again_ = false;  // G replaces a PgUp still waiting on a page
        history();
        return;
    }
    if (name == "older") { history(true); return; }
    if (name == "read") { mark_visible_read(); return; }
    if (name == "file")
    {
        if (!writable()) { return; }
        auto const conversation = data.active;
        auto const view = view_;
        auto action = [this, conversation, view](std::string path) {
            if (view != view_ || conversation != data.active || !writable()) { return; }
            auto bytes = read_local_file(path, chat::max_attachment_size);
            if (!bytes) { data.status = bytes.error(); return; }
            outgoing_message item;
            item.client_id = chat::client::new_client_id();
            item.conversation = conversation;
            item.text = std::filesystem::path(path).filename().string();
            item.file = std::move(*bytes);
            item.reply = std::exchange(data.reply, {});
            data.outgoing.push_back(std::move(item));
            follow_outgoing();
            submit(data.outgoing.back());
        };
        if (argument.empty()) { ask("发送附件：本地文件路径（最多 10 MiB）", {}, std::move(action)); }
        else { action(std::move(argument)); }
        return;
    }
    if (name == "retry" || name == "retry-without-reply" || name == "discard" || (name == "copy" && data.selected_outgoing()))
    {
        auto const* selected = data.selected_outgoing();
        if (!selected) { data.status = "请先选择未发送的消息"; return; }
        auto item = std::ranges::find(data.outgoing, selected->client_id, &outgoing_message::client_id);
        if (name == "copy")
        {
            data.copy_text = item->text;
            navigate(page::copy);
            return;
        }
        if (!item->failed) { data.status = "消息仍在发送"; return; }
        if (name == "discard")
        {
            data.outgoing.erase(item);
            data.move_message(0);
            return;
        }
        if (!online()) { return; }
        // The same client_id: whatever reached the server before is not stored twice.
        item->failed = false;
        item->error.clear();
        if (name == "retry-without-reply") { item->reply.reset(); }
        submit(*item);
        return;
    }
    auto const* selected = data.selected_message();
    if (!selected) { data.status = "请先选择消息"; return; }
    auto const message = *selected;
    auto const conversation = data.active;
    auto const view = view_;
    if (name == "copy")
    {
        data.copy_text = message.deleted ? "消息已删除" : message.text;
        if (message.attachment) { data.copy_text = message.attachment->filename; }
        navigate(page::copy);
        return;
    }
    if (name == "reply")
    {
        if (!writable()) { return; }
        if (message.deleted) { data.status = "无法回复已删除消息"; return; }
        cancel_edit();
        navigate(page::conversation);
        data.reply = quoted_message{message.id, message.from, message.username, message.text, message.edited_at, false};
        data.editing = 0;
        data.selecting = false;
        data.composing = true;
        return;
    }
    if (name == "edit")
    {
        if (!writable()) { return; }
        if (message.from != data.self.id || message.deleted || message.attachment)
        { data.status = "只能编辑自己未删除的文字消息"; return; }
        navigate(page::conversation);
        if (!data.editing) { drafts_[data.active] = data.draft; }
        data.editing = message.id;
        data.reply.reset();
        data.draft = message.text;
        data.selecting = false;
        data.composing = true;
        return;
    }
    if (name == "delete")
    {
        if (!online()) { return; }
        if (message.from != data.self.id || message.deleted)
        { data.status = "只能删除自己未删除的消息"; return; }
        confirm("删除这条消息？", [this, conversation, view, id = message.id] {
            if (view != view_ || conversation != data.active || !online()) { return; }
            client_->delete_message(conversation, id, callback([this, conversation, view](auto value) {
                if (view != view_ || conversation != data.active) { return; }
                if (!value) { error(value.error()); return; }
                updated(std::move(*value));
                notify("消息已删除");
            }));
        });
        return;
    }
    if (name == "reaction")
    {
        if (!writable()) { return; }
        if (message.deleted) { data.status = "已删除消息不能回应"; return; }
        auto action = [this, conversation, view, id = message.id](std::string choice) {
            if (view != view_ || conversation != data.active || !writable()) { return; }
            int index = -1;
            auto parsed = std::from_chars(choice.data(), choice.data() + choice.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != choice.data() + choice.size() ||
                index < 0 || index > static_cast<int>(chat::reaction_choices.size()))
            { data.status = "请选择 0–6"; return; }
            std::string emoji = index == 0 ? "" : std::string(chat::reaction_choices[index - 1]);
            client_->set_message_reaction(conversation, id, std::move(emoji),
                callback([this, conversation, view](auto value) {
                    if (view != view_ || conversation != data.active) { return; }
                    if (!value) { error(value.error()); return; }
                    data.apply_reaction(std::move(*value));
                    notify("回应已更新");
                }));
        };
        if (argument.empty()) { ask("回应：0 清除  1 👍  2 ❤️  3 😂  4 😮  5 😢  6 🎉", {}, std::move(action)); }
        else { action(std::move(argument)); }
        return;
    }
    if (name == "save")
    {
        if (!online()) { return; }
        if (message.deleted || !message.attachment) { data.status = "请选择含附件的消息"; return; }
        auto action = [this, conversation, view, id = message.id](std::string path) {
            if (view != view_ || conversation != data.active || !online()) { return; }
            if (path.empty()) { data.status = "请输入明确的保存路径"; return; }
            notify("正在下载附件…", true);
            client_->get_attachment(conversation, id, callback([this, conversation, view, path = std::move(path)](auto value) {
                if (view != view_ || conversation != data.active) { return; }
                if (!value) { error(value.error()); return; }
                auto saved = save_local_file(path, *value);
                if (saved) { notify("附件已保存：" + path); }
                else { data.status = saved.error(); }
            }));
        };
        if (argument.empty()) { ask("附件保存到（必须是新文件路径）", {}, std::move(action)); }
        else { action(std::move(argument)); }
        return;
    }
    data.status = "未知消息命令";
}
}
