#include "state.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace chat::tui
{
namespace
{
void merge_quote(std::optional<quoted_message>& current, std::optional<quoted_message> const& incoming)
{
    if (!incoming) { return; }
    if (!current) { current = incoming; return; }
    if (current->id == incoming->id && !current->deleted &&
        (incoming->deleted || incoming->edited_at.value_or(0) > current->edited_at.value_or(0)))
    { current = incoming; }
}

void merge(message& current, message const& incoming)
{
    if (current.deleted) { return; }
    auto quote = current.reply;
    auto reactions = current.reactions;
    auto revision = current.reaction_revision;
    if (incoming.deleted || incoming.edited_at.value_or(0) > current.edited_at.value_or(0))
    {
        current = incoming;
        if (!current.deleted && revision > current.reaction_revision)
        {
            current.reaction_revision = revision;
            current.reactions = std::move(reactions);
        }
        merge_quote(current.reply, quote);
    }
    if (!current.deleted && incoming.reaction_revision > current.reaction_revision)
    {
        current.reaction_revision = incoming.reaction_revision;
        current.reactions = incoming.reactions;
    }
    merge_quote(current.reply, incoming.reply);
}

void update_quote(std::optional<quoted_message>& quote, message const& value)
{
    if (!quote || quote->id != value.id || quote->deleted) { return; }
    if (value.deleted || value.edited_at.value_or(0) > quote->edited_at.value_or(0))
    {
        quote = quoted_message{value.id, value.from, value.username, value.text, value.edited_at, value.deleted};
    }
}

int bounded(int index, std::size_t size)
{
    return size == 0 ? 0 : std::clamp(index, 0, static_cast<int>(size) - 1);
}

bool friend_matches(std::string const& username, std::string const& query)
{
    return query.empty() || std::search(username.begin(), username.end(), query.begin(), query.end(), [](unsigned char a, unsigned char b) {
        return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == (b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b);
    }) != username.end();
}
}

chat::conversation const* state::active_conversation() const
{
    auto found = std::ranges::find(conversations, active, &chat::conversation::id);
    return found == conversations.end() ? nullptr : &*found;
}

chat::conversation* state::active_conversation()
{
    return const_cast<chat::conversation*>(std::as_const(*this).active_conversation());
}

message const* state::selected_message() const
{
    auto const& values = view == page::search ? search_results : messages;
    auto index = view == page::search ? selected : message_selected;
    return index < 0 || static_cast<std::size_t>(index) >= values.size() ? nullptr : &values[index];
}

bool state::can_send() const
{
    auto const* current = active_conversation();
    return link == connection::online && current && current->can_send;
}

member_role state::self_role() const
{
    auto found = std::ranges::find(members, self.id, &conversation_member::id);
    return found == members.end() ? member_role::member : found->role;
}

std::vector<menu_item> state::message_actions(message const& value) const
{
    bool const online = link == connection::online;
    bool const writable = can_send();
    bool const own = value.from == self.id;
    auto const* current = active_conversation();
    std::vector<menu_item> items;
    if (writable && !value.deleted)
    {
        items.push_back({"回复", 'r', "reply", {}});
        items.push_back({"表情回应", 'a', "reaction", {}});
    }
    items.push_back({"复制", 'y', "copy", {}});
    // Editing needs send permission; deleting one's own message does not.
    if (writable && own && !value.deleted && !value.attachment) { items.push_back({"编辑", 'e', "edit", {}}); }
    if (online && own && !value.deleted) { items.push_back({"删除", 'd', "delete", {}}); }
    if (online && value.attachment && !value.deleted) { items.push_back({"保存文件", 's', "save", {}}); }
    if (online && current && current->kind == conversation_kind::group && self_role() != member_role::member && !value.deleted)
    {
        bool const pinned = current->pinned_message && current->pinned_message->id == value.id;
        items.push_back({pinned ? "取消群置顶" : "置顶到群", 't', pinned ? "unpin-message" : "pin-message", {}});
    }
    return items;
}

std::vector<menu_item> state::member_actions(conversation_member const& value) const
{
    std::vector<menu_item> items{{"查看资料", 'v', "profile", {}}};
    // Nobody manages themselves; every change needs a connection.
    if (value.id == self.id || link != connection::online) { return items; }
    auto const role = self_role();
    if (role == member_role::owner && value.role == member_role::member)
    {
        bool const full = std::ranges::count(members, member_role::admin, &conversation_member::role) >= 3;
        items.push_back({"设为管理员", 'A', "admin", full ? "管理员已满 3 人" : ""});
    }
    if (role == member_role::owner && value.role == member_role::admin)
    {
        items.push_back({"取消管理员", 'A', "admin", {}});
        // Ownership goes only to a current administrator.
        items.push_back({"转让群主", 'O', "transfer", {}});
    }
    if ((role == member_role::owner && value.role != member_role::owner) ||
        (role == member_role::admin && value.role == member_role::member))
    { items.push_back({"移除成员", 'D', "kick", {}}); }
    return items;
}

std::vector<menu_item> state::account_actions() const
{
    // The profile page shows the avatar state; "account" goes there rather than reopening this menu.
    std::vector<menu_item> items{{"我的资料", 'p', "account", {}}};
    // Avatar changes need the server; copying and signing out do not.
    if (link == connection::online)
    {
        items.push_back({"设置头像（PNG/JPEG）", 'a', "avatar", {}});
        if (self.avatar.present) { items.push_back({"清除头像", 'c', "avatar-clear", {}}); }
    }
    items.push_back({"复制用户名", 'y', "copy-self", {}});
    items.push_back({"退出登录", 'l', "logout", {}});
    return items;
}

std::vector<message const*> state::history_entries() const
{
    std::vector<message const*> entries;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    {
        auto const& m = *it;
        if (m.deleted) { continue; }
        bool const image = m.attachment && m.attachment->media_type.starts_with("image/");
        bool const link = m.text.find("http://") != std::string::npos || m.text.find("https://") != std::string::npos;
        bool const wanted = history_category == 1 ? image : history_category == 2 ? m.attachment && !image
                          : history_category == 3 ? link : true;
        if (wanted) { entries.push_back(&m); }
    }
    return entries;
}

bool state::is_contact(std::int64_t id) const
{
    return std::ranges::find(contacts, id, &user::id) != contacts.end();
}

void state::apply_conversations(conversations_result result, bool append)
{
    auto selected_id = conversation_selected >= 0 && static_cast<std::size_t>(conversation_selected) < conversations.size()
        ? conversations[conversation_selected].id : 0;
    if (!append) { conversations.clear(); }
    for (auto& value : result.conversations)
    {
        auto found = std::ranges::find(conversations, value.id, &chat::conversation::id);
        if (found == conversations.end()) { conversations.push_back(std::move(value)); }
        else { *found = std::move(value); }
    }
    next_conversations = result.next;
    auto found = std::ranges::find(conversations, selected_id, &chat::conversation::id);
    conversation_selected = found == conversations.end() ? bounded(conversation_selected, conversations.size())
        : static_cast<int>(found - conversations.begin());
    if (!can_send()) { composing = false; editing = 0; reply.reset(); }
}

friendship_state state::friendship(std::int64_t id) const
{
    if (is_contact(id)) { return friendship_state::accepted; }
    for (auto const& request : friends.incoming) { if (request.user.id == id) { return friendship_state::incoming_pending; } }
    for (auto const& request : friends.outgoing) { if (request.user.id == id) { return friendship_state::outgoing_pending; } }
    return friendship_state::none;
}

std::string state::friendship_hint(std::int64_t id) const
{
    switch (friendship(id))
    {
        case friendship_state::outgoing_pending: return "好友申请已发送，等待对方确认";
        case friendship_state::incoming_pending: return "对方已发送好友申请，确认后可继续聊天";
        case friendship_state::accepted: return "正在刷新聊天权限";
        default: return "你们目前不是好友";
    }
}
std::vector<user const*> state::pick_candidates() const
{
    std::vector<user const*> values;
    for (auto const& value : contacts)
    {
        if (value.id != self.id && (friend_matches(value.username, pick_query) ||
            std::ranges::find(picked_contacts, value.id) != picked_contacts.end())) { values.push_back(&value); }
    }
    return values;
}

std::vector<user const*> state::visible_contacts() const
{
    std::vector<user const*> values;
    for (auto const& value : contacts)
    { if (friend_matches(value.username, contacts_query)) { values.push_back(&value); } }
    return values;
}

user const* state::selected_user() const
{
    if (view == page::contacts)
    {
        auto values = visible_contacts();
        return selected > 0 && static_cast<std::size_t>(selected) <= values.size() ? values[selected - 1] : nullptr;
    }
    return view == page::users && selected >= 0 && static_cast<std::size_t>(selected) < users.size() ? &users[selected] : nullptr;
}

void state::apply_contacts(std::vector<user> values)
{
    auto const* selected_contact = selected_user();
    auto selected_id = view == page::contacts && selected_contact ? selected_contact->id : 0;
    // The "find user" row follows the matches, wherever their number moves it.
    bool const on_search = view == page::contacts && !contacts_query.empty() &&
                           selected == static_cast<int>(visible_contacts().size()) + 1;
    if (view == page::pick_contacts)
    {
        auto candidates = pick_candidates();
        if (selected >= 0 && static_cast<std::size_t>(selected) < candidates.size())
        { selected_id = candidates[selected]->id; }
    }
    contacts = std::move(values);
    std::erase_if(picked_contacts, [this](auto id) { return !is_contact(id); });
    if (view == page::contacts)
    {
        auto candidates = visible_contacts();
        auto found = std::ranges::find_if(candidates, [selected_id](auto value) { return value->id == selected_id; });
        selected = on_search ? static_cast<int>(candidates.size()) + 1
            : found != candidates.end() ? static_cast<int>(found - candidates.begin()) + 1
                                        : std::clamp(selected, 0, static_cast<int>(candidates.size()));
    }
    if (view == page::pick_contacts)
    {
        auto candidates = pick_candidates();
        auto found = std::ranges::find_if(candidates, [selected_id](auto value) { return value->id == selected_id; });
        selected = found == candidates.end() ? bounded(selected, candidates.size())
                                            : static_cast<int>(found - candidates.begin());
    }
    std::erase_if(presences, [this](auto const& value) { return !is_contact(value.first); });
}

std::pair<friend_request const*, bool> state::friend_request_at(int index) const
{
    if (index < 0) { return {nullptr, false}; }
    auto const position = static_cast<std::size_t>(index);
    if (position < friends.incoming.size()) { return {&friends.incoming[position], true}; }
    if (position - friends.incoming.size() < friends.outgoing.size())
    { return {&friends.outgoing[position - friends.incoming.size()], false}; }
    return {nullptr, false};
}

void state::apply_friend_requests(friend_requests_result values)
{
    // ":friend-sent" asked for the sent group before the requests were loaded: start there.
    if (focus_sent && (view == page::friend_requests || view == page::friend_sent))
    {
        focus_sent = false;
        friends = std::move(values);
        selected = static_cast<int>(friends.outgoing.empty() ? 0 : friends.incoming.size());
        return;
    }
    focus_sent = false;
    // Keep the same request selected (same person, same direction) across a refresh.
    auto const [old, received] = friend_request_at(selected);
    auto const selected_id = old ? old->user.id : 0;
    friends = std::move(values);
    if (view == page::friend_requests || view == page::friend_sent)
    {
        auto const& requests = received ? friends.incoming : friends.outgoing;
        auto found = std::ranges::find_if(requests, [selected_id](auto const& value) { return value.user.id == selected_id; });
        auto const total = friends.incoming.size() + friends.outgoing.size();
        selected = found == requests.end() || !selected_id ? bounded(selected, total)
            : static_cast<int>(found - requests.begin()) + (received ? 0 : static_cast<int>(friends.incoming.size()));
    }
}

void state::apply_history(messages_result result, bool older)
{
    auto selected_id = message_selected >= 0 && static_cast<std::size_t>(message_selected) < messages.size()
        ? messages[message_selected].id : 0;
    std::optional<std::int64_t> page_before;
    for (auto const& value : result.messages)
    {
        if (!page_before || value.id < *page_before) { page_before = value.id; }
    }
    // A latest-page refresh says nothing about the availability before an
    // already loaded older page. Realtime updates do not move this cursor.
    if (older || !history_before || (page_before && *page_before < *history_before))
    {
        if (page_before) { history_before = page_before; }
        history_more = result.has_more;
    }
    for (auto& value : result.messages) { apply_message(std::move(value)); }
    for (auto& position : result.read_positions)
    {
        auto previous = std::ranges::find(read_positions, position.user, &read_position::user);
        if (previous != read_positions.end()) { position.message = std::max(position.message, previous->message); }
    }
    read_positions = std::move(result.read_positions);
    if (at_latest && !older) { message_selected = bounded(static_cast<int>(messages.size()) - 1, messages.size()); }
    else
    {
        auto found = std::ranges::find(messages, selected_id, &message::id);
        message_selected = found == messages.end() ? bounded(message_selected, messages.size())
            : static_cast<int>(found - messages.begin());
    }
}

void state::apply_search(messages_result result, bool append)
{
    auto selected_id = append && selected >= 0 && static_cast<std::size_t>(selected) < search_results.size()
        ? search_results[selected].id : 0;
    if (!append) { search_results.clear(); search_before.reset(); }
    for (auto const& value : result.messages)
    {
        if (value.conversation == active && (!search_before || value.id < *search_before))
        { search_before = value.id; }
    }
    for (auto& value : result.messages)
    {
        if (value.conversation != active) { continue; }
        // Search snapshots can arrive after an edit, reaction or deletion event.
        // Reconcile with known history without moving its pagination boundary.
        auto known = std::ranges::find(messages, value.id, &message::id);
        if (known != messages.end()) { merge(value, *known); }
        for (auto const& target : messages) { update_quote(value.reply, target); }
        auto current = std::ranges::find(search_results, value.id, &message::id);
        if (current == search_results.end()) { search_results.push_back(std::move(value)); }
        else { merge(*current, value); }
    }
    std::erase_if(search_results, [](auto const& value) { return value.deleted; });
    std::ranges::sort(search_results, std::greater{}, &message::id);
    search_more = result.has_more;
    if (view == page::search)
    {
        auto current = std::ranges::find(search_results, selected_id, &message::id);
        selected = current == search_results.end() ? 0 : static_cast<int>(current - search_results.begin());
    }
}

void state::apply_message(message value)
{
    if (value.conversation != active) { return; }
    auto const selected_search_id = view == page::search && selected >= 0 &&
        static_cast<std::size_t>(selected) < search_results.size() ? search_results[selected].id : 0;
    // The history page lists newest first, so a new message would shift the selection down.
    std::int64_t selected_history_id = 0;
    if (view == page::history)
    {
        auto const entries = history_entries();
        if (selected >= 0 && static_cast<std::size_t>(selected) < entries.size()) { selected_history_id = entries[selected]->id; }
    }
    auto selected_id = message_selected >= 0 && static_cast<std::size_t>(message_selected) < messages.size()
        ? messages[message_selected].id : 0;
    auto found = std::ranges::lower_bound(messages, value.id, {}, &message::id);
    if (found == messages.end() || found->id != value.id) { found = messages.insert(found, std::move(value)); }
    else { merge(*found, value); }
    // Use the merged value so a delayed snapshot cannot undo a tombstone or quote edit.
    auto const& authoritative = *found;
    for (auto& item : messages) { update_quote(item.reply, authoritative); }
    for (auto const& item : messages) { update_quote(found->reply, item); }
    for (auto& item : search_results)
    {
        if (item.id == authoritative.id) { merge(item, authoritative); }
        update_quote(item.reply, authoritative);
    }
    std::erase_if(search_results, [](auto const& item) { return item.deleted; });
    if (view == page::search)
    {
        auto current = std::ranges::find(search_results, selected_search_id, &message::id);
        selected = current == search_results.end() ? bounded(selected, search_results.size())
                                                  : static_cast<int>(current - search_results.begin());
    }
    if (view == page::history)
    {
        auto const entries = history_entries();
        auto current = std::ranges::find(entries, selected_history_id, &message::id);
        selected = current == entries.end() ? bounded(selected, entries.size()) : static_cast<int>(current - entries.begin());
    }
    if (reply && reply->id == authoritative.id)
    {
        if (authoritative.deleted) { reply.reset(); }
        else { update_quote(reply, authoritative); }
    }
    if (editing == authoritative.id && authoritative.deleted) { editing = 0; }
    if (auto* conversation = active_conversation()) { update_quote(conversation->pinned_message, authoritative); }
    if (at_latest) { message_selected = bounded(static_cast<int>(messages.size()) - 1, messages.size()); }
    else
    {
        auto selected_item = std::ranges::find(messages, selected_id, &message::id);
        message_selected = selected_item == messages.end() ? bounded(message_selected, messages.size())
            : static_cast<int>(selected_item - messages.begin());
    }
}

void state::apply_reaction(reaction_update value)
{
    if (value.conversation != active) { return; }
    for (auto* values : {&messages, &search_results})
    {
        auto found = std::ranges::find(*values, value.message, &message::id);
        if (found != values->end() && !found->deleted && value.revision > found->reaction_revision)
        {
            found->reaction_revision = value.revision;
            found->reactions = value.reactions;
        }
    }
}

void state::apply_read(std::int64_t conversation, std::int64_t user, std::int64_t message)
{
    if (conversation != active) { return; }
    auto found = std::ranges::find(read_positions, user, &read_position::user);
    if (found == read_positions.end()) { read_positions.push_back({user, message}); }
    else { found->message = std::max(found->message, message); }
}

void state::select_conversation(std::int64_t id)
{
    active = id;
    view = page::conversation;
    messages.clear();
    read_positions.clear();
    members.clear();
    search_results.clear();
    search_query.clear();
    requests.clear();
    next_requests.reset();
    reply.reset();
    editing = 0;
    composing = false;
    selecting = false;
    draft.clear();
    message_selected = selected = 0;
    history_before.reset();
    search_before.reset();
    history_more = search_more = false;
    at_latest = true;
}

void state::move_message(int delta)
{
    if (view == page::search) { selected = bounded(selected + delta, search_results.size()); return; }
    message_selected = bounded(message_selected + delta, messages.size());
    at_latest = messages.empty() || message_selected == static_cast<int>(messages.size()) - 1;
}

layout_mode state::layout(int width, int height)
{
    if (width < 40 || height < 12) { return layout_mode::too_small; }
    return width < 100 ? layout_mode::narrow : layout_mode::wide;
}
}
