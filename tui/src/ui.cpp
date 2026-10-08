#include "ui.hpp"
#include "app.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <string_view>
#include <utility>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

namespace chat::tui
{
namespace
{
using namespace ftxui;
struct shortcut { std::string_view key, description; };
constexpr std::array shortcuts{
    shortcut{"j / Down, k / Up", "Move selection"},
    shortcut{"Enter", "Open selected item / confirm"},
    shortcut{"Esc", "Back / close prompt / keep draft"},
    shortcut{"Tab / Shift+Tab", "Switch conversations and messages / input focus"},
    shortcut{"i", "Compose (Enter sends, Esc keeps draft)"},
    shortcut{"r / e / d", "Reply / edit / confirm delete"},
    shortcut{"a", "Reaction picker (0 removes)"},
    shortcut{"y / s", "Show copyable message / save attachment"},
    shortcut{"/", "Search current conversation"},
    shortcut{"PgUp / PgDn", "Earlier history / next search or request page"},
    shortcut{"G", "Latest message"},
    shortcut{"[ / ]", "Scroll within selected long message"},
    shortcut{"m / p", "Mute / personal pin"},
    shortcut{"h / c / u", "Chats / Contacts / Account"},
    shortcut{"N (Shift+n)", "New: add friend / create group / join group"},
    shortcut{"g", "Current group actions"},
    shortcut{"A / O / D (members)", "Admin / transfer owner / remove member"},
    shortcut{"y / n (requests)", "Accept / reject selected request"},
    shortcut{"Tab (new friends)", "Switch incoming / outgoing requests"},
    shortcut{"/ (contacts / picker)", "Filter accepted friends by name"},
    shortcut{"Space / Enter (contacts picker)", "Toggle member / finish selection"},
    shortcut{":", "Command input (:help for command list)"},
    shortcut{"?", "Help"},
    shortcut{"Ctrl+C / :quit", "Safe exit"},
};
struct menu_action { std::string label, command; };
std::vector<menu_action> actions(state const& s)
{
    if (s.view == page::new_action)
    { return {{"Add friend", "add-contact"}, {"Create group", "create-group"}, {"Join group", "join"}}; }
    if (s.view == page::profile)
    {
        std::vector<menu_action> items{{"Show copyable username", "copy-user"}};
        if (s.profile.id == s.self.id)
        {
            items.push_back({"Set avatar from PNG/JPEG path", "avatar"});
            items.push_back({"Clear avatar", "avatar-clear"});
            items.push_back({"Log out", "logout"});
        }
        else if (s.is_contact(s.profile.id))
        {
            items.push_back({"Message", "message"});
            items.push_back({"Remove friend", "remove-contact"});
        }
        else if (s.friendship(s.profile.id) == friendship_state::outgoing_pending)
        { items.push_back({"Cancel friend request", "cancel-friend"}); }
        else if (s.friendship(s.profile.id) == friendship_state::incoming_pending)
        {
            items.push_back({"Accept friend request", "accept-friend"});
            items.push_back({"Reject friend request", "reject-friend"});
        }
        else { items.push_back({"Add friend", "add"}); }
        return items;
    }
    if (s.view != page::group) { return {}; }
    auto c = s.active_conversation();
    if (!c || c->kind != conversation_kind::group) { return {}; }
    std::vector<menu_action> items{{"All members", "members"}};
    if (!c->announcement.empty()) { items.push_back({"Show full announcement", "show-announcement"}); }
    if (c->pinned_message) { items.push_back({"View pinned message", "pinned"}); }
    if (s.self_role() != member_role::member)
    {
        items.push_back({"Invite accepted friends", "invite"});
        items.push_back({"Rename group", "rename"});
        items.push_back({"Edit / clear announcement", "announcement"});
        items.push_back({"Pin selected message", "pin-message"});
        if (c->pinned_message) { items.push_back({"Unpin group message", "unpin-message"}); }
        items.push_back({"Show invite code", "link"});
        items.push_back({"Generate invite code", "link-create"});
        items.push_back({"Revoke invite code", "link-revoke"});
        items.push_back({c->join_approval ? "Disable join approval" : "Enable join approval", "approval"});
        items.push_back({"Pending join requests", "requests"});
    }
    if (s.self_role() != member_role::owner) { items.push_back({"Leave group", "leave"}); }
    return items;
}
std::string first_glyph(std::string const& name)
{
    auto glyphs = Utf8ToGlyphs(name);
    return glyphs.empty() ? "?" : glyphs.front();
}
Element wrapped_text(std::string const& value, int width)
{
    width = std::max(1, width);
    std::string output;
    int column = 0;
    // Use FTXUI's glyphs and terminal cell widths for wrapping.
    for (auto const& glyph : Utf8ToGlyphs(value))
    {
        if (glyph.empty()) { continue; } // reserved second cell of a wide glyph
        if (glyph == "\n" || glyph == "\r\n") { output += glyph; column = 0; continue; }
        auto const cells = DisplayWidth(glyph);
        if (column && column + cells > width) { output += '\n'; column = 0; }
        output += glyph;
        column += cells;
    }
    return text(output);
}
Element help_content(int width, int* line_count = nullptr)
{
    Elements rows;
    if (line_count) { *line_count = 1; } // The separator contributes one row.
    auto wrap = [width, line_count, &rows](std::string_view value) {
        std::string output;
        int column = 0;
        int lines = 1;
        std::size_t start = 0;
        while (start < value.size())
        {
            auto const end = value.find(' ', start);
            auto const word = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
            if (!word.empty())
            {
                auto const cells = DisplayWidth(word);
                if (column && column + 1 + cells > width)
                { if (!line_count) { output += '\n'; } column = 0; ++lines; }
                else if (column) { if (!line_count) { output += ' '; } ++column; }
                if (!line_count) { output += word; }
                column += cells;
            }
            if (end == std::string_view::npos) { break; }
            start = end + 1;
        }
        if (line_count) { *line_count += lines; return; }
        rows.push_back(text(output));
    };
    for (auto const& shortcut : shortcuts)
    { wrap(std::string(shortcut.key) + "  " + std::string(shortcut.description)); }
    if (!line_count) { rows.push_back(separator()); }
    wrap("Commands: new, chats, contacts, friend-requests, friend-sent, accept-friend, reject-friend, cancel-friend, filter, add-contact, profile, account, create-group, join, file, save, members, invite, rename, announcement, show-announcement, pinned, pin-message, unpin-message, link, link-create, link-revoke, approval, requests, avatar, avatar-clear, logout, quit");
    wrap("Clipboard: copyable text page; select with your terminal.");
    return line_count ? Element{} : vbox(std::move(rows));
}
Element preview_text(std::string value, int width)
{
    // Headers and quotes are previews: embedded line breaks must not consume
    // the history or composer. Full text remains available on the copy page.
    for (char& c : value) { if (c == '\n' || c == '\r' || c == '\t') { c = ' '; } }
    width = std::max(1, width);
    if (DisplayWidth(value) > width)
    {
        std::string clipped;
        int column = 0;
        for (auto const& glyph : Utf8ToGlyphs(value))
        {
            if (glyph.empty()) { continue; }
            auto cells = DisplayWidth(glyph);
            if (column + cells >= width) { break; }
            clipped += glyph;
            column += cells;
        }
        value = std::move(clipped) + "…";
    }
    return text(value) | size(HEIGHT, EQUAL, 1);
}
std::string user_label(std::string const& name) { return "[" + first_glyph(name) + "] " + name; }
std::string timestamp(std::int64_t value)
{
    if (value <= 0) { return {}; }
    // Protocol timestamps are Unix milliseconds.
    const auto seconds = static_cast<std::time_t>(value / 1000);
    std::tm time{};
    localtime_r(&seconds, &time);
    std::array<char, 32> output{};
    std::strftime(output.data(), output.size(), "%m-%d %H:%M", &time);
    return output.data();
}
std::string presence_label(state const& s, std::int64_t id)
{
    if (!s.is_contact(id)) { return {}; }
    auto p = s.presences.find(id);
    if (p == s.presences.end()) { return {}; }
    if (p->second.online) { return "online"; }
    return p->second.last_seen ? "last seen " + timestamp(p->second.last_seen) : "offline";
}
std::string role_label(member_role role)
{
    switch (role) { case member_role::owner: return "owner"; case member_role::admin: return "admin"; default: return "member"; }
}
std::string link_label(connection link)
{
    switch (link)
    {
        case connection::online: return "connected";
        case connection::connecting: return "connecting…";
        case connection::authenticating: return "authenticating…";
        case connection::reconnecting: return "正在重连…";
        default: return "signed out";
    }
}
Element selected(Element item, bool value, bool active = true)
{
    if (!value) { return item; }
    return active ? item | inverted | focus : item | focus;
}
Element scroll(Elements items)
{
    if (items.empty()) { items.push_back(text("No items")); }
    return vbox(std::move(items)) | vscroll_indicator | yframe | flex;
}
Element conversation_list(state const& s, int width)
{
    Elements rows;
    if (s.conversations.empty())
    {
        rows.push_back(text("No chats to display"));
        rows.push_back(text("N: new chat options") | dim);
    }
    for (std::size_t i = 0; i < s.conversations.size(); ++i)
    {
        auto const& c = s.conversations[i];
        auto label = user_label(c.username);
        if (c.kind == conversation_kind::direct)
        {
            auto presence = presence_label(s, c.user);
            if (!presence.empty()) { label += " · " + presence; }
        }
        auto unread = c.unread ? " (" + std::to_string(c.unread) + ")" : std::string{};
        std::string flags;
        if (c.pinned) { flags += " [pin]"; }
        if (c.muted) { flags += " [mute]"; }
        auto summary = c.last.deleted ? "消息已删除" : c.last.attachment ? "[文件] " + c.last.attachment->filename : c.last.text;
        rows.push_back(selected(vbox({
            hbox({preview_text(label, width - DisplayWidth(unread)) | flex, text(unread)}),
            hbox({preview_text(summary, width - DisplayWidth(flags)) | dim | flex, text(flags)})}),
            s.conversation_selected == static_cast<int>(i), s.view == page::conversations));
    }
    if (s.next_conversations) { rows.push_back(text("↓ More conversations")); }
    return vbox({text("Chats") | bold, separator(), scroll(std::move(rows))}) | flex;
}
Element message_item(state const& s, message const& m, bool highlighted, int width, int scroll_line)
{
    auto const content_width = std::max(1, width * 3 / 4);
    std::string heading = m.from == s.self.id ? "You" : m.username;
    heading += " " + timestamp(m.timestamp);
    if (m.edited_at && !m.deleted) { heading += " (edited)"; }
    Elements lines{preview_text(heading, content_width) | bold};
    if (m.reply)
    {
        lines.push_back(preview_text("↪ " + m.reply->username + ": " + (m.reply->deleted ? "消息已删除" : m.reply->text), content_width) | dim);
    }
    if (m.deleted) { lines.push_back(text("消息已删除") | dim); }
    else
    {
        if (!m.text.empty()) { lines.push_back(wrapped_text(m.text, content_width)); }
        if (m.attachment)
        {
            auto const& a = *m.attachment;
            lines.push_back(text(std::string(a.media_type.starts_with("image/") ? "[图片] " : "[文件] ") + a.filename + " · " + std::to_string(a.size) + " bytes"));
        }
        std::string reaction_text;
        for (auto const& reaction : m.reactions)
        {
            reaction_text += reaction.emoji + " " + std::to_string(reaction.users.size());
            if (std::ranges::find(reaction.users, s.self.id) != reaction.users.end()) { reaction_text += "*"; }
            reaction_text += "  ";
        }
        if (!reaction_text.empty()) { lines.push_back(text(reaction_text)); }
        if (!m.mentions.empty())
        {
            std::string mentions = "Mentions:";
            for (auto const& value : m.mentions) { mentions += " @" + value.username; }
            lines.push_back(text(mentions) | dim);
        }
    }
    if (m.from == s.self.id)
    {
        auto c = s.active_conversation();
        auto read_count = std::ranges::count_if(s.read_positions, [&](auto const& p) {
            if (p.user == s.self.id || p.message < m.id) { return false; }
            return !c || c->kind != conversation_kind::group ||
                std::ranges::find(s.members, p.user, &conversation_member::id) != s.members.end();
        });
        lines.push_back(text(c && c->kind == conversation_kind::group ? "已读 " + std::to_string(read_count) + " 人" : read_count ? "✓✓" : "✓") | dim);
    }
    auto item = vbox(std::move(lines)) | size(WIDTH, LESS_THAN, content_width);
    if (highlighted)
    {
        item->ComputeRequirement();
        auto const last_line = std::max(1, item->requirement().min_y - 1);
        float position = scroll_line < 0 ? 1.f : static_cast<float>(std::clamp(scroll_line, 0, last_line)) / last_line;
        if (s.view == page::conversation || s.view == page::search) { item = item | inverted; }
        item = item | focusPositionRelative(0.f, position);
    }
    return m.from == s.self.id ? hbox({filler(), item}) : hbox({item, filler()});
}
Element history(state const& s, int width, int message_scroll)
{
    Elements items;
    if (s.messages.empty()) { items.push_back(text("No messages to display") | dim); }
    if (s.history_more) { items.push_back(text("PgUp: load earlier history") | dim); }
    for (std::size_t i = 0; i < s.messages.size(); ++i)
    {
        items.push_back(message_item(s, s.messages[i], s.message_selected == static_cast<int>(i), width, message_scroll));
        items.push_back(text(""));
    }
    return scroll(std::move(items));
}
Element conversation_view(state const& s, Element input, std::string typing, int width, int message_scroll)
{
    auto c = s.active_conversation();
    if (!c) { return text("Select a conversation and press Enter") | center | flex; }
    auto detail = c->kind == conversation_kind::group ? " · " + std::to_string(c->member_count) + " members" : presence_label(s, c->user);
    if (c->kind == conversation_kind::direct && !detail.empty()) { detail = " · " + detail; }
    Elements items{hbox({preview_text(c->username, width - DisplayWidth(detail)) | bold | flex, text(detail) | dim})};
    if (c->pinned_message) { items.push_back(preview_text("Pinned: " + (c->pinned_message->deleted ? "消息已删除" : c->pinned_message->text), width) | dim); }
    if (!c->announcement.empty()) { items.push_back(preview_text("公告: " + c->announcement, width) | dim); }
    items.push_back(separator());
    items.push_back(history(s, width, message_scroll));
    if (!typing.empty()) { items.push_back(text(typing) | dim); }
    if (!s.at_latest) { items.push_back(text("Browsing history · G: latest (new messages stay unread)") | dim); }
    items.push_back(separator());
    if (!s.can_send())
    {
        std::string hint = "当前会话不可发送消息";
        if (s.link != connection::online) { hint = "Waiting for connection…"; }
        else if (c->kind == conversation_kind::direct)
        {
            hint = s.friendship_hint(c->user);
        }
        items.push_back(wrapped_text(hint, width));
    }
    else
    {
        if (s.reply) { items.push_back(preview_text("Reply " + s.reply->username + ": " + s.reply->text, width) | dim); }
        if (s.editing) { items.push_back(text("Editing message · Esc: keep draft") | dim); }
        items.push_back(input ? input | size(HEIGHT, EQUAL, 1) : preview_text(s.composing ? "> " + s.draft : s.draft.empty() ? "i: compose" : "i: compose · " + s.draft, width));
    }
    return vbox(std::move(items)) | flex;
}
Element secondary(state const& s, int width, int message_scroll)
{
    Elements rows;
    std::string title;
    std::string hint;
    switch (s.view)
    {
        case page::new_action:
        {
            title = "New";
            auto items = actions(s);
            for (std::size_t i = 0; i < items.size(); ++i)
            { rows.push_back(selected(text(items[i].label), s.selected == static_cast<int>(i))); }
            break;
        }
        case page::contacts:
        case page::users:
        case page::pick_contacts:
        {
            title = s.view == page::contacts ? "Contacts" : s.view == page::users ? "User search" : "Choose friends";
            if (s.view == page::users && !s.users.empty()) { hint = "Enter: profile"; }
            if (s.view == page::pick_contacts) { hint = "Space: toggle · Enter: next"; }
            if (s.view == page::users && s.users.empty()) { rows.push_back(text("No users match this search") | dim); }
            if (s.view == page::contacts && s.contacts.empty()) { rows.push_back(text("No accepted friends yet") | dim); }
            if (s.view == page::contacts) { rows.push_back(preview_text("/: search · " + s.contacts_query, width) | dim); }
            if (s.view == page::pick_contacts)
            {
                rows.push_back(text("Selected: " + std::to_string(s.picked_contacts.size()) + " · /: search · " + s.pick_query));
                // Selected friends remain removable even while the current filter hides them.
                std::string picked = "Picked: ";
                for (auto id : s.picked_contacts)
                {
                    auto found = std::ranges::find(s.contacts, id, &user::id);
                    if (found != s.contacts.end()) { picked += found->username + "; "; }
                }
                rows.push_back(preview_text(picked, width));
                auto values = s.pick_candidates();
                for (std::size_t i = 0; i < values.size(); ++i)
                {
                    auto const& value = *values[i];
                    auto checked = std::ranges::find(s.picked_contacts, value.id) != s.picked_contacts.end();
                    rows.push_back(selected(text(std::string(checked ? "[x] " : "[ ] ") + user_label(value.username)), s.selected == static_cast<int>(i)));
                }
            }
            else
            {
                auto contacts = s.visible_contacts();
                auto count = s.view == page::users ? s.users.size() : contacts.size();
                for (std::size_t i = 0; i < count; ++i)
                {
                    auto const& value = s.view == page::users ? s.users[i] : *contacts[i];
                    auto offset = s.view == page::contacts ? 1 : 0;
                    auto label = user_label(value.username);
                    auto presence = presence_label(s, value.id);
                    if (!presence.empty()) { label += " · " + presence; }
                    rows.push_back(selected(text(label), s.selected == static_cast<int>(i) + offset));
                }
            }
            if (s.view == page::contacts)
            {
                return vbox({text(title) | bold, separator(),
                    selected(text("New friends (" + std::to_string(s.friends.incoming.size()) + ")"), s.selected == 0),
                    separator(), scroll(std::move(rows))}) | flex;
            }
            break;
        }
        case page::friend_requests:
        case page::friend_sent:
        {
            bool const incoming = s.view == page::friend_requests;
            title = incoming ? "New friends · Incoming" : "New friends · Outgoing";
            hint = incoming ? "Tab: outgoing · y: accept · n: reject · Enter: profile" : "Tab: incoming · x: cancel · Enter: profile";
            auto const& requests = incoming ? s.friends.incoming : s.friends.outgoing;
            for (std::size_t i = 0; i < requests.size(); ++i)
            {
                auto const& request = requests[i];
                rows.push_back(selected(text(user_label(request.user.username) + " " + timestamp(request.created_at)), s.selected == static_cast<int>(i)));
            }
            if (requests.empty()) { rows.push_back(text("No pending friend requests")); }
            break;
        }
        case page::profile:
        {
            title = std::string(s.profile.id == s.self.id ? "Account · " : "Profile · ") + user_label(s.profile.username);
            rows.push_back(text(std::string("Avatar: ") + (s.profile.avatar.present ? "set" : "default")));
            auto presence = presence_label(s, s.profile.id);
            if (!presence.empty()) { rows.push_back(text(presence)); }
            if (s.profile.id != s.self.id)
            {
                auto relation = s.friendship(s.profile.id);
                rows.push_back(text(relation == friendship_state::accepted ? "Friends" :
                    relation == friendship_state::outgoing_pending ? "Waiting for acceptance" :
                    relation == friendship_state::incoming_pending ? "Incoming friend request" : "Not friends"));
            }
            rows.push_back(separator());
            auto items = actions(s);
            for (std::size_t i = 0; i < items.size(); ++i) { rows.push_back(selected(text(items[i].label), s.selected == static_cast<int>(i))); }
            break;
        }
        case page::members:
            title = "Members (" + std::to_string(s.members.size()) + ")";
            if (s.self_role() == member_role::owner) { hint = "A: toggle admin · O: transfer to admin · D: remove"; }
            else if (s.self_role() == member_role::admin) { hint = "D: remove selected member"; }
            for (std::size_t i = 0; i < s.members.size(); ++i)
            {
                auto const& m = s.members[i];
                auto role = " · " + role_label(m.role) + (m.id == s.self.id ? " (you)" : "");
                rows.push_back(selected(hbox({preview_text(user_label(m.username), width - DisplayWidth(role)) | flex,
                    text(role) | dim}), s.selected == static_cast<int>(i)));
            }
            break;
        case page::requests:
            title = "Join requests";
            if (!s.requests.empty()) { hint = "y: accept · n: reject"; }
            if (s.requests.empty()) { rows.push_back(text("No join requests to display") | dim); }
            for (std::size_t i = 0; i < s.requests.size(); ++i)
            {
                auto const& value = s.requests[i];
                rows.push_back(selected(text(user_label(value.applicant.username) + " " + timestamp(value.created_at)), s.selected == static_cast<int>(i)));
            }
            if (s.next_requests) { rows.push_back(text("PgDn: more requests")); }
            break;
        case page::search:
            title = "Search: " + s.search_query;
            hint = s.search_results.empty() ? "/: search again" : "Enter/y: show copyable text";
            rows.push_back(text("Loaded hits · live text") | dim);
            rows.push_back(text("Re-search for current matches") | dim);
            if (s.search_results.empty()) { rows.push_back(text("No loaded search hits") | dim); }
            for (std::size_t i = 0; i < s.search_results.size(); ++i) { rows.push_back(message_item(s, s.search_results[i], s.selected == static_cast<int>(i), width, message_scroll)); rows.push_back(text("")); }
            if (s.search_more) { rows.push_back(text("PgDn: earlier results")); }
            break;
        case page::group:
        {
            auto c = s.active_conversation();
            title = c ? "Group · " + c->username : "Group";
            if (c)
            {
                rows.push_back(text("Members: " + std::to_string(s.members.size()) + " · Your role: " + role_label(s.self_role())));
                auto const preview_count = std::min<std::size_t>(3, s.members.size());
                std::string preview = "Preview (" + std::to_string(preview_count) + " of " + std::to_string(s.members.size()) + "): ";
                for (std::size_t i = 0; i < preview_count; ++i)
                { preview += s.members[i].username + " (" + role_label(s.members[i].role) + ") "; }
                rows.push_back(preview_text(preview, width));
                if (c->pinned_message) { rows.push_back(preview_text("Pinned: " + c->pinned_message->text, width)); }
                rows.push_back(wrapped_text("公告: " + (c->announcement.empty() ? "(none)" : c->announcement), width) | size(HEIGHT, LESS_THAN, 3));
                rows.push_back(text(c->join_approval ? "Join approval: on" : "Join approval: off"));
                rows.push_back(separator());
            }
            auto items = actions(s);
            for (std::size_t i = 0; i < items.size(); ++i) { rows.push_back(selected(text(items[i].label), s.selected == static_cast<int>(i))); }
            break;
        }
        case page::help:
            title = "Keyboard help";
            hint = "j/k: scroll · Esc: back";
            rows.push_back(help_content(width));
            break;
        case page::copy:
            title = "Copyable text";
            hint = "Use terminal selection · Esc: back";
            rows.push_back(wrapped_text(s.copy_text, width));
            break;
        default: title = "Chat"; break;
    }
    Elements panel{(s.view == page::profile ? wrapped_text(title, width) : preview_text(title, width)) | bold};
    if (!hint.empty()) { panel.push_back(text(hint) | dim); }
    panel.push_back(separator());
    if (s.view == page::help || s.view == page::copy)
    {
        auto body = vbox(std::move(rows));
        body->ComputeRequirement();
        auto const last_line = std::max(1, body->requirement().min_y - 1);
        body = body | focusPositionRelative(0.f, static_cast<float>(std::clamp(s.selected, 0, last_line)) / last_line) | vscroll_indicator | yframe | flex;
        panel.push_back(body);
    }
    else { panel.push_back(scroll(std::move(rows))); }
    return vbox(std::move(panel)) | flex;
}
Element render_impl(state const& s, int width, int height, Element compose = {}, std::string typing = {}, int message_scroll = -1)
{
    if (state::layout(width, height) == layout_mode::too_small) { return text("Terminal too small (40x12 minimum)") | center; }
    if (!s.self.id)
    {
        return vbox({text("Chat · Login / Register") | bold, text("和朋友，轻松聊。") | dim,
                     text(""), text("Username"), text(""), text("Password"), text(""),
                     text("Log in") | bold, text("Create account"), text("Server settings"),
                     paragraph(s.status), text("Tab: move · Enter: select · Ctrl+C: quit") | dim}) |
            size(WIDTH, LESS_THAN, std::min(44, width - 4)) | center;
    }
    Element content;
    if (s.view == page::conversations || s.view == page::conversation)
    {
        if (state::layout(width, height) == layout_mode::wide)
        {
            content = hbox({conversation_list(s, 29) | size(WIDTH, EQUAL, 30), separator(), conversation_view(s, compose, std::move(typing), width - 34, message_scroll)}) | flex;
        }
        else { content = s.view == page::conversations ? conversation_list(s, width - 3) : conversation_view(s, compose, std::move(typing), width - 3, message_scroll); }
    }
    else { content = secondary(s, width - 3, message_scroll); }
    auto link = " " + link_label(s.link);
    return vbox({hbox({preview_text("Chat · " + s.self.username, width - 2 - DisplayWidth(link)) | bold | flex, text(link)}), text("h Chats  c Contacts  u Account  N New") | dim, separator(), content, separator(), hbox({preview_text(s.status.empty() ? "?: help · : command · Ctrl+C: quit" : s.status, width - 12) | flex, text(" Esc: back") | dim})}) | border;
}
std::size_t selection_count(state const& s, int width)
{
    switch (s.view)
    {
        case page::conversations: return s.conversations.size();
        case page::contacts: return s.visible_contacts().size() + 1;
        case page::pick_contacts: return s.pick_candidates().size();
        case page::friend_requests: return s.friends.incoming.size();
        case page::friend_sent: return s.friends.outgoing.size();
        case page::users: return s.users.size();
        case page::members: return s.members.size();
        case page::requests: return s.requests.size();
        case page::search: return s.search_results.size();
        case page::new_action: case page::profile: case page::group: return actions(s).size();
        case page::help:
        {
            int lines = 0;
            help_content(width, &lines);
            return static_cast<std::size_t>(lines);
        }
        case page::copy:
        {
            auto body = wrapped_text(s.copy_text, width);
            body->ComputeRequirement();
            return static_cast<std::size_t>(std::max(1, body->requirement().min_y));
        }
        default: return 0;
    }
}
class terminal_ui final : public ComponentBase
{
public:
    terminal_ui(app& value, std::function<void()> quit) : app_(value), quit_(std::move(quit))
    {
        InputOption single;
        single.multiline = false;
        url_ = Input(&app_.server_url, "ws://127.0.0.1:18080/ws", single);
        username_ = Input(&app_.username, "Username", single);
        auto password_option = single;
        password_option.password = true;
        password_option.on_enter = [this] { app_.login(); };
        password_ = Input(&app_.password, "Password", password_option);
        ButtonOption action;
        action.transform = [](EntryState const& item) {
            auto body = text((item.focused ? "> " : "  ") + item.label);
            if (item.label == "Log in") { body |= bold; }
            if (item.focused) { body |= inverted; }
            return body;
        };
        login_ = Button("Log in", [this] { app_.login(); }, action);
        register_ = Button("Create account", [this] { app_.login(true); }, action);
        server_settings_ = Button("Server settings", [this] {
            server_settings_open_ = !server_settings_open_;
            if (server_settings_open_) { url_->TakeFocus(); }
            else { username_->TakeFocus(); }
        }, action);
        login_form_ = Container::Vertical({username_, password_, login_, register_, server_settings_, Maybe(url_, &server_settings_open_)});
        Add(login_form_);
        auto compose_option = single;
        compose_option.multiline = true;
        compose_option.on_change = [this] { app_.compose_changed(); };
        compose_ = Input(&app_.data.draft, "Message · Enter: send · Esc: keep", compose_option);
        Add(compose_);
        command_ = Input(&app_.command_text, "command", single);
        Add(command_);
        prompt_ = Input(&prompt_text_, "", single);
        Add(prompt_);
        username_->TakeFocus();
    }
    Element OnRender() override
    {
        flush_paste();
        auto terminal = Terminal::Size();
        update_viewport(terminal);
        sync_message_scroll();
        auto& s = app_.data;
        Element page;
        if (state::layout(terminal.dimx, terminal.dimy) == layout_mode::too_small)
        { return render(s, terminal.dimx, terminal.dimy); }
        if (!s.self.id)
        {
            auto const width = std::min(44, terminal.dimx - 4);
            auto const roomy = terminal.dimy >= 20;
            Elements fields{hbox({text("Chat") | bold, text(" · Login / Register") | dim})};
            if (roomy) { fields.push_back(text("和朋友，轻松聊。") | dim); fields.push_back(text("")); }
            fields.push_back(text("Username"));
            fields.push_back(username_->Render());
            if (roomy) { fields.push_back(text("")); }
            fields.push_back(text("Password"));
            fields.push_back(password_->Render());
            if (roomy) { fields.push_back(text("")); }
            fields.push_back(login_->Render());
            fields.push_back(register_->Render());
            fields.push_back(server_settings_->Render());
            if (server_settings_open_) { fields.push_back(url_->Render()); }
            if (!s.status.empty())
            { fields.push_back(wrapped_text(s.status, width)); }
            else if (s.link != connection::signed_out)
            { fields.push_back(text(link_label(s.link)) | bold); }
            if (roomy) { fields.push_back(text("")); }
            if (roomy || s.status.empty())
            { fields.push_back(paragraph("Tab: move · Enter: select · Ctrl+C: quit") | dim); }
            page = vbox(std::move(fields)) | size(WIDTH, EQUAL, width) | center;
        }
        else
        {
            page = render_impl(s, terminal.dimx, terminal.dimy, s.composing ? compose_->Render() : Element{}, app_.typing_text(), message_scroll_);
        }
        if (app_.dialog)
        {
            sync_prompt();
            return dbox({page, vbox({paragraph(app_.dialog->title) | bold, separator(), prompt_->Render(), text(app_.dialog->confirmation ? "Enter: confirm y · Esc: cancel" : "Enter: confirm · Esc: cancel")}) | border | clear_under | center});
        }
        if (app_.command_mode)
        { return dbox({page, vbox({text("Command"), command_->Render(), text("Enter: run · Esc: cancel")}) | border | clear_under | center}); }
        return page;
    }
    bool OnEvent(Event event) override
    {
        update_viewport(Terminal::Size());
        // Pasted text belongs to the target as it was before queued results change it.
        if (!paste_buffer_.empty() && app_.pending()) { flush_paste(); }
        app_.drain();
        sync_message_scroll();
        auto& s = app_.data;
        if (event == Event::CtrlC)
        {
            app_.shutdown();
            quit_();
            return true;
        }
        if (app_.exiting) { quit_(); return true; }
        if (event == Event::Special("\x1b[200~"))
        {
            if (!pasting_)
            {
                pasting_ = true;
                paste_input_ = input();
                paste_conversation_ = s.active;
                paste_buffer_.clear();
            }
            return true;
        }
        if (event == Event::Special("\x1b[201~"))
        { flush_paste(); pasting_ = false; paste_input_.reset(); return true; }
        if (pasting_ && event != Event::Custom)
        {
            if (paste_input_ != input() || paste_conversation_ != s.active) { paste_buffer_.clear(); paste_input_.reset(); }
            if (paste_input_)
            {
                // Inserting character by character rescans the whole text each time, so a large
                // paste is collected and inserted at once.
                if (event == Event::Escape) { flush_paste(); paste_input_.reset(); }
                else if (event.is_character()) { paste_buffer_ += event.character(); }
                else if (event == Event::Return) { paste_buffer_ += paste_input_ == compose_ ? '\n' : ' '; }
                else if (event == Event::Tab) { paste_buffer_ += ' '; }
            }
            return true;
        }
        if (event == Event::Custom)
        {
            if (!s.self.id) { login_form_->TakeFocus(); }
            else if (s.composing) { compose_->TakeFocus(); }
            auto terminal = Terminal::Size();
            if (state::layout(terminal.dimx, terminal.dimy) != layout_mode::too_small) { app_.mark_visible_read(); }
            return true;
        }
        if (app_.dialog)
        {
            sync_prompt();
            if (event == Event::Escape) { app_.cancel_prompt(); prompt_active_ = false; return true; }
            if (event == Event::Return)
            {
                app_.dialog->text = prompt_text_;
                app_.submit_prompt();
                prompt_active_ = false;
                return true;
            }
            return prompt_->OnEvent(event);
        }
        prompt_active_ = false;
        if (app_.command_mode)
        {
            command_->TakeFocus();
            if (event == Event::Escape) { app_.command_mode = false; return true; }
            if (event == Event::Return)
            {
                auto command = std::exchange(app_.command_text, {});
                app_.command_mode = false;
                app_.command(std::move(command));
                if (app_.exiting) { quit_(); }
                return true;
            }
            return command_->OnEvent(event);
        }
        if (!s.self.id)
        {
            if (s.link != connection::signed_out)
            {
                if (event == Event::Escape) { app_.logout(); }
                return true;
            }
            if (event == Event::Escape && server_settings_open_)
            {
                server_settings_open_ = false;
                username_->TakeFocus();
                return true;
            }
            return login_form_->OnEvent(event);
        }
        if (s.composing)
        {
            compose_->TakeFocus();
            if (event == Event::Escape) { app_.stop_composing(); return true; }
            if (event == Event::Return) { app_.send(); return true; }
            return compose_->OnEvent(event);
        }
        if (event == Event::Escape) { app_.back(); return true; }
        if (event == Event::Character(':')) { app_.command_mode = true; app_.command_text.clear(); command_->TakeFocus(); return true; }
        if (event == Event::Tab || event == Event::TabReverse)
        {
            if (s.view == page::friend_requests || s.view == page::friend_sent)
            { app_.command(s.view == page::friend_requests ? "friend-sent" : "friend-requests"); }
            else if (s.view == page::conversations && s.active) { app_.navigate(page::conversation); }
            else if (s.view == page::conversation) { app_.navigate(page::conversations); }
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) { move(1); return true; }
        if (event == Event::ArrowUp || event == Event::Character('k')) { move(-1); return true; }
        if (event == Event::Return)
        {
            auto options = actions(s);
            if (!options.empty() && s.selected >= 0 && static_cast<std::size_t>(s.selected) < options.size()) { app_.command(options[s.selected].command); }
            else if (s.view == page::pick_contacts) { app_.finish_pick(); }
            else if (s.view == page::members) { app_.command("profile"); }
            else { app_.activate(); }
            return true;
        }
        if (event == Event::Character(' ') && s.view == page::pick_contacts) { app_.toggle_pick(); return true; }
        if (event == Event::PageUp)
        {
            if (s.view == page::conversation) { s.at_latest = false; app_.history(true); }
            else if (s.view == page::search) { app_.search(s.search_query, true); }
            return true;
        }
        if (event == Event::PageDown)
        {
            if (s.view == page::search) { app_.search(s.search_query, true); }
            else if (s.view == page::requests) { app_.requests(true); }
            else if (s.view == page::conversations) { app_.conversations(true); }
            return true;
        }
        if ((event == Event::Character('[') || event == Event::Character(']')) &&
            (s.view == page::conversation || s.view == page::search))
        {
            auto const* message = s.selected_message();
            if (!message) { return true; }
            auto width = app_.viewport_width - (s.view == page::conversation && state::layout(app_.viewport_width, app_.viewport_height) == layout_mode::wide ? 34 : 3);
            auto item = message_item(s, *message, false, width, -1);
            item->ComputeRequirement();
            int last = std::max(0, item->requirement().min_y - 1);
            int current = message_scroll_ < 0 ? last : message_scroll_;
            current = std::clamp(current + (event == Event::Character('[') ? -1 : 1), 0, last);
            message_scroll_ = current == last ? -1 : current;
            if (s.view == page::conversation)
            {
                s.at_latest = message_scroll_ < 0 && !s.messages.empty() &&
                    s.message_selected == static_cast<int>(s.messages.size()) - 1;
                app_.mark_visible_read();
            }
            return true;
        }
        if (event == Event::Character('G') && s.view == page::conversation)
        { message_scroll_ = -1; app_.command("latest"); return true; }
        if (s.view == page::members)
        {
            if (event == Event::Character('A')) { app_.command("admin"); return true; }
            if (event == Event::Character('O')) { app_.command("transfer"); return true; }
            if (event == Event::Character('D')) { app_.command("kick"); return true; }
        }
        if ((s.view == page::contacts || s.view == page::pick_contacts) && event == Event::Character('/'))
        { app_.command("filter"); return true; }
        if (s.view == page::friend_requests)
        {
            if (event == Event::Character('y')) { app_.command("accept-friend"); return true; }
            if (event == Event::Character('n')) { app_.command("reject-friend"); return true; }
        }
        if (s.view == page::friend_sent && event == Event::Character('x'))
        { app_.command("cancel-friend"); return true; }
        if (s.view == page::requests)
        {
            if (event == Event::Character('y')) { app_.command("accept"); return true; }
            if (event == Event::Character('n')) { app_.command("reject"); return true; }
        }
        if (event == Event::Character('N')) { app_.command("new"); return true; }
        if (event == Event::Character('h')) { app_.command("chats"); return true; }
        if (event == Event::Character('u')) { app_.command("account"); return true; }
        if (event == Event::Character('i'))
        {
            if (s.view != page::conversation) { return false; }
            app_.command("compose");
            compose_->TakeFocus();
            return true;
        }
        for (auto const& [key, command] : std::array<std::pair<char, const char*>, 12>{{
            {'r', "reply"}, {'e', "edit"}, {'d', "delete"}, {'a', "reaction"},
            {'y', "copy"}, {'s', "save"}, {'/', "search"}, {'m', "mute"}, {'p', "pin"},
            {'c', "contacts"}, {'g', "group"}, {'?', "help"}}})
        {
            if (event == Event::Character(key))
            {
                if (std::string_view("redays/").find(key) != std::string_view::npos &&
                    s.view != page::conversation && s.view != page::search) { return false; }
                if ((key == 'm' || key == 'p') && s.view != page::conversation && s.view != page::conversations) { return false; }
                app_.command(command);
                if (s.composing) { compose_->TakeFocus(); }
                return true;
            }
        }
        return false;
    }
private:
    Component input()
    {
        if (app_.dialog) { sync_prompt(); return prompt_; }
        if (app_.command_mode) { return command_; }
        if (app_.data.composing) { return compose_; }
        if (!app_.data.self.id && app_.data.link == connection::signed_out)
        {
            for (auto const& field : {url_, username_, password_}) { if (field->Focused()) { return field; } }
        }
        return {};
    }
    void flush_paste()
    {
        if (paste_buffer_.empty()) { return; }
        auto text = std::exchange(paste_buffer_, {});
        if (paste_input_ && paste_input_ == input() && paste_conversation_ == app_.data.active)
        { paste_input_->OnEvent(Event::Character(std::move(text))); }
    }
    void sync_message_scroll()
    {
        auto const& s = app_.data;
        auto const* current = s.selected_message();
        auto const id = current ? current->id : 0;
        if (scroll_conversation_ != s.active || scroll_message_ != id || scroll_page_ != s.view)
        {
            message_scroll_ = -1;
            scroll_conversation_ = s.active;
            scroll_message_ = id;
            scroll_page_ = s.view;
        }
    }
    void update_viewport(ftxui::Dimensions dimensions)
    {
        // Headless component tests may have no terminal dimensions. Production
        // resize events update visibility before any queued SDK result is used.
        if (dimensions.dimx > 0 && dimensions.dimy > 0)
        {
            app_.viewport_width = dimensions.dimx;
            app_.viewport_height = dimensions.dimy;
        }
    }
    void sync_prompt()
    {
        if (!prompt_active_)
        {
            prompt_text_ = app_.dialog->text;
            prompt_active_ = true;
            prompt_->TakeFocus();
        }
    }
    void move(int delta)
    {
        auto& s = app_.data;
        if (s.view == page::conversation || s.view == page::search) { message_scroll_ = -1; app_.select_message(delta); app_.mark_visible_read(); return; }
        auto count = selection_count(s, app_.viewport_width - 3);
        auto& index = s.view == page::conversations ? s.conversation_selected : s.selected;
        index = count ? std::clamp(index + delta, 0, static_cast<int>(count) - 1) : 0;
        if (s.view == page::conversations && delta > 0 && static_cast<std::size_t>(index + 1) >= count && s.next_conversations) { app_.conversations(true); }
    }
    app& app_;
    std::function<void()> quit_;
    Component url_, username_, password_, login_, register_, server_settings_, login_form_, compose_, command_, prompt_;
    bool server_settings_open_ = false;
    bool pasting_ = false;
    Component paste_input_;
    std::string paste_buffer_;
    std::int64_t paste_conversation_ = 0;
    std::string prompt_text_;
    bool prompt_active_ = false;
    int message_scroll_ = -1;
    std::int64_t scroll_conversation_ = 0, scroll_message_ = 0;
    page scroll_page_ = page::conversations;
};
}

ftxui::Element render(state const& data, int width, int height, int message_scroll) { return render_impl(data, width, height, {}, {}, message_scroll); }
ftxui::Component make_ui(app& application, std::function<void()> quit)
{
    return std::make_shared<terminal_ui>(application, std::move(quit));
}
}
