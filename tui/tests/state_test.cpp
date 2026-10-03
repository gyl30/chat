#include "state.hpp"

#include <iostream>
#include <stdexcept>

using namespace chat;
using namespace chat::tui;

namespace
{
void check(bool value, char const* description)
{
    if (!value) { throw std::runtime_error(description); }
}
message msg(std::int64_t id, std::string text = "hello")
{
    message value;
    value.id = id;
    value.conversation = 7;
    value.from = 1;
    value.username = "张 三";
    value.text = std::move(text);
    return value;
}
conversation convo(std::int64_t id, bool send = true)
{
    conversation value;
    value.id = id;
    value.username = "Alice Bob";
    value.can_send = send;
    return value;
}
}

int main()
{
    try
    {
        state s;
        s.self.id = 1;
        s.apply_conversations({{convo(7), convo(3)}, conversation_cursor{10, 3, false}}, false);
        s.select_conversation(7);
        check(!s.can_send(), "Offline cannot compose");
        s.link = connection::online;
        check(s.can_send(), "Authoritative can_send enables compose");
        s.composing = true;
        s.draft = "Unicode 草稿";
        s.apply_conversations({{convo(7, false), convo(3)}, {}}, false);
        check(!s.can_send() && !s.composing && s.draft == "Unicode 草稿", "Permission revoke preserves draft");
        s.apply_conversations({{convo(3), convo(9)}, {}}, true);
        check(s.conversations.size() == 3 && s.conversations[0].id == 7 && s.conversations[1].id == 3 &&
              s.conversations[2].id == 9, "Pagination deduplicates without sorting server order");
        s.conversation_selected = 2;
        s.apply_conversations({{convo(9), convo(7)}, {}}, false);
        check(s.conversation_selected == 0, "Conversation refresh preserves selected identity");
        s.apply_contacts({{1, "self", {}}, {2, "peer", {}}});
        s.presences[2] = {2, true, 0};
        s.presences[3] = {3, true, 0};
        s.apply_contacts({{1, "self", {}}});
        check(s.presences.empty() && !s.is_contact(2), "Presence filtered after contact removal");
        s.members = {{1, "self", member_role::owner, {}}};
        check(s.self_role() == member_role::owner, "Role derives from current members");
        s.apply_history({{msg(10), msg(20)}, {{1, 10}}, true}, false);
        check(s.messages.size() == 2 && s.message_selected == 1 && s.at_latest && s.history_more, "Initial history selects latest");
        s.move_message(-1);
        check(!s.at_latest && s.selected_message()->id == 10, "Browsing old message leaves latest area");
        s.apply_message(msg(30));
        check(!s.at_latest && s.selected_message()->id == 10, "Incoming message preserves old selection");
        s.apply_history({{msg(1), msg(10)}, {{1, 10}, {2, 20}}, false}, true);
        check(s.messages.size() == 4 && s.selected_message()->id == 10 && !s.history_more,
              "Older pagination preserves selection and removes duplicates");
        s.apply_read(7, 2, 30);
        s.apply_read(7, 2, 1);
        s.apply_read(99, 2, 99);
        check(s.read_positions[1].message == 30, "Read events are monotonic and conversation scoped");
        auto edit = msg(10, "edited");
        edit.edited_at = 100;
        s.apply_message(edit);
        s.apply_message(msg(10, "stale"));
        check(s.selected_message()->text == "edited", "Late history cannot revert edit");
        s.apply_reaction({7, 10, 5, {{"👍", {1}}}});
        s.apply_reaction({7, 10, 3, {{"❤️", {2}}}});
        auto later_edit = edit;
        later_edit.edited_at = 200;
        later_edit.reaction_revision = 1;
        s.apply_message(later_edit);
        check(s.selected_message()->reaction_revision == 5 && s.selected_message()->reactions[0].emoji == "👍",
              "Stale reaction and edit snapshot cannot revert reaction revision");
        auto reply_message = msg(40, "reply");
        reply_message.reply = quoted_message{10, 1, "张 三", "old quote", {}, false};
        s.apply_message(reply_message);
        check(s.messages.back().reply->text == "edited", "Loaded target refreshes newly loaded quote");
        s.reply = quoted_message{10, 1, "张 三", "edited", 200, false};
        s.editing = 10;
        s.search_results = {edit};
        s.selected = 6;
        auto deleted = msg(10, "");
        deleted.deleted = true;
        s.apply_message(deleted);
        check(!s.reply && s.editing == 0 && s.draft == "Unicode 草稿", "Deleted reply target cancels action and preserves draft");
        check(s.selected == 6, "Incoming messages do not change unrelated page selection");
        check(s.messages.back().reply->deleted && s.search_results.empty(), "Deletion updates historical quotes and removes search result");
        s.apply_message(later_edit);
        s.apply_reaction({7, 10, 50, {{"🎉", {2}}}});
        check(s.selected_message()->deleted, "Tombstone cannot be resurrected");
        auto stale = msg(50);
        stale.conversation = 99;
        s.apply_message(stale);
        check(s.messages.size() == 5, "Stale conversation event cannot leak into active history");
        s.move_message(100);
        check(s.at_latest && s.selected_message()->id == 40, "Jump latest clamps selection");
        s.view = page::search;
        s.search_results = {msg(80)};
        s.selected = 0;
        check(s.selected_message()->id == 80, "Search selection uses search result");
        s.select_conversation(99);
        check(s.messages.empty() && s.members.empty() && s.search_results.empty() && s.read_positions.empty() &&
              !s.reply && !s.editing && !s.composing, "Conversation switch clears scoped state");
        check(state::layout(39, 24) == layout_mode::too_small && state::layout(120, 11) == layout_mode::too_small &&
              state::layout(60, 20) == layout_mode::narrow && state::layout(80, 24) == layout_mode::narrow &&
              state::layout(100, 30) == layout_mode::wide && state::layout(120, 40) == layout_mode::wide,
              "Layout thresholds cover terminal sizes");
        std::cout << "PASS TUI state permissions, pagination, revisions, quotes, navigation and resize\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL TUI state: " << error.what() << "\n";
        return 1;
    }
}
