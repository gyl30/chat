#pragma once

#include <chat/conversation.hpp>
#include <chat/member.hpp>
#include <chat/presence.hpp>
#include <unordered_map>

namespace chat::tui
{
enum class connection { signed_out, connecting, authenticating, online, reconnecting };
enum class page { conversations, conversation, contacts, users, profile, members, requests, search, group, help, pick_contacts, copy };
enum class layout_mode { too_small, narrow, wide };

// Mutated exclusively by the UI event loop. SDK callbacks only enqueue payloads.
struct state
{
    connection link = connection::signed_out;
    page view = page::conversations;
    chat::user self;
    std::string status;
    std::vector<chat::conversation> conversations;
    std::optional<conversation_cursor> next_conversations;
    std::vector<user> contacts, users;
    std::unordered_map<std::int64_t, presence> presences;
    std::int64_t active = 0;
    std::vector<message> messages, search_results;
    std::vector<read_position> read_positions;
    std::vector<conversation_member> members;
    std::vector<group_join_request> requests;
    std::optional<std::int64_t> next_requests;
    // Oldest history-page boundary; realtime edits can refer to older messages.
    std::optional<std::int64_t> history_before;
    bool history_more = false, search_more = false, at_latest = true, composing = false;
    std::string draft, search_query;
    std::optional<quoted_message> reply;
    std::int64_t editing = 0;
    int conversation_selected = 0, message_selected = 0, selected = 0;
    user profile;
    std::string copy_text;
    std::vector<std::int64_t> picked_contacts;

    chat::conversation const* active_conversation() const;
    chat::conversation* active_conversation();
    message const* selected_message() const;
    bool can_send() const;
    member_role self_role() const;
    bool is_contact(std::int64_t id) const;
    void apply_conversations(conversations_result result, bool append);
    void apply_contacts(std::vector<user> values);
    void apply_history(messages_result result, bool older);
    void apply_search(messages_result result, bool append);
    void apply_message(message value);
    void apply_reaction(reaction_update value);
    void apply_read(std::int64_t conversation, std::int64_t user, std::int64_t message);
    void select_conversation(std::int64_t id);
    void move_message(int delta);
    static layout_mode layout(int width, int height);
};
}
