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

void check_search_live_selection()
{
    auto search = [] {
        state value;
        value.select_conversation(7);
        value.view = page::search;
        value.search_query = "hello";
        value.draft = "未发送 Unicode 草稿 é 👩‍💻";
        value.apply_search({{msg(30), msg(20), msg(10)}, {}, true}, false);
        value.selected = 1;
        value.history_before = 80;
        return value;
    };
    auto deleted = [](std::int64_t id) {
        auto value = msg(id, "");
        value.deleted = true;
        return value;
    };
    for (auto id : {30, 10})
    {
        auto value = search();
        value.apply_message(deleted(id));
        check(value.selected_message() && value.selected_message()->id == 20 && value.search_results.size() == 2,
              id == 30 ? "Deleting before search selection preserves selected message identity"
                       : "Deleting after search selection preserves selected message identity");
        check(value.draft == "未发送 Unicode 草稿 é 👩‍💻" && value.search_query == "hello" &&
              value.search_before == 10 && value.search_more && value.history_before == 80,
              "Live search deletion preserves draft, query and independent page cursors");
    }
    {
        auto value = search();
        value.apply_message(deleted(20));
        check(value.selected == 1 && value.selected_message() && value.selected_message()->id == 10,
              "Deleting the selected search message chooses its remaining row neighbor");
        value.selected = 1;
        value.apply_message(deleted(10));
        check(value.selected == 0 && value.selected_message() && value.selected_message()->id == 30,
              "Deleting the selected last search message clamps to its preceding neighbor");
        value.apply_message(deleted(30));
        check(value.selected == 0 && value.search_results.empty() && !value.selected_message() &&
              value.draft == "未发送 Unicode 草稿 é 👩‍💻" && value.search_before == 10 && value.search_more,
              "Deleting the final search message leaves a safe empty selection and preserves draft and cursor");
    }
    {
        auto value = search();
        value.apply_message(deleted(30));
        auto edit = msg(20, "edited body no longer matching");
        edit.edited_at = 200;
        value.apply_message(edit);
        // The pending older page contains only IDs below its cursor10. A live
        // tombstone can arrive for one of those IDs before that page completes.
        value.apply_message(deleted(5));
        value.apply_search({{msg(5), msg(2)}, {}, false}, true);
        check(value.selected_message() && value.selected_message()->id == 20 &&
              value.selected_message()->text == "edited body no longer matching" &&
              value.search_results.size() == 3 && value.search_results[0].id == 20 &&
              value.search_results.back().id == 2 && value.search_before == 2 &&
              !value.search_more && value.history_before == 80,
              "A delayed older page preserves live selected identity, newer body and tombstone without moving history cursor");
        check(value.draft == "未发送 Unicode 草稿 é 👩‍💻" && value.search_query == "hello",
              "Delayed search page preserves the unsent draft and query");
    }
    {
        auto value = search();
        value.view = page::contacts;
        value.selected = 6;
        value.apply_message(deleted(30));
        check(value.selected == 6 && value.search_results.size() == 2 &&
              value.draft == "未发送 Unicode 草稿 é 👩‍💻",
              "Background search deletion does not change selection on a non-search page");
    }
    {
        auto value = search();
        auto other = deleted(30);
        other.conversation = 99;
        value.apply_message(other);
        check(value.selected == 1 && value.selected_message()->id == 20 && value.search_results.size() == 3,
              "An unrelated-conversation deletion cannot alter search selection");
        value.selected = -1;
        value.apply_message(msg(20));
        check(value.selected == 0 && value.selected_message()->id == 30,
              "An invalid prior search selection clamps without reading outside the vector");
    }
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
        s.friends.outgoing = {{{2, "peer", {}}, 1}};
        check(s.friendship(2) == chat::friendship_state::outgoing_pending && !s.is_contact(2), "Outgoing request is not a friend");
        s.friends.outgoing.clear(); s.friends.incoming = {{{2, "peer", {}}, 1}};
        check(s.friendship(2) == chat::friendship_state::incoming_pending && !s.is_contact(2), "Incoming request is not a friend");
        s.apply_contacts({{2, "peer", {}}});
        check(s.friendship(2) == chat::friendship_state::accepted, "Accepted contacts are authoritative");
        s.friends.incoming = {{{3, "pending incoming", {}}, 1}};
        s.friends.outgoing = {{{4, "pending outgoing", {}}, 2}};
        s.apply_contacts({{2, "peer", {}}});
        check(s.contacts.size() == 1 && s.contacts.front().id == 2 && !s.is_contact(3) && !s.is_contact(4),
              "Pending snapshots do not populate accepted contacts");
        s.view = page::conversations;
        auto history_count = s.conversations.size();
        s.apply_conversations({{convo(9), convo(7, false)}, {}}, false);
        check(s.view == page::conversations && s.active == 7 && s.conversations.size() == history_count,
              "Read-only refresh preserves Chats navigation and historical direct");
        s.view = page::contacts;
        s.apply_contacts({{2, "peer", {}}, {5, "another friend", {}}});
        s.selected = 2;
        s.apply_contacts({{5, "another friend", {}}, {2, "peer", {}}});
        check(s.selected == 1, "Contact refresh preserves selected friend identity");
        s.apply_contacts({{2, "peer", {}}});
        check(s.selected == 1, "Removing selected friend leaves a valid accepted row");
        s.apply_contacts({});
        check(s.selected == 0, "Empty contacts selects New friends entry");
        s.apply_contacts({{2, "peer", {}}});
        s.pick_query = "peer";
        check(s.pick_candidates().size() == 1, "Pick accepted matching friend");
        s.pick_query = "missing";
        check(s.pick_candidates().empty(), "Group picker filters without extra snapshot");
        {
            state local;
            local.self.id = 1;
            local.view = page::contacts;
            local.apply_contacts({{2, "Alice Bob", {}}, {3, "张 三", {}}, {4, "a.b", {}}, {5, "Älice", {}}});
            local.friends.incoming = {{{6, "Pending Bob", {}}, 1}};
            local.friends.outgoing = {{{7, "Outgoing Bob", {}}, 1}};
            local.users = {{8, "Stranger Bob", {}}};
            for (auto const& [query, id] : std::vector<std::pair<std::string, std::int64_t>>{{"BOB", 2}, {"三", 3}, {".b", 4}, {"Äli", 5}})
            {
                local.contacts_query = local.pick_query = query;
                check(local.visible_contacts().size() == 1 && local.visible_contacts()[0]->id == id,
                      "Contacts searches accepted friends using literal Unicode and ASCII-insensitive substring");
                check(local.pick_candidates().size() == 1 && local.pick_candidates()[0]->id == id,
                      "Contacts and picker use the same local matching rule");
                local.selected = 1;
                check(local.selected_user()->id == id, "Visible filtered selection targets the matching user");
            }
            local.contacts_query = local.pick_query = "äli";
            check(local.visible_contacts().empty() && local.pick_candidates().empty(), "TUI does not pretend to fold non-ASCII case");
            local.contacts_query = "Bob";
            local.selected = 1;
            local.apply_contacts({{3, "张 三", {}}, {2, "Alice Bob", {}}});
            check(local.selected_user()->id == 2, "Contacts refresh preserves identity within filtered rows");
            local.apply_contacts({{3, "张 三", {}}});
            check(local.selected == 0 && !local.selected_user(), "Empty filtered Contacts keeps the New friends entry selected");
            local.contacts_query.clear();
            check(local.visible_contacts().size() == 1, "Clear filter restores accepted friends only");
            check(local.friendship_hint(8) == "你们目前不是好友" &&
                  local.friendship_hint(6) == "对方已发送好友申请，确认后可继续聊天" &&
                  local.friendship_hint(7) == "好友申请已发送，等待对方确认" &&
                  local.friendship_hint(3) == "正在刷新聊天权限", "Read-only hints distinguish all authoritative relationship states");
        }
        for (auto view : {page::friend_requests, page::friend_sent})
        {
            state requests;
            requests.view = view;
            auto snapshot = [view](std::vector<friend_request> entries) {
                friend_requests_result value;
                (view == page::friend_requests ? value.incoming : value.outgoing) = std::move(entries);
                return value;
            };
            requests.apply_friend_requests(snapshot({{{2, "Alice", {}}, 1}, {{3, "Bob", {}}, 2}, {{4, "Carol", {}}, 3}}));
            requests.selected = 1;
            requests.apply_friend_requests(snapshot({{{3, "Bob", {}}, 2}, {{4, "Carol", {}}, 3}}));
            check(requests.selected == 0, "Request removed before selection preserves Bob in both tabs");
            requests.apply_friend_requests(snapshot({{{4, "Carol", {}}, 3}, {{3, "Bob", {}}, 2}}));
            check(requests.selected == 1, "Request reorder preserves selected user");
            requests.apply_friend_requests(snapshot({{{4, "Carol", {}}, 3}}));
            check(requests.selected == 0, "Removing selected request clamps to remaining user");
            requests.apply_friend_requests(snapshot({}));
            check(requests.selected == 0, "Empty requests resets selection in both tabs");
        }
        for (auto const& query : {std::string{}, std::string{"friend"}})
        {
            state picker;
            picker.self.id = 1;
            picker.view = page::pick_contacts;
            picker.pick_query = query;
            picker.apply_contacts({{2, "Alice friend", {}}, {3, "Bob friend", {}}, {4, "Carol friend", {}}});
            picker.selected = 1;
            picker.apply_contacts({{3, "Bob friend", {}}, {4, "Carol friend", {}}});
            check(picker.pick_candidates()[picker.selected]->id == 3, "Picker removal before selection preserves user identity");
            picker.apply_contacts({{4, "Carol friend", {}}, {3, "Bob friend", {}}});
            check(picker.selected == 1, "Picker reorder preserves selected user");
            picker.picked_contacts = {3};
            picker.apply_contacts({{4, "Carol friend", {}}});
            check(picker.selected == 0 && picker.pick_candidates()[0]->id == 4 && picker.picked_contacts.empty(),
                  "Removed selected friend leaves a valid candidate and clears picked identity");
            picker.apply_contacts({});
            check(picker.selected == 0 && picker.pick_candidates().empty(), "Empty picker resets selection");
        }
        {
            state picker;
            picker.view = page::pick_contacts;
            picker.apply_contacts({{2, "match first", {}}, {3, "hidden", {}}, {4, "match last", {}}});
            picker.pick_query = "match";
            picker.picked_contacts = {3};
            picker.selected = 1;
            picker.apply_contacts({{4, "match last", {}}, {3, "hidden", {}}});
            check(picker.pick_candidates()[picker.selected]->id == 3, "Filtered picked friend remains selected and cancellable");
            picker.picked_contacts.clear();
            picker.selected = 0;
            picker.apply_contacts({{3, "hidden", {}}});
            check(picker.pick_candidates().empty() && picker.selected == 0, "Empty filtered candidates reset selection");
        }
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
        s.apply_history({{msg(20), msg(30)}, {{1, 10}, {2, 20}}, true}, false);
        check(!s.history_more && s.history_before == 1, "Latest refresh preserves exhausted older-page cursor");
        check(s.read_positions[1].message == 30, "Late history snapshot cannot regress read position");
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
        check(!s.history_before, "Conversation switch clears history cursor");
        state pages;
        pages.select_conversation(7);
        auto old_update = msg(1);
        old_update.deleted = true;
        pages.apply_message(old_update);
        pages.apply_history({{msg(80), msg(90)}, {}, true}, false);
        check(pages.history_before == 80 && pages.messages.front().id == 1,
              "Realtime old-message update cannot skip unloaded history");
        pages.apply_history({{msg(40), msg(60)}, {}, true}, true);
        check(pages.history_before == 40 && pages.history_more, "Older page advances server history boundary");
        pages.apply_history({{msg(80), msg(90)}, {}, true}, false);
        check(pages.history_before == 40 && pages.history_more, "Latest refresh preserves unfinished older boundary");
        pages.apply_history({{msg(1), msg(20)}, {}, false}, true);
        check(pages.history_before == 1 && !pages.history_more && pages.messages.front().deleted,
              "Delayed history preserves earlier deletion and reaches oldest page");
        state searched;
        searched.select_conversation(7);
        searched.view = page::search;
        auto tombstone = msg(10);
        tombstone.deleted = true;
        searched.apply_message(tombstone);
        auto authoritative = msg(20, "new text");
        authoritative.edited_at = 200;
        authoritative.reaction_revision = 5;
        authoritative.reactions = {{"👍", {1}}};
        searched.apply_message(authoritative);
        auto quoted = msg(30);
        quoted.reply = quoted_message{10, 1, "Alice", "old quote", {}, false};
        searched.apply_search({{msg(10), msg(20, "old text"), quoted, msg(20)}, {}, true}, false);
        check(searched.search_results.size() == 2 && searched.search_results[0].id == 30 &&
              searched.search_results[1].id == 20 && searched.search_more,
              "Search deduplicates descending results and does not resurrect tombstones");
        check(searched.search_results[1].text == "new text" && searched.search_results[1].reaction_revision == 5 &&
              searched.search_results[0].reply->deleted,
              "Search reconciles current edits, reactions and quote deletion");
        searched.selected = 1;
        searched.apply_search({{msg(5), msg(20, "stale")}, {}, false}, true);
        check(searched.search_results.size() == 3 && searched.search_results[searched.selected].id == 20 &&
              !searched.search_more && searched.search_results[1].text == "new text",
              "Search older page preserves selected identity and newer revisions");
        check(searched.messages.size() == 2 && !searched.history_before,
              "Search never inserts its results into loaded history");
        searched.apply_search({{}, {}, false}, false);
        check(searched.search_results.empty() && searched.selected == 0 && !searched.search_before,
              "Empty new search resets selection and cursor");
        searched.apply_search({{msg(10)}, {}, true}, false);
        check(searched.search_results.empty() && searched.search_more && searched.search_before == 10,
              "Entire search page filtered as deleted still retains its server cursor");
        searched.apply_search({{msg(5)}, {}, false}, true);
        check(searched.search_results.size() == 1 && searched.search_results[0].id == 5 && searched.search_before == 5,
              "Older search page remains accessible after filtered page");
        searched.select_conversation(99);
        check(!searched.search_before, "Conversation switch clears search cursor");
        check_search_live_selection();
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
