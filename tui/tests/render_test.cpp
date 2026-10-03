#include "ui.hpp"
#include "app.hpp"
#include <ftxui/component/event.hpp>

#include <iostream>
#include <string>
#include <utility>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

namespace
{
bool expect(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; }
    return condition;
}
std::string draw(chat::tui::state const& data, int columns, int rows, int line = -1)
{
    ftxui::Screen screen(columns, rows);
    ftxui::Render(screen, chat::tui::render(data, columns, rows, line));
    return screen.ToString();
}
}

int main()
{
    using namespace chat::tui;
    bool ok = true;
    ok &= expect(ftxui::string_width("张 三") == 5, "CJK uses terminal cells");
    state s;
    ok &= expect(draw(s, 80, 24).find("Login / Register") != std::string::npos, "login page");
    s.self = {1, "Alice", {}};
    s.link = connection::online;
    chat::conversation direct;
    direct.id = 10;
    direct.user = 2;
    direct.username = "张 三";
    s.conversations.push_back(direct);
    s.select_conversation(10);
    for (const auto& [columns, rows] : {std::pair{80, 24}, {100, 30}, {120, 40}, {60, 20}})
    {
        auto output = draw(s, columns, rows);
        ok &= expect(output.find("张 三") != std::string::npos, "Unicode remains visible");
        ok &= expect(output.find("你们目前不是好友") != std::string::npos, "read-only direct prompt");
    }
    s.friends.outgoing = {{{direct.user, direct.username, {}}, 1}};
    ok &= expect(draw(s, 80, 24).find("好友申请已发送，等待对方确认") != std::string::npos, "historical outgoing direct has pending banner");
    s.friends.outgoing.clear();
    s.friends.incoming = {{{direct.user, direct.username, {}}, 1}};
    ok &= expect(draw(s, 80, 24).find("对方已发送好友申请，确认后可继续聊天") != std::string::npos, "historical incoming direct has pending banner");
    s.friends.incoming.clear();
    ok &= expect(draw(s, 20, 4).find("Terminal too small") != std::string::npos, "too small fallback");
    s.view = page::conversations;
    ok &= expect(draw(s, 60, 20).find("Chats") != std::string::npos, "narrow list navigation");
    ok &= expect(draw(s, 60, 20).find("你们目前不是好友") == std::string::npos, "narrow list does not squeeze conversation");
    {
        auto long_name_list = s;
        long_name_list.conversations.front().username = "LongUsernamePrefix" + std::string(46, 'x');
        ok &= expect(draw(long_name_list, 120, 40).find("LongUsernamePrefix") != std::string::npos, "sidebar retains long username prefix");
        ok &= expect(draw(long_name_list, 60, 20).find("LongUsernamePrefix") != std::string::npos, "narrow list retains long username prefix");
    }
    {
        auto status_list = s;
        auto& item = status_list.conversations.front();
        item.kind = chat::conversation_kind::group;
        item.username = "百人验证_" + std::string(48, 'x');
        item.pinned = true; item.muted = true; item.unread = 100;
        for (auto const& [columns, rows] : {std::pair{80, 24}, {160, 45}})
        {
            auto output = draw(status_list, columns, rows);
            ok &= expect(output.find("[pin]") != std::string::npos, "long conversation title keeps personal pin visible");
            ok &= expect(output.find("[mute]") != std::string::npos, "long conversation title keeps mute visible");
            ok &= expect(output.find("(100)") != std::string::npos, "long conversation title keeps unread count visible");
            ok &= expect(output.find("[百] 百") != std::string::npos, "conversation status leaves an identifiable title prefix");
        }
    }
    {
        auto multiline_list = s;
        multiline_list.conversations.front().username = "Main summary group";
        multiline_list.conversations.front().last.text.clear();
        for (int i = 0; i < 512; ++i) { multiline_list.conversations.front().last.text += "summary line\n"; }
        auto next = multiline_list.conversations.front();
        next.id = 11; next.username = "Next visible friend"; next.last.text = "short";
        multiline_list.conversations.push_back(std::move(next));
        for (auto const& [columns, rows] : {std::pair{80,24}, {160,45}})
        {
            auto output = draw(multiline_list, columns, rows);
            ok &= expect(output.find("[M] Main summary group") != std::string::npos, "multiline latest summary keeps selected conversation title visible");
            ok &= expect(output.find("Next visible friend") != std::string::npos, "multiline latest summary leaves next conversation visible");
        }
    }
    // A visible conversation must not keep the keyboard highlight after Esc.
    {
        auto navigation = s;
        navigation.messages = {chat::message{}};
        navigation.messages.front().id = 1;
        navigation.messages.front().conversation = 10;
        navigation.messages.front().text = "READONLY_HISTORY";
        navigation.messages.front().username = "peer";
        for (auto view : {page::conversations, page::conversation})
        {
            navigation.view = view;
            ftxui::Screen screen(120, 40);
            ftxui::Render(screen, render(navigation, 120, 40));
            bool list_focus = false, history_focus = false;
            for (int y = 0; y < 40; ++y)
                for (int x = 1; x < 119; ++x)
                    if (screen.CellAt(x, y).inverted)
                    { (x < 31 ? list_focus : history_focus) = true; }
            ok &= expect(list_focus == (view == page::conversations), "only Chats keyboard focus highlights its list");
            ok &= expect(history_focus == (view == page::conversation), "Esc removes conversation keyboard highlight even when read only");
        }
    }
    s.view = page::conversation;
    auto& group = s.conversations.front();
    group.kind = chat::conversation_kind::group;
    group.member_count = 3;
    group.can_send = true;
    group.announcement = "开发公告";
    chat::message message;
    message.id = 1;
    message.conversation = 10;
    message.username = "张 三";
    message.from = 2;
    message.text = "你好 @Alice";
    message.edited_at = 1;
    message.reply = chat::quoted_message{2, 1, "Alice", "old", {}, true};
    message.reactions = {{"👍", {1, 2}}};
    message.mentions = {{1, "Alice"}};
    s.messages.push_back(message);
    auto output = draw(s, 120, 40);
    for (auto const* token : {"开发公告", "你好", "(edited)", "消息已删除", "👍", "@Alice"})
    { ok &= expect(output.find(token) != std::string::npos, token); }
    {
        auto preview_state = s;
        auto& current = preview_state.conversations.front();
        current.announcement.clear();
        std::string quote;
        for (int i = 0; i < 2048; ++i) { current.announcement += "a\n"; }
        for (int i = 0; i < 80; ++i) { quote += "q\n"; }
        current.pinned_message = chat::quoted_message{2, 1, "Alice", quote, {}, false};
        preview_state.reply = current.pinned_message;
        preview_state.messages.front().reply = current.pinned_message;
        preview_state.messages.front().text = "VISIBLE_HISTORY";
        preview_state.composing = true;
        preview_state.draft = "DRAFT_ACCESS\nsecond line";
        for (auto const& [columns, rows] : {std::pair{60, 20}, {80, 24}})
        {
            auto preview_output = draw(preview_state, columns, rows);
            ok &= expect(preview_output.find("DRAFT_ACCESS") != std::string::npos, "multiline previews preserve composer");
            ok &= expect(preview_output.find("VISIBLE_HISTORY") != std::string::npos, "multiline previews preserve history");
        }
    }
    s.view = page::profile;
    s.profile = {3, "stranger", {}};
    output = draw(s, 80, 24);
    ok &= expect(output.find("Add friend") != std::string::npos, "non-contact add action");
    ok &= expect(output.find("Message") == std::string::npos, "non-contact cannot message");
    s.presences.emplace(3, chat::presence{3, true, 0});
    ok &= expect(draw(s, 80, 24).find("online") == std::string::npos, "non-contact presence hidden");
    s.friends.outgoing = {{{3, "stranger", {}}, 1}};
    output = draw(s, 80, 24);
    ok &= expect(output.find("Waiting for acceptance") != std::string::npos && output.find("Cancel friend request") != std::string::npos,
                 "outgoing pending profile can cancel");
    ok &= expect(output.find("Message") == std::string::npos && output.find("online") == std::string::npos,
                 "pending profile cannot message or see presence");
    s.friends.outgoing.clear();
    s.friends.incoming = {{{3, "stranger", {}}, 1}};
    output = draw(s, 80, 24);
    ok &= expect(output.find("Accept friend request") != std::string::npos && output.find("Reject friend request") != std::string::npos,
                 "incoming pending profile actions");
    s.view = page::contacts;
    ok &= expect(draw(s, 80, 24).find("New friends (1)") != std::string::npos, "contacts expose pending friend count");
    ok &= expect(draw(s, 80, 24).find(":create-group") == std::string::npos, "Contacts header contains no group creation action");
    s.contacts = {{9, "AcceptedOnly", {}}};
    s.friends.outgoing = {{{10, "OutgoingOnly", {}}, 1}};
    output = draw(s, 80, 24);
    ok &= expect(output.find("AcceptedOnly") != std::string::npos && output.find("OutgoingOnly") == std::string::npos &&
                 output.find("stranger") == std::string::npos, "Contacts rows exclude incoming and outgoing requests");
    s.view = page::friend_requests;
    ok &= expect(draw(s, 60, 20).find("Incoming") != std::string::npos, "incoming requests page");
    s.view = page::friend_sent;
    ok &= expect(draw(s, 60, 20).find("Outgoing") != std::string::npos, "outgoing requests page");
    s.contacts = {{3, "stranger", {}}, {4, "张 三", {}}};
    s.view = page::pick_contacts;
    s.pick_query = "张";
    s.picked_contacts = {4};
    output = draw(s, 80, 24);
    ok &= expect(output.find("Selected: 1") != std::string::npos && output.find("[x]") != std::string::npos && output.find("stranger") == std::string::npos,
                 "group picker filters accepted friends and shows selected count");
    s.pick_query.clear(); s.picked_contacts.clear();
    s.view = page::group;
    s.conversations.front().pinned_message = chat::quoted_message{1, 2, "张 三", "pinned", {}, false};
    s.members = {{1, "Alice", chat::member_role::member, {}}};
    ok &= expect(draw(s, 80, 24).find("invitation link") == std::string::npos, "members never see secret link actions");
    ok &= expect(draw(s, 80, 24).find("Show full announcement") != std::string::npos, "members can read full announcement");
    ok &= expect(draw(s, 80, 24).find("View pinned message") != std::string::npos, "members can view pinned message");
    auto const saved_announcement = s.conversations.front().announcement;
    s.conversations.front().announcement = std::string(2000, 'A');
    ok &= expect(draw(s, 60, 20).find("Show full announcement") != std::string::npos, "long announcement preview leaves actions visible");
    s.conversations.front().announcement = saved_announcement;
    s.members.front().role = chat::member_role::owner;
    ok &= expect(draw(s, 120, 40).find("Create invitation link") != std::string::npos, "owner management actions");
    s.view = page::help;
    ok &= expect(draw(s, 120, 40).find("Ctrl+C") != std::string::npos, "keyboard help");
    s.view = page::conversation;
    s.messages = {message};
    s.messages.front().from = s.self.id;
    s.read_positions = {{1, 100}, {2, 100}, {99, 100}};
    s.members = {{1, "Alice", chat::member_role::owner, {}}, {2, "张 三", chat::member_role::member, {}}};
    ok &= expect(draw(s, 120, 40).find("已读 1 人") != std::string::npos, "group read count excludes departed members");
    auto& long_message = s.messages.front();
    long_message.reply.reset();
    long_message.reactions.clear();
    long_message.mentions.clear();
    long_message.text = "HEAD";
    for (int i = 0; i < 900; ++i) { long_message.text += "长"; }
    long_message.text += "MIDDLE";
    for (int i = 0; i < 900; ++i) { long_message.text += "文"; }
    long_message.text += "TAIL";
    ok &= expect(draw(s, 60, 20, 0).find("HEAD") != std::string::npos, "long unspaced message beginning wraps");
    ok &= expect(draw(s, 60, 20, -1).find("TAIL") != std::string::npos, "long message tail scrolls into view");
    ok &= expect(draw(s, 60, 20, 34).find("MIDDLE") != std::string::npos, "long message middle scrolls into view");
    s.view = page::copy;
    s.copy_text = long_message.text;
    s.selected = 0;
    ok &= expect(draw(s, 60, 20).find("HEAD") != std::string::npos, "long copy beginning");
    s.selected = 10000;
    ok &= expect(draw(s, 60, 20).find("TAIL") != std::string::npos, "long copy tail");
    // Contacts remain a distinct, usable list even with many accepted and pending rows.
    for (int count : {0, 1, 25})
    {
        state contacts;
        contacts.self = {1, "自己", {}};
        contacts.link = connection::online;
        contacts.view = page::contacts;
        for (int i = 0; i < count; ++i) { contacts.contacts.push_back({i + 2, "好友" + std::to_string(i), {}}); }
        for (int i = 0; i < 5; ++i)
        {
            contacts.friends.incoming.push_back({{100 + i, "PendingIncoming" + std::to_string(i), {}}, 1});
            contacts.friends.outgoing.push_back({{200 + i, "PendingOutgoing" + std::to_string(i), {}}, 1});
        }
        contacts.selected = count;
        for (auto [columns, rows] : {std::pair{70, 20}, {80, 24}, {120, 40}})
        {
            auto text = draw(contacts, columns, rows);
            ok &= expect(text.find("New friends (5)") != std::string::npos, "New friends entry remains at Contacts top while scrolling accepted rows");
            ok &= expect(text.find("PendingIncoming") == std::string::npos && text.find("PendingOutgoing") == std::string::npos,
                         "Many pending requests never appear as accepted rows");
            if (count) { ok &= expect(text.find("好友" + std::to_string(count - 1)) != std::string::npos, "Selected accepted friend stays visible"); }
        }
    }
    {
        auto hundred = s;
        hundred.view = page::members;
        hundred.members.clear();
        for (int i = 0; i < 100; ++i)
        { hundred.members.push_back({i + 1, "Member" + std::to_string(i), i == 0 ? chat::member_role::owner : i < 4 ? chat::member_role::admin : chat::member_role::member, {}}); }
        for (int index : {0, 2, 50, 99})
        {
            hundred.selected = index;
            for (auto [columns, rows] : {std::pair{70, 20}, {100, 30}, {120, 40}})
            {
                auto text = draw(hundred, columns, rows);
                ok &= expect(text.find("Members (100)") != std::string::npos && text.find("Member" + std::to_string(index)) != std::string::npos,
                             "Hundred member navigation retains header and selected member at every width");
            }
        }
    }
    // Real FTXUI input and routing: no terminal or network is needed.
    {
        app application;
        bool quit = false;
        auto component = make_ui(application, [&] { quit = true; });
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Character("张三"));
        ok &= expect(application.username == "张三", "login Unicode input");
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Character("secret-password"));
        ftxui::Screen login_screen(80, 24);
        ftxui::Render(login_screen, component->Render());
        ok &= expect(login_screen.ToString().find("secret-password") == std::string::npos, "password hidden");
        application.data.self = {1, "Alice", {}};
        application.data.view = page::contacts;
        application.data.contacts = {{2, "Bob", {}}, {3, "张三", {}}};
        component->OnEvent(ftxui::Event::Character('j'));
        ok &= expect(application.data.selected == 1, "contact keyboard selection");
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.view == page::profile && application.data.profile.id == 2, "contact profile navigation");
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.view == page::contacts, "Esc returns from profile");
        application.data.conversations = {direct};
        application.data.active = direct.id;
        application.data.friends.outgoing = {{{direct.user, direct.username, {}}, 1}};
        application.data.draft = "read-only draft";
        for (int width : {120, 70, 120, 70})
        {
            application.viewport_width = width;
            application.navigate(page::conversations);
            application.navigate(page::conversation);
            component->OnEvent(ftxui::Event::Escape);
            ok &= expect(application.data.view == page::conversations && application.data.active == direct.id &&
                         application.data.draft == "read-only draft", "read-only Escape returns to Chats without hiding history or draft");
        }
        application.navigate(page::conversations);
        application.navigate(page::conversation);
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Escape);
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.view == page::conversations, "Tab between Chats and history does not create a Back loop");
        application.navigate(page::contacts);
        application.navigate(page::friend_requests);
        application.navigate(page::friend_sent);
        application.navigate(page::friend_requests);
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.view == page::contacts, "Friend request tabs return directly to Contacts");
        component->OnEvent(ftxui::Event::Character('N'));
        output = draw(application.data, 80, 24);
        ok &= expect(output.find("Add friend") != std::string::npos && output.find("Create group") != std::string::npos &&
                     output.find("Join group") != std::string::npos, "New menu exposes existing three actions");
        component->OnEvent(ftxui::Event::Escape);
        for (auto requests_page : {page::friend_requests, page::requests})
        {
            application.navigate(requests_page);
            component->OnEvent(ftxui::Event::Character('N'));
            ok &= expect(application.data.view == page::new_action, "New key never invokes a friend/group request rejection");
            component->OnEvent(ftxui::Event::Escape);
        }
        component->OnEvent(ftxui::Event::Character('u'));
        output = draw(application.data, 80, 24);
        ok &= expect(output.find("Account") != std::string::npos && output.find("Log out") != std::string::npos,
                     "Account primary shortcut exposes existing profile and logout");
        application.data.link = connection::online;
        application.command("logout");
        ok &= expect(application.dialog && application.dialog->confirmation && application.data.self.id == 1, "logout requires confirmation");
        application.cancel_prompt();
        application.data.draft = "保留草稿";
        application.data.composing = true;
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(!application.data.composing && application.data.draft == "保留草稿", "Esc preserves compose draft");
        component->OnEvent(ftxui::Event::Character(':'));
        component->OnEvent(ftxui::Event::Character("help"));
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.view == page::help && !application.command_mode, "command routes to same help action");
        application.dialog = prompt{"Confirm", "", true};
        component->OnEvent(ftxui::Event::Character("y"));
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(!application.dialog, "confirmation can be cancelled");
        component->OnEvent(ftxui::Event::CtrlC);
        ok &= expect(quit && application.exiting, "Ctrl+C shuts down before exit");
    }
    // FTXUI text must not pass untrusted terminal escapes to the output.
    ftxui::Screen screen(50, 1);
    ftxui::Render(screen, ftxui::text("hello\x1b[2J\a\xc2\x9b"));
    ok &= expect(screen.ToString().find('\x1b') == std::string::npos, "ESC is filtered");
    ok &= expect(screen.ToString().find('\a') == std::string::npos, "BEL is filtered");
    ok &= expect(screen.ToString().find("\xc2\x9b") == std::string::npos, "C1 control is filtered");
    ftxui::Screen message_screen(100, 3);
    ftxui::Render(message_screen, ftxui::paragraph("message\x1b]52;c;payload\a\x1b[2J\xc2\x9b"));
    auto safe_message = message_screen.ToString();
    ok &= expect(safe_message.find("\x1b]52;") == std::string::npos, "paragraph does not emit clipboard escapes");
    ok &= expect(safe_message.find("\x1b[2J") == std::string::npos, "paragraph does not emit clear-screen escapes");
    ok &= expect(safe_message.find('\a') == std::string::npos, "paragraph filters BEL");
    ok &= expect(safe_message.find("\xc2\x9b") == std::string::npos, "paragraph filters C1 control");
    return ok ? 0 : 1;
}
