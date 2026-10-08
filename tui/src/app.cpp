#include "app.hpp"
#include <chat/error_text.hpp>
#include <chat/text.hpp>
#include <algorithm>
#include <cassert>
#include <limits>
#include <tuple>

namespace chat::tui
{
app::app(std::function<void()> wake)
    : inbox_(std::make_shared<inbox>(std::move(wake))), ui_thread_(std::this_thread::get_id())
{
    timer_ = std::make_unique<deadline_timer>(inbox_, [this] { tick(); });
}
app::~app() { shutdown(); }
void app::assert_ui() const { assert(std::this_thread::get_id() == ui_thread_); }
void app::drain(std::function<void()> const& before) { assert_ui(); inbox_->drain(before); }
void app::shutdown()
{
    assert_ui();
    if (exiting) { return; }
    stop_typing();
    exiting = true;
    reconnect_enabled_ = false;
    ++session_;
    inbox_->stop();
    timer_->stop();
    if (client_) { client_->close(); client_.reset(); }
    password.clear();
}
bool app::online()
{
    if (data.link == connection::online && client_) { return true; }
    data.status = "未连接，请等待重连或登录";
    return false;
}
bool app::writable()
{
    if (!online()) { return false; }
    if (data.can_send()) { return true; }
    auto const* current = data.active_conversation();
    data.status = current && current->kind == conversation_kind::direct
        ? data.friendship_hint(current->user) : "当前会话不可发送消息";
    return false;
}
void app::error(chat::error const& value)
{
    data.status = value.message.empty() ? "请求失败" : chat::error_text(value);
    // Classify now: a notice expiring in the same batch must not clear this error.
    status_set_ = data.status; data.status_error = true; status_expires_.reset();
    if (value.kind == error_kind::transport &&
        (data.link == connection::connecting || data.link == connection::authenticating))
    {
        disconnected();
        data.status = (data.link == connection::reconnecting ? "连接失败，正在重连：" : "连接失败：") + chat::error_text(value);
    }
}
void app::login(bool registration)
{
    assert_ui();
    if (exiting) { return; }
    if (!chat::valid_username(username))
    {
        data.status = "用户名须为 1–64 UTF-8 字节，首尾不能有空白，且不能含 @ 或控制字符";
        return;
    }
    if (password.empty() || password.size() > 72) { data.status = "密码须为 1–72 字节"; return; }
    if (!(server_url.starts_with("ws://") || server_url.starts_with("wss://")))
    { data.status = "请输入 ws:// 或 wss:// 服务器 URL"; return; }
    if (data.link == connection::connecting || data.link == connection::authenticating) { return; }
    registering_ = registration;
    reconnect_enabled_ = !registration;
    retry_ = 0;
    start_connection();
}
void app::start_connection()
{
    assert_ui();
    ++session_;
    ++view_;
    ++list_request_;
    ++search_request_;
    client_.reset(); // UI-thread destruction joins the previous network thread.
    conversations_busy_ = history_busy_ = search_busy_ = requests_busy_ = sending_ = false;
    requests_again_ = false;
    conversations_again_ = false;
    reconnect_at_.reset();
    data.link = connection::connecting;
    notify(retry_ ? "正在重连…" : "正在连接…", true);
    client_ = std::make_unique<chat::client>();
    client_->set_connected_handler([cb = callback([this](bool) {
        data.link = connection::authenticating;
        if (registering_)
        {
            notify("正在注册…", true);
            client_->register_user(username, password, callback([this](auto value) {
                registering_ = false;
                if (!value)
                {
                    error(value.error());
                    reconnect_enabled_ = false;
                    ++session_;
                    client_.reset();
                    data.link = connection::signed_out;
                    return;
                }
                notify("注册成功，请登录");
                ++session_;
                client_.reset();
                data.link = connection::signed_out;
            }));
            return;
        }
        notify("正在认证…", true);
        client_->authenticate(username, password, callback([this](auto value) {
            if (!value)
            {
                if (value.error().kind == error_kind::transport) { error(value.error()); }
                else if (data.self.id && value.error().code == -32004)
                {
                    client_->close(); disconnected();
                    notify("旧连接正在释放，稍后重新认证…", true);
                }
                else { auto message = chat::error_text(value.error()); logout(); data.status = std::move(message); }
                return;
            }
            if (!value->authenticated) { logout(); data.status = "用户名或密码错误"; return; }
            data.self = {value->user, username, value->avatar};
            data.link = connection::online;
            notify(retry_ ? "已恢复连接" : "已连接");
            retry_ = 0;
            refresh();
        }));
    })]() mutable { cb(true); });
    client_->set_disconnected_handler([cb = callback([this](bool) { disconnected(); })]() mutable { cb(true); });
    client_->set_error_handler(callback([this](chat::error value) { error(value); }));
    client_->set_message_handler(callback([this](chat::message value) { incoming(std::move(value)); }));
    client_->set_message_updated_handler(callback([this](chat::message value) { updated(std::move(value)); }));
    client_->set_reaction_handler(callback([this](reaction_update value) { data.apply_reaction(std::move(value)); }));
    client_->set_friendship_handler(callback([this](std::int64_t) {
        contacts(); friend_requests(); conversations();
    }));
    client_->set_presence_handler(callback([this](chat::presence value) {
        if (data.is_contact(value.user)) { data.presences[value.user] = value; }
    }));
    client_->set_typing_handler(callback([this](typing_event value) {
        if (value.conversation != data.active || value.user == data.self.id) { return; }
        if (auto* conversation = data.active_conversation(); conversation &&
            conversation->kind == conversation_kind::direct && !data.is_contact(value.user)) { return; }
        if (value.typing) { typing_[value.user] = {std::move(value.username), clock::now() + std::chrono::seconds(5)}; }
        else { typing_.erase(value.user); }
        schedule();
    }));
    client_->set_conversation_handler([cb = callback([this](auto value) { changed(value.first, value.second); })]
        (std::int64_t conversation, bool removed) mutable { cb(std::pair{conversation, removed}); });
    client_->set_read_handler([cb = callback([this](auto value) {
        auto [conversation, user, message] = value; data.apply_read(conversation, user, message);
    })](std::int64_t conversation, std::int64_t user, std::int64_t message) mutable {
        cb(std::tuple{conversation, user, message});
    });
    client_->set_avatar_handler([cb = callback([this](auto value) {
        auto [user, avatar] = value;
        if (user == data.self.id) { data.self.avatar = avatar; }
        if (user == data.profile.id) { data.profile.avatar = avatar; }
        for (auto& contact : data.contacts) { if (contact.id == user) { contact.avatar = avatar; } }
    })](std::int64_t user, avatar_state avatar) mutable { cb(std::pair{user, avatar}); });
    client_->set_group_join_request_handler(callback([this](group_join_request_event value) {
        if (value.user == data.self.id && value.state == group_join_request_state::accepted)
        { pending_open_ = value.conversation; conversations(); }
        if (value.conversation == data.active && data.view == page::requests) { requests(); }
    }));
    client_->connect(server_url);
    schedule();
}
void app::logout()
{
    assert_ui();
    stop_typing();
    ++session_;
    ++view_;
    reconnect_enabled_ = false;
    reconnect_at_.reset();
    timer_->schedule({});
    client_.reset();
    password.clear();
    data = state{};
    pages_.clear(); drafts_.clear(); typing_.clear();
    dialog.reset(); prompt_action_ = {};
    command_mode = false; command_text.clear();
    pending_open_ = 0; marked_read_ = 0;
    notify("已退出登录");
}
void app::disconnected()
{
    if (data.link == connection::reconnecting || data.link == connection::signed_out) { return; }
    ++session_; ++view_;
    pending_open_ = 0;
    data.composing = false;
    typing_sent_ = false; typing_stop_at_.reset(); typing_.clear();
    data.friends = {}; data.pick_query.clear();
    data.presences.clear(); data.members.clear(); data.requests.clear(); data.search_results.clear(); data.search_before.reset();
    data.messages.clear(); data.history_before.reset(); data.history_more = false;
    data.read_positions.clear(); data.reply.reset(); data.editing = 0; marked_read_ = 0;
    for (auto& conversation : data.conversations) { conversation.can_send = false; }
    dialog.reset(); prompt_action_ = {}; command_mode = false;
    data.view = data.active ? page::conversation : page::conversations;
    pages_.clear(); data.copy_text.clear(); data.picked_contacts.clear();
    pick_action_.clear(); group_title_.clear();
    conversations_busy_ = history_busy_ = search_busy_ = requests_busy_ = sending_ = false;
    requests_again_ = false;
    if (reconnect_enabled_ && !password.empty())
    {
        data.link = connection::reconnecting;
        notify("正在重连…", true);
        reconnect_at_ = clock::now() + std::chrono::seconds(std::min(15, 1 << std::min(retry_++, 4)));
    }
    else { data.link = connection::signed_out; }
    schedule();
}
void app::reconnect()
{
    if (!data.self.id || password.empty()) { data.status = "请先登录"; return; }
    stop_typing();
    if (client_) { client_->close(); }
    disconnected();
    reconnect_at_ = clock::now(); schedule();
}
void app::refresh()
{
    if (!online()) { return; }
    contacts(); friend_requests(); conversations();
    if (data.active) { history(); members(); }
}
void app::contacts()
{
    if (!online()) { return; }
    auto const request = ++contacts_request_;
    ++presence_request_;
    client_->get_contacts(callback([this, request](auto result) {
        if (request != contacts_request_) { return; }
        if (!result) { error(result.error()); return; }
        data.apply_contacts(std::move(*result)); presence();
    }));
}
void app::friend_requests()
{
    if (!online()) { return; }
    auto const request = ++friends_request_;
    client_->get_friend_requests(callback([this, request](auto value) {
        if (request != friends_request_) { return; }
        if (!value) { error(value.error()); return; }
        data.apply_friend_requests(std::move(*value));
    }));
}
void app::presence()
{
    if (!online()) { return; }
    auto const request = ++presence_request_;
    client_->get_presence(callback([this, request](auto result) {
        if (request != presence_request_) { return; }
        if (!result) { error(result.error()); return; }
        auto& values = *result;
        data.presences.clear();
        for (auto const& value : values) { if (data.is_contact(value.user)) { data.presences[value.user] = value; } }
    }));
}
void app::conversations(bool more)
{
    if (!online()) { return; }
    if (conversations_busy_) { if (!more) { conversations_again_ = true; } return; }
    if (more && !data.next_conversations) { return; }
    conversations_busy_ = true;
    conversations_page(more ? data.next_conversations : std::nullopt, more, ++list_request_, data.conversations.size());
}
void app::conversations_page(std::optional<conversation_cursor> cursor, bool append, std::uint64_t request,
                             std::size_t target, std::vector<chat::conversation> values)
{
    client_->get_conversations(cursor, callback([this, append, request, target, values = std::move(values)](auto result) mutable {
        if (request != list_request_) { return; }
        if (!result) { conversations_busy_ = false; error(result.error()); return; }
        for (auto& value : result->conversations)
        {
            auto found = std::ranges::find(values, value.id, &chat::conversation::id);
            if (found == values.end()) { values.push_back(std::move(value)); }
            else { *found = std::move(value); }
        }
        auto contains = [&](auto id) { return !id || std::ranges::find(values, id, &chat::conversation::id) != values.end(); };
        // Refresh the loaded window atomically; retain active permissions and selection until authoritative replacement.
        if (!append && result->next && (values.size() < target || !contains(data.active) || !contains(pending_open_)))
        {
            conversations_page(result->next, false, request, target, std::move(values));
            return;
        }
        conversations_busy_ = false;
        // Defer destructive absence conclusions when a newer event already requested recovery.
        // Ordinary activity updates still apply, so sustained traffic cannot starve the list.
        if (!append && conversations_again_ && (!contains(data.active) || !contains(pending_open_)))
        {
            conversations_again_ = false;
            conversations();
            return;
        }
        data.apply_conversations({std::move(values), result->next}, append);
        if (!append && !data.next_conversations && data.active && !data.active_conversation())
        {
            drafts_[data.active] = data.draft;
            ++view_;
            stop_composing();
            data.select_conversation(0);
            data.view = page::conversations;
            pages_.clear(); cancel_prompt();
            history_busy_ = search_busy_ = requests_busy_ = sending_ = false;
            requests_again_ = false;
            marked_read_ = 0;
            notify("当前会话已不可访问，已刷新会话列表");
        }
        if (data.active && !data.can_send()) { stop_composing(); }
        if (pending_open_)
        {
            if (std::ranges::find(data.conversations, pending_open_, &chat::conversation::id) != data.conversations.end())
            { auto id = pending_open_; pending_open_ = 0; open_conversation(id); }
            else if (data.next_conversations) { conversations(true); return; }
            else { pending_open_ = 0; }
        }
        if (conversations_again_) { conversations_again_ = false; conversations(); }
    }));
}
void app::open_conversation(std::int64_t id)
{
    assert_ui();
    pending_open_ = 0;
    if (!online()) { return; }
    stop_typing();
    if (data.active) { drafts_[data.active] = data.draft; }
    ++view_; ++search_request_;
    history_busy_ = search_busy_ = requests_busy_ = sending_ = false;
    requests_again_ = false;
    data.select_conversation(id);
    data.draft = drafts_[id];
    data.view = page::conversation;
    pages_.clear();
    marked_read_ = 0;
    typing_.clear(); cancel_prompt();
    history(); members();
}
void app::history(bool older)
{
    if (!online() || !data.active || history_busy_) { return; }
    if (older && (!data.history_more || !data.history_before)) { return; }
    history_busy_ = true;
    auto const request = ++history_request_;
    auto const conversation = data.active; auto const view = view_;
    auto before = older ? data.history_before : std::nullopt;
    client_->get_messages(conversation, before, callback([this, conversation, view, older, request](auto value) {
        if (view != view_ || conversation != data.active || request != history_request_) { return; }
        history_busy_ = false;
        if (!value) { error(value.error()); return; }
        data.apply_history(std::move(*value), older);
        mark_visible_read();
    }));
}
void app::search(std::string query, bool more)
{
    if (!online() || !data.active || query.empty()) { return; }
    if (more && (!data.search_more || !data.search_before || search_busy_)) { return; }
    if (!more)
    {
        stop_composing(); navigate(page::search); data.search_query = query;
        data.search_results.clear(); data.search_before.reset(); data.selected = 0;
    }
    search_busy_ = true;
    auto const request = ++search_request_; auto const conversation = data.active; auto const view = view_;
    auto before = more ? data.search_before : std::nullopt;
    client_->search_messages(conversation, query, before, callback([this, conversation, view, query, request, more](auto value) {
        if (request != search_request_ || view != view_ || conversation != data.active || query != data.search_query) { return; }
        search_busy_ = false;
        if (!value) { error(value.error()); return; }
        data.apply_search(std::move(*value), more);
    }));
}
void app::navigate(page target)
{
    assert_ui();
    pending_open_ = 0;
    stop_composing();
    if (data.view != target)
    {
        ++view_; ++search_request_;
        history_busy_ = search_busy_ = requests_busy_ = sending_ = false;
        requests_again_ = false;
        bool const primary = target == page::conversations || target == page::contacts;
        bool const chat_tab = data.view == page::conversations && target == page::conversation;
        bool const friend_tab = (data.view == page::friend_requests || data.view == page::friend_sent) &&
                                (target == page::friend_requests || target == page::friend_sent);
        // Primary destinations and sibling tabs are not nested detail pages.
        if (primary) { pages_.clear(); }
        else if (!chat_tab && !friend_tab) { pages_.push_back(data.view); }
        data.view = target; data.selected = 0;
        if (target == page::conversation) { data.selecting = false; }
    }
    cancel_prompt();
    if (target == page::conversation && data.active) { history(); members(); }
}
void app::back()
{
    assert_ui();
    pending_open_ = 0;
    if (dialog) { cancel_prompt(); return; }
    if (command_mode) { command_mode = false; return; }
    if (data.composing) { stop_composing(); return; }
    ++view_; ++search_request_; history_busy_ = search_busy_ = requests_busy_ = sending_ = false;
    requests_again_ = false;
    bool const from_help = data.view == page::help;
    if (!pages_.empty()) { data.view = pages_.back(); pages_.pop_back(); }
    else { data.view = page::conversations; }
    // Help is a look-up: the page behind it keeps its selection.
    data.selected = from_help ? help_return_selected_ : 0;
    if (data.view == page::conversation) { history(); members(); }
    mark_visible_read();
}
void app::ask(std::string title, std::string initial, std::function<void(std::string)> action, bool confirmation)
{
    stop_composing();
    dialog = prompt{std::move(title), std::move(initial), confirmation};
    auto const session = session_; auto const view = view_;
    prompt_action_ = [this, session, view, action = std::move(action)](std::string text) {
        if (session == session_ && view == view_) { action(std::move(text)); }
    };
}
void app::confirm(std::string title, std::function<void()> action)
{
    ask(std::move(title) + " [y/N]", {}, [action = std::move(action)](std::string text) {
        if (text == "y" || text == "Y") { action(); }
    }, true);
}
void app::submit_prompt()
{
    if (!dialog) { return; }
    auto text = std::move(dialog->text); auto action = std::move(prompt_action_);
    dialog.reset(); prompt_action_ = {};
    if (action) { action(std::move(text)); }
}
void app::cancel_prompt() { dialog.reset(); prompt_action_ = {}; }
void app::activate()
{
    if (data.view == page::conversations)
    {
        if (data.conversation_selected >= 0 && data.conversation_selected < static_cast<int>(data.conversations.size()))
        { open_conversation(data.conversations[data.conversation_selected].id); }
    }
    else if (data.view == page::contacts || data.view == page::users)
    {
        if (data.view == page::contacts && data.selected == 0) { command("friend-requests"); return; }
        if (auto const* user = data.selected_user())
        { data.profile = *user; navigate(page::profile); }
    }
    else if (data.view == page::friend_requests || data.view == page::friend_sent)
    {
        auto const& requests = data.view == page::friend_requests ? data.friends.incoming : data.friends.outgoing;
        if (data.selected >= 0 && static_cast<std::size_t>(data.selected) < requests.size())
        { data.profile = requests[data.selected].user; navigate(page::profile); }
    }
    else if (data.view == page::pick_contacts) { finish_pick(); }
    else if (data.view == page::search) { command("copy"); }
}
void app::command(std::string text)
{
    assert_ui();
    if (exiting) { return; }
    if (!text.empty() && text.front() == ':') { text.erase(0, 1); }
    auto const split = text.find(' ');
    auto const name = text.substr(0, split);
    auto argument = split == std::string::npos ? std::string{} : text.substr(split + 1);
    if (name == "quit") { shutdown(); return; }
    if (name == "logout") { confirm("退出当前账号？", [this] { logout(); }); return; }
    if (name == "reconnect") { reconnect(); return; }
    if (name == "help") { auto const selected = data.selected; navigate(page::help); help_return_selected_ = selected; return; }
    if (data.self.id && name == "new") { navigate(page::new_action); return; }
    if (data.self.id && (name == "chats" || name == "conversations")) { navigate(page::conversations); return; }
    if (data.self.id && name == "account")
    { profile_command(name, {}); pages_.clear(); data.selected = 0; return; }
    if (data.self.id && name == "contacts")
    {
        navigate(page::contacts);
        if (data.link == connection::online) { contacts(); friend_requests(); }
        return;
    }
    if (name == "filter" && (data.view == page::contacts || data.view == page::pick_contacts))
    {
        auto filter = [this](std::string query) {
            (data.view == page::contacts ? data.contacts_query : data.pick_query) = std::move(query);
            data.selected = 0;
        };
        if (argument.empty()) { ask("搜索已接受的好友 · 留空显示全部", data.view == page::contacts ? data.contacts_query : data.pick_query, std::move(filter)); }
        else { filter(std::move(argument)); }
        return;
    }
    if (!online()) { return; }
    if (name == "friend-requests" || name == "friend-sent")
    { navigate(name == "friend-requests" ? page::friend_requests : page::friend_sent); friend_requests(); return; }
    if (name == "refresh") { refresh(); return; }
    if (name == "more")
    {
        if (data.view == page::conversations) { conversations(true); }
        if (data.view == page::requests) { requests(true); }
        if (data.view == page::search) { search(data.search_query, true); }
        return;
    }
    if (name == "search-users" || name == "add-contact")
    {
        auto find = [this](std::string query) {
            if (query.empty()) { return; }
            navigate(page::users); ++view_; data.users.clear(); auto const view = view_;
            client_->search_users(query, callback([this, view](auto result) {
                if (view != view_ || data.view != page::users) { return; }
                if (!result) { error(result.error()); return; }
                data.users = std::move(*result); data.selected = 0;
            }));
        };
        if (argument.empty()) { ask("搜索用户（姓名前缀）", {}, find); } else { find(std::move(argument)); }
        return;
    }
    if (name == "mute" || name == "pin")
    {
        auto const* conversation = data.active_conversation();
        if (data.view == page::conversations && data.conversation_selected >= 0 && data.conversation_selected < static_cast<int>(data.conversations.size()))
        { conversation = &data.conversations[data.conversation_selected]; }
        if (!conversation) { return; }
        auto done = result([this](bool) { conversations(); });
        if (name == "mute") { client_->set_conversation_muted(conversation->id, !conversation->muted, std::move(done)); }
        else { client_->set_conversation_pinned(conversation->id, !conversation->pinned, std::move(done)); }
        return;
    }
    if (name == "group" || name == "create-group" || name == "members" || name == "invite" || name == "rename" ||
        name == "announcement" || name == "show-announcement" || name == "pinned" || name == "admin" || name == "transfer" || name == "kick" || name == "leave" ||
        name == "pin-message" || name == "unpin-message" || name == "link" || name == "link-create" ||
        name == "link-revoke" || name == "approval" || name == "requests" || name == "accept" || name == "reject" || name == "join")
    { group_command(name, std::move(argument)); return; }
    if (name == "profile" || name == "account" || name == "add" || name == "remove-contact" || name == "message" ||
        name == "avatar" || name == "avatar-clear" || name == "copy-user" ||
        name == "accept-friend" || name == "reject-friend" || name == "cancel-friend")
    { profile_command(name, std::move(argument)); return; }
    message_command(name, std::move(argument));
}
void app::changed(std::int64_t conversation, bool removed)
{
    if (removed && conversation == data.active)
    {
        stop_composing(); ++view_; data.select_conversation(0); data.view = page::conversations;
        pages_.clear(); cancel_prompt(); notify("已离开该群");
    }
    conversations();
    if (!removed && conversation == data.active)
    {
        // Membership removal/rejoin can reset server read positions to zero.
        // Invalidate any pre-change history response before authoritative recovery.
        ++history_request_;
        history_busy_ = false;
        data.read_positions.clear(); marked_read_ = 0;
        history(); members();
    }
}
void app::stop_typing()
{
    if (typing_sent_ && client_ && data.link == connection::online && data.active)
    { client_->set_typing(data.active, false, [](auto) {}); }
    typing_sent_ = false; typing_stop_at_.reset(); schedule();
}
void app::schedule()
{
    if (exiting) { return; }
    std::optional<clock::time_point> next = reconnect_at_;
    auto consider = [&](auto deadline) { if (deadline && (!next || *deadline < *next)) { next = deadline; } };
    consider(typing_stop_at_);
    consider(status_expires_);
    for (auto const& [id, value] : typing_) { (void)id; consider(std::optional{value.second}); }
    timer_->schedule(next);
}
void app::tick()
{
    assert_ui();
    auto now = clock::now();
    if (reconnect_at_ && now >= *reconnect_at_) { start_connection(); }
    if (typing_stop_at_ && now >= *typing_stop_at_) { stop_typing(); }
    std::erase_if(typing_, [&](auto const& item) { return item.second.second <= now; });
    if (status_expires_ && now >= *status_expires_)
    {
        status_expires_.reset();
        // Only the notice that set this deadline is cleared, never text that replaced it.
        if (!data.status_error && data.status == status_set_) { data.status.clear(); status_set_.clear(); }
    }
    schedule();
}
void app::notify(std::string text, bool sticky)
{
    data.status = std::move(text);
    data.status_error = false;
    status_set_ = data.status;
    status_expires_.reset();
    if (!sticky) { status_expires_ = clock::now() + notice_duration; }
    schedule();
}
void app::observe_status()
{
    if (data.status == status_set_) { return; }
    // Anything not set through notify() reports a failure and stays until the next key press.
    status_set_ = data.status;
    data.status_error = !data.status.empty();
    status_expires_.reset();
}
void app::dismiss_error()
{
    if (!data.status_error) { return; }
    data.status.clear(); status_set_.clear();
    data.status_error = false;
}
std::string app::typing_text() const
{
    std::string text; int count = 0;
    for (auto const& [id, value] : typing_)
    {
        (void)id;
        if (count++ == 2) { text += " 等"; break; }
        if (!text.empty()) { text += "、"; }
        text += value.first;
    }
    return text.empty() ? text : text + " 正在输入…";
}
}
