#include "state.hpp"

#include <algorithm>
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

void state::apply_contacts(std::vector<user> values)
{
    contacts = std::move(values);
    std::erase_if(presences, [this](auto const& value) { return !is_contact(value.first); });
}

void state::apply_history(messages_result result, bool older)
{
    auto selected_id = message_selected >= 0 && static_cast<std::size_t>(message_selected) < messages.size()
        ? messages[message_selected].id : 0;
    for (auto& value : result.messages) { apply_message(std::move(value)); }
    read_positions = std::move(result.read_positions);
    history_more = result.has_more;
    if (at_latest && !older) { message_selected = bounded(static_cast<int>(messages.size()) - 1, messages.size()); }
    else
    {
        auto found = std::ranges::find(messages, selected_id, &message::id);
        message_selected = found == messages.end() ? bounded(message_selected, messages.size())
            : static_cast<int>(found - messages.begin());
    }
}

void state::apply_message(message value)
{
    if (value.conversation != active) { return; }
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
    if (view == page::search) { selected = bounded(selected, search_results.size()); }
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
    draft.clear();
    message_selected = selected = 0;
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
