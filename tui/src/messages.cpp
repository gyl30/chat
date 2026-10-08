#include "app.hpp"
#include "file_io.hpp"

#include <chat/attachment.hpp>
#include <chat/reaction.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <type_traits>

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
    bool const input = data.view == page::conversation && !dialog && !command_mode && !data.selecting && data.can_send();
    if (input && !data.composing) { data.composing = true; }
    else if (!input && data.composing) { stop_composing(); }
}

void app::send()
{
    assert_ui();
    if (sending_ || !writable()) { return; }
    if (data.draft.empty()) { data.status = "消息不能为空"; return; }
    auto const conversation = data.active;
    auto const view = view_;
    auto const draft = data.draft;
    auto const editing = data.editing;
    auto const reply = data.reply ? std::optional(data.reply->id) : std::nullopt;
    if (editing)
    {
        chat::message const* target = nullptr;
        for (auto const* values : {&data.messages, &data.search_results})
        {
            auto found = std::ranges::find(*values, editing, &chat::message::id);
            if (found != values->end()) { target = &*found; break; }
        }
        if (!target || target->from != data.self.id || target->deleted || target->attachment)
        { data.status = "只能编辑自己未删除的文字消息"; return; }
    }
    sending_ = true;
    auto complete = [this, conversation, view, draft, editing, reply](auto value) {
        if (value)
        {
            if (conversation == data.active)
            {
                auto const current_reply = data.reply ? std::optional(data.reply->id) : std::nullopt;
                if (data.draft == draft &&
                    ((data.editing == editing && current_reply == reply) ||
                     (view != view_ && !data.editing && !current_reply)))
                {
                    data.draft.clear();
                    data.reply.reset();
                    data.editing = 0;
                    stop_typing();
                }
            }
            else if (auto saved = drafts_.find(conversation); saved != drafts_.end() && saved->second == draft)
            { saved->second.clear(); }
        }
        if (view != view_ || conversation != data.active)
        { if (value) { conversations(); } return; }
        sending_ = false;
        if (!value) { error(value.error()); return; }
        if constexpr (std::is_same_v<typename decltype(value)::value_type, chat::message>)
        {
            data.apply_message(std::move(*value));
        }
        else
        {
            chat::message sent;
            sent.id = value->message_id;
            sent.conversation = conversation;
            sent.from = data.self.id;
            sent.username = data.self.username;
            sent.timestamp = value->timestamp;
            sent.text = draft;
            sent.reply = std::move(value->reply);
            sent.mentions = std::move(value->mentions);
            sent.avatar = data.self.avatar;
            data.apply_message(std::move(sent));
        }
        notify(editing ? "消息已编辑" : "消息已发送");
        history();
        conversations();
    };
    if (editing) { client_->edit_message(conversation, editing, draft, callback(std::move(complete))); }
    else { client_->send_message(conversation, draft, callback(std::move(complete)), reply); }
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
    if (data.link != connection::online || data.view != page::conversation || !data.at_latest ||
        state::layout(viewport_width, viewport_height) == layout_mode::too_small || dialog || command_mode || history_rows < 1 ||
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
        data.message_selected = data.messages.empty() ? 0 : static_cast<int>(data.messages.size()) - 1;
        history();
        return;
    }
    if (name == "older") { history(true); return; }
    if (name == "read") { mark_visible_read(); return; }
    if (name == "file")
    {
        if (!writable() || sending_) { return; }
        auto const conversation = data.active;
        auto const view = view_;
        auto action = [this, conversation, view](std::string path) {
            if (view != view_ || conversation != data.active || !writable() || sending_) { return; }
            auto bytes = read_local_file(path, chat::max_attachment_size);
            if (!bytes) { data.status = bytes.error(); return; }
            auto filename = std::filesystem::path(path).filename().string();
            auto const reply = data.reply ? std::optional(data.reply->id) : std::nullopt;
            sending_ = true;
            notify("正在上传附件…", true);
            client_->send_attachment(conversation, std::move(filename), std::move(*bytes),
                callback([this, conversation, view, reply](auto value) {
                    if (view != view_ || conversation != data.active) { return; }
                    sending_ = false;
                    if (!value) { error(value.error()); return; }
                    data.apply_message(std::move(*value));
                    if (data.reply && reply && data.reply->id == *reply) { data.reply.reset(); }
                    notify("附件已发送");
                    conversations();
                    mark_visible_read();
                }), reply);
        };
        if (argument.empty()) { ask("发送附件：本地文件路径（最多 10 MiB）", {}, std::move(action)); }
        else { action(std::move(argument)); }
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
