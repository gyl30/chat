#include "ui.hpp"
#include "app.hpp"
#include <chat/error_text.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>

#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

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

bool has_erase_character(std::string const& wire)
{
    for (std::size_t i = 0; (i = wire.find("\x1b[", i)) != std::string::npos; ++i)
    {
        auto end = wire.find_first_not_of("0123456789;", i + 2);
        if (end != std::string::npos && wire[end] == 'X') { return true; }
    }
    return false;
}

// Capture the public loop's real output, not a private Draw hook or serializer substitute.
bool check_blank_preerase(ftxui::App screen, bool check_resize = false)
{
    using namespace ftxui;
    screen.HandlePipedInput(false);
    screen.TrackMouse(false);
    // Keep a 6x2 first frame with both literal spaces and untouched default cells.
    Element frame = vbox({text("AB CD "), text("UV")});
    auto component = Renderer([&] { return frame; });
    std::ostringstream output;
    struct RestoreOutput
    {
        std::streambuf* previous;
        ~RestoreOutput() { std::cout.rdbuf(previous); }
    } restore{std::cout.rdbuf(output.rdbuf())};
    bool ok = true;
    {
        Loop loop(&screen, component);
        loop.RunOnce();
        ok &= expect(!has_erase_character(output.str()), "First allocation must not pre-erase an unestablished inline area");
        auto next = [&](Element value)
        {
            auto begin = output.str().size();
            frame = std::move(value);
            screen.PostEvent(Event::Custom);
            loop.RunOnce();
            return output.str().substr(begin);
        };
        if (check_resize)
        {
            auto wire = next(vbox({text("A  B "), text("UVWXY")}));
            ok &= expect(!has_erase_character(wire), "A resized frame uses its existing allocation path, not blank pre-erasure");
            wire = next(vbox({text("A  B "), text("UVWXY")}));
            ok &= expect(wire.find("\x1b[0m\x1b[1C\x1b[2X\x1b[3C\x1b[1X\rA  B ") != std::string::npos,
                         "The following same-size inline frame erases only its allocated default blanks");
        }
        else
        {
            auto wire = next(vbox({text("A  B  "), text("  C  D")}));
            // Row 0: [1,3), [4,6); row 1: [0,2), [3,5). Return to the same top-left origin.
            auto const prefix = "\x1b[0m\x1b[1C\x1b[2X\x1b[3C\x1b[2X\r\x1b[1B\x1b[2X\x1b[3C\x1b[2X\r\x1b[1A";
            ok &= expect(wire.find(std::string(prefix) + "A  B  \r\n  C  D") != std::string::npos,
                         "Real Draw pre-erases bounded blank runs and restores its relative inline/fullscreen origin");
            wire = next(vbox({text("ABCDE "), text("UVWXYZ")}));
            ok &= expect(wire.find("\x1b[0m\x1b[5C\x1b[1X\rABCDE ") != std::string::npos,
                         "Last-column ECH never advances beyond the allocated last column");
            wire = next(vbox({text("      "), text("UVWXYZ")}));
            ok &= expect(wire.find("\x1b[0m\x1b[6X\r      ") != std::string::npos,
                         "An entirely default blank row is erased within its existing allocation");
            wire = next(vbox({text("中  R "), text("UVWXYZ")}));
            ok &= expect(wire.find("\x1b[0m\x1b[2C\x1b[2X\x1b[3C\x1b[1X\r中  R ") != std::string::npos,
                         "Wide glyph heads and negative continuation cells are never erased");
            for (auto const& style : std::vector<Decorator>{bold, dim, italic, inverted, underlined,
                    underlinedDouble, blink, strikethrough, automerge, color(Color::Red),
                    bgcolor(Color::Blue), hyperlink("https://example.invalid/blank")})
            {
                wire = next(vbox({hbox({text("A"), text(" ") | style, text(" "), text("B"),
                                       text(" ") | style, text("C")}), text("UVWXYZ")}));
                ok &= expect(wire.find("\x1b[0m\x1b[2C\x1b[1X\r") != std::string::npos &&
                             wire.find("\x1b[2X") == std::string::npos,
                             "Every style flag, foreground, background and hyperlink excludes neighboring blanks from ECH");
            }
            wire = next(vbox({text("ABCDEF"), text("UVWXYZ")}));
            ok &= expect(!has_erase_character(wire), "A frame without eligible blank runs emits no pre-erasure");
        }
    }
    return ok;
}

// A signed-in app with one open conversation that accepts typing (no network client).
void writable_conversation(chat::tui::app& application)
{
    application.data.self = {1, "Alice", {}};
    application.data.link = chat::tui::connection::online;
    chat::conversation conversation;
    conversation.id = 10; conversation.username = "group"; conversation.can_send = true;
    conversation.kind = chat::conversation_kind::group;
    application.data.conversations = {conversation};
    application.data.active = 10;
    application.data.view = chat::tui::page::conversation;
    application.data.composing = true;
}

bool check_real_draw_blank_preerase()
{
    auto fallback = ftxui::Terminal::Size();
    ftxui::Terminal::SetFallbackSize({6, 2});
    bool ok = check_blank_preerase(ftxui::App::FixedSize(6, 2));
    ok &= check_blank_preerase(ftxui::App::Fullscreen());
    ok &= check_blank_preerase(ftxui::App::FitComponent(), true);
    ftxui::Terminal::SetFallbackSize(fallback);
    // The text/dump contract remains literal, including default empty cells.
    ftxui::Screen literal(6, 2);
    ftxui::Render(literal, ftxui::vbox({ftxui::text("A  B  "), ftxui::text("  C  D")}));
    ok &= expect(literal.ToString() == "A  B  \r\n  C  D", "Screen::ToString retains its exact literal blank contract");
    ftxui::Screen empty_cells(6, 2);
    ok &= expect(empty_cells.ToString() == "      \r\n      ",
                 "Screen::ToString serializes untouched default empty cells as literal spaces");
    return ok;
}
}

int main()
{
    using namespace chat::tui;
    bool ok = true;
    ok &= expect(ftxui::string_width("张 三") == 5, "CJK uses terminal cells");
    state s;
    ok &= expect(draw(s, 80, 24).find("登录 / 注册") != std::string::npos, "login page");
    s.self = {1, "Alice", {}};
    s.link = connection::online;
    {
        auto contact = s;
        contact.contacts = {{2, "Bob", {}}};
        contact.profile = contact.contacts.front();
        contact.presences.emplace(2, chat::presence{2, true, 0});
        chat::conversation conversation;
        conversation.id = 11; conversation.user = 2; conversation.username = "Bob";
        contact.conversations = {conversation};
        contact.select_conversation(conversation.id);
        for (int columns : {60, 70, 80, 100, 120, 160})
        {
            contact.view = page::profile;
            auto output = draw(contact, columns, 30);
            ok &= expect(output.find("在线") != std::string::npos && output.find(" · 在线") == std::string::npos,
                         "Standalone profile presence has no leading append separator");
            contact.presences.at(2).online = false;
            output = draw(contact, columns, 30);
            ok &= expect(output.find("离线") != std::string::npos && output.find(" · 离线") == std::string::npos,
                         "Standalone offline presence has no leading append separator");
            contact.presences.at(2).last_seen = 1700000000000;
            output = draw(contact, columns, 30);
            ok &= expect(output.find("最后在线 ") != std::string::npos && output.find(" · 最后在线 ") == std::string::npos,
                         "Standalone last-seen presence has no leading append separator");
            contact.presences.at(2) = {2, true, 0};
            for (auto view : {page::conversations, page::contacts, page::conversation})
            {
                contact.view = view;
                ok &= expect(draw(contact, columns, 30).find(" · 在线") != std::string::npos,
                             "Appended presence remains separated from identity");
            }
        }
    }
    {
        app application([]{});
        application.data.self = {1, "Alice", {}};
        application.data.link = connection::online;
        application.data.contacts = {{2, "Bob", {}}};
        application.data.view = page::pick_contacts;
        application.data.status = "请至少选择一位联系人创建群聊";
        auto component = make_ui(application, []{});
        component->OnEvent(ftxui::Event::Character(' '));
        ok &= expect(application.data.picked_contacts == std::vector<std::int64_t>{2} && application.data.status.empty(),
                     "Selecting a contact resolves stale picker input feedback");
        for (int columns : {60, 70, 80, 100, 120, 160})
        {
            auto output = draw(application.data, columns, 30);
            ok &= expect(output.find("已选 1 人") != std::string::npos &&
                         output.find("请至少选择一位联系人创建群聊") == std::string::npos,
                         "A valid selection does not display the former empty-selection error");
        }
        application.data.status = "previous input feedback";
        component->OnEvent(ftxui::Event::Character(' '));
        ok &= expect(application.data.picked_contacts.empty() && application.data.status.empty(),
                     "Changing a selection clears feedback for its previous input");
        application.data.selected = -1;
        application.data.status = "unchanged feedback";
        application.toggle_pick();
        ok &= expect(application.data.picked_contacts.empty() && application.data.status == "unchanged feedback",
                     "An invalid picker action does not clear feedback without changing input");
    }
    {
        auto long_account = s;
        long_account.self.username = std::string(64, 'A');
        ftxui::Screen screen(60, 24);
        ftxui::Render(screen, render(long_account, 60, 24));
        std::string header;
        for (int x = 0; x < 60; ++x) { header += screen.CellAt(x, 1).character; }
        ok &= expect(header.find("… ● 已连接") != std::string::npos,
                     "A clipped account identity stays separated from connection status");
    }
    {
        auto output = draw(s, 60, 20);
        ok &= expect(output.find("暂无聊天") != std::string::npos &&
                     output.find("按 N 添加好友或创建群聊") != std::string::npos,
                     "An empty chat list explains its content and next action");
    }
    chat::conversation direct;
    direct.id = 10;
    direct.user = 2;
    direct.username = "张 三";
    s.conversations.push_back(direct);
    s.select_conversation(10);
    ok &= expect(draw(s, 60, 20).find("暂无消息") != std::string::npos,
                 "An empty history identifies messages instead of generic items");
    {
        auto requests = s;
        requests.view = page::requests;
        auto output = draw(requests, 60, 20);
        ok &= expect(output.find("暂无入群申请") != std::string::npos,
                     "An empty group request page identifies join requests");
        ok &= expect(output.find("y 接受") == std::string::npos,
                     "An empty request page does not offer decisions without an applicant");
    }
    {
        auto users = s;
        users.view = page::users;
        ok &= expect(draw(users, 60, 20).find("没有找到匹配的用户") != std::string::npos,
                     "An empty user search explains that its prefix has no matches");
    }
    {
        auto search = s;
        search.view = page::search;
        search.search_query = "no_such_message_prefix";
        auto empty_search = draw(search, 60, 20);
        ok &= expect(empty_search.find("已加载的结果中没有匹配项") != std::string::npos &&
                     empty_search.find("No messages match this search") == std::string::npos,
                     "An empty message search describes loaded hits rather than claiming a fresh server match set");
        search.search_query = std::string(64, 'q');
        chat::message match;
        match.id = 1; match.conversation = 10; match.from = 2; match.username = "peer"; match.text = "matching message";
        search.search_results = {match};
        for (auto [columns, rows] : {std::pair{60,20}, {70,22}, {80,24}, {100,30}, {120,40}, {160,45}})
        {
            auto output = draw(search, columns, rows);
            ok &= expect(output.find("搜索：") != std::string::npos &&
                         output.find("Enter 或 y 显示可复制文本") != std::string::npos &&
                         output.find("matching message") != std::string::npos,
                         "Long search queries do not clip the action hint or hide their selected result");
            ok &= expect(output.find("显示已加载的结果") != std::string::npos &&
                         output.find("重新搜索可获取最新匹配") != std::string::npos,
                         "Search explains loaded membership and live bodies at every supported width");
        }
        search.search_query = "matching message";
        auto edited = match;
        edited.text = "edited body without the keyword";
        edited.edited_at = 2;
        search.apply_message(edited);
        ok &= expect(draw(search, 60, 20).find(edited.text) != std::string::npos,
                     "A loaded search hit keeps its live body without client-side matching");
        edited.deleted = true;
        search.apply_message(edited);
        auto deleted_search = draw(search, 60, 20);
        ok &= expect(deleted_search.find("已加载的结果中没有匹配项") != std::string::npos &&
                     deleted_search.find(edited.text) == std::string::npos,
                     "Deleting the final hit hides it without claiming no current server matches");
    }
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
    s.contacts = {{direct.user, direct.username, {}}};
    ok &= expect(draw(s, 80, 24).find("正在刷新聊天权限") != std::string::npos, "accepted friend with stale permission shows refresh hint");
    s.contacts.clear();
    ok &= expect(draw(s, 20, 4).find("终端太小") != std::string::npos, "too small fallback");
    s.view = page::conversations;
    ok &= expect(draw(s, 60, 20).find("聊天") != std::string::npos, "narrow list navigation");
    ok &= expect(draw(s, 60, 20).find("你们目前不是好友") == std::string::npos, "narrow list does not squeeze conversation");
    {
        auto long_name_list = s;
        long_name_list.conversations.front().username = "LongUsernamePrefix" + std::string(46, 'x');
        ok &= expect(draw(long_name_list, 120, 40).find("LongUsernamePrefix") != std::string::npos, "sidebar retains long username prefix");
        ok &= expect(draw(long_name_list, 60, 20).find("LongUsernamePrefix") != std::string::npos, "narrow list retains long username prefix");
        long_name_list.conversations.front().username.clear();
        long_name_list.conversations.front().kind = chat::conversation_kind::group;
        for (int i = 0; i < 7; ++i) { long_name_list.conversations.front().username += "长中文群名称"; }
        for (int columns : {100, 160})
        {
            ftxui::Screen screen(columns, 30);
            ftxui::Render(screen, render(long_name_list, columns, 30));
            bool visible_ellipsis = false;
            for (int y = 0; y < 30; ++y)
            {
                std::string row;
                for (int x = 1; x < 30; ++x) { row += screen.CellAt(x, y).character; }
                if (row.starts_with("> 长中文") || row.starts_with("  长中文")) { visible_ellipsis = row.find("…") != std::string::npos; }
            }
            ok &= expect(visible_ellipsis, "The sidebar scroll indicator cannot hide a clipped title's ellipsis");
        }
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
            ok &= expect(output.find("置顶") != std::string::npos, "long conversation title keeps personal pin visible");
            ok &= expect(output.find("免打扰") != std::string::npos, "long conversation title keeps mute visible");
            ok &= expect(output.find(" 100") != std::string::npos, "long conversation title keeps unread count visible");
            ok &= expect(output.find("百人验") != std::string::npos, "conversation status leaves an identifiable title prefix");
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
            ok &= expect(output.find("Main summary group") != std::string::npos, "multiline latest summary keeps selected conversation title visible");
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
            ok &= expect((screen.ToString().find("> 张 三") != std::string::npos) == (view == page::conversations),
                         "The focused list row also has a text mark");
        }
    }
    s.view = page::conversation;
    auto& group = s.conversations.front();
    group.kind = chat::conversation_kind::group;
    group.member_count = 3;
    group.can_send = true;
    group.announcement = "开发公告";
    {
        auto composer = s;
        for (int columns : {60, 70, 80, 100, 120, 160})
        {
            auto empty = draw(composer, columns, 30);
            ok &= expect(empty.find("按 i 输入消息") != std::string::npos &&
                         empty.find("按 i 输入消息 ·") == std::string::npos,
                         "An empty composer hint has no dangling draft separator");
            composer.draft = "未发送 draft";
            auto retained = draw(composer, columns, 30);
            ok &= expect(retained.find("按 i 继续输入 · 未发送 draft") != std::string::npos &&
                         composer.draft == "未发送 draft",
                         "A retained draft stays distinct from its compose hint without changing its text");
            composer.draft.clear();
        }
    }
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
    {
        auto long_title = s;
        auto& current = long_title.conversations.front();
        current.username.clear();
        for (int i = 0; i < 7; ++i) { current.username += "长中文群名称"; }
        current.username += " END";
        current.announcement.clear();
        for (int columns : {60, 70, 80, 100, 120, 160})
        {
            ftxui::Screen screen(columns, 30);
            ftxui::Render(screen, render(long_title, columns, 30));
            auto output = screen.ToString();
            ok &= expect(output.find("长中文群名称") != std::string::npos &&
                         output.find("3 位成员") != std::string::npos,
                         "Long group headers retain identity and member count at every terminal width");
            if (columns < 160)
            { ok &= expect(output.find("…") != std::string::npos, "Clipped group identities have a visible ellipsis"); }
            for (int y = 1; y < 29; ++y)
            { ok &= expect(ftxui::string_width(screen.CellAt(columns - 2, y).character) != 2,
                           "A clipped wide glyph cannot consume the outer right border"); }
        }
    }
    auto output = draw(s, 120, 40);
    for (auto const* token : {"开发公告", "你好", "（已编辑）", "消息已删除", "👍", "@Alice"})
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
    {
        auto aligned = s;
        auto peer = message;
        peer.text = "PEER_MESSAGE";
        peer.reply.reset(); peer.reactions.clear(); peer.mentions.clear(); peer.edited_at.reset();
        auto own = peer;
        own.id = 2; own.from = aligned.self.id; own.text = "OWN_MESSAGE";
        aligned.messages = {peer, own};
        aligned.message_selected = 1;
        aligned.conversations.front().announcement.clear();
        for (int columns : {60, 80, 120, 160})
        {
            ftxui::Screen screen(columns, 30);
            ftxui::Render(screen, render(aligned, columns, 30));
            int peer_x = -1, own_x = -1;
            for (int y = 0; y < 30; ++y)
                for (int x = 0; x < columns; ++x)
                {
                    std::string cells;
                    for (int i = x; i < std::min(columns, x + 12); ++i) { cells += screen.CellAt(i, y).character; }
                    if (cells.starts_with("PEER_MESSAGE")) { peer_x = x; }
                    if (cells.starts_with("OWN_MESSAGE")) { own_x = x; }
                }
            ok &= expect(peer_x >= 0 && own_x > peer_x + 8, "Own messages align right and peer messages left at narrow and wide widths");
            ok &= expect(own_x - peer_x > (columns - peer_x - 2) / 2, "Short own messages shrink toward the right edge");
            ok &= expect(screen.ToString().find("我 ") != std::string::npos && screen.ToString().find("已读") != std::string::npos,
                         "Message alignment retains own heading and group read state");
        }
    }
    s.view = page::profile;
    {
        auto long_profile = s;
        for (auto id : {1, 2})
        {
            long_profile.profile = {id, std::string(60, 'A') + "_END", {}};
            for (int columns : {60, 70, 80, 100, 120, 160})
            {
                ftxui::Screen screen(columns, 24);
                ftxui::Render(screen, render(long_profile, columns, 24));
                std::string identity;
                for (int y = 0; y < 24; ++y)
                    for (int x = 1; x < columns - 1; ++x)
                        if (screen.CellAt(x, y).character != " ") { identity += screen.CellAt(x, y).character; }
                auto output = screen.ToString();
                ok &= expect(identity.find(std::string(60, 'A') + "_END") != std::string::npos &&
                             output.find("显示可复制的用户名") != std::string::npos,
                             "Own and peer profiles show the complete maximum-length identity at every width");
            }
        }
    }
    s.profile = {3, "stranger", {}};
    output = draw(s, 80, 24);
    ok &= expect(output.find("添加好友") != std::string::npos, "non-contact add action");
    ok &= expect(output.find("发消息") == std::string::npos, "non-contact cannot message");
    s.presences.emplace(3, chat::presence{3, true, 0});
    ok &= expect(draw(s, 80, 24).find("在线") == std::string::npos, "non-contact presence hidden");
    s.friends.outgoing = {{{3, "stranger", {}}, 1}};
    output = draw(s, 80, 24);
    ok &= expect(output.find("等待对方确认") != std::string::npos && output.find("撤回好友申请") != std::string::npos,
                 "outgoing pending profile can cancel");
    ok &= expect(output.find("发消息") == std::string::npos && output.find("在线") == std::string::npos,
                 "pending profile cannot message or see presence");
    s.friends.outgoing.clear();
    s.friends.incoming = {{{3, "stranger", {}}, 1}};
    output = draw(s, 80, 24);
    ok &= expect(output.find("接受好友申请") != std::string::npos && output.find("拒绝好友申请") != std::string::npos,
                 "incoming pending profile actions");
    s.view = page::contacts;
    ok &= expect(draw(s, 80, 24).find("新的朋友 (1)") != std::string::npos, "contacts expose pending friend count");
    ok &= expect(draw(s, 80, 24).find(":create-group") == std::string::npos, "Contacts header contains no group creation action");
    s.contacts = {{9, "AcceptedOnly", {}}};
    s.friends.outgoing = {{{10, "OutgoingOnly", {}}, 1}};
    output = draw(s, 80, 24);
    ok &= expect(output.find("AcceptedOnly") != std::string::npos && output.find("OutgoingOnly") == std::string::npos &&
                 output.find("stranger") == std::string::npos, "Contacts rows exclude incoming and outgoing requests");
    s.view = page::friend_requests;
    ok &= expect(draw(s, 60, 20).find("新的朋友 · 收到") != std::string::npos, "incoming requests page");
    s.view = page::friend_sent;
    ok &= expect(draw(s, 60, 20).find("新的朋友 · 发出") != std::string::npos, "outgoing requests page");
    s.contacts = {{3, "stranger", {}}, {4, "张 三", {}}};
    s.view = page::pick_contacts;
    s.pick_query = "张";
    s.picked_contacts = {4};
    output = draw(s, 80, 24);
    ok &= expect(output.find("已选 1 人") != std::string::npos && output.find("[x]") != std::string::npos && output.find("stranger") == std::string::npos,
                 "group picker filters accepted friends and shows selected count");
    s.pick_query.clear(); s.picked_contacts.clear();
    s.view = page::group;
    s.conversations.front().pinned_message = chat::quoted_message{1, 2, "张 三", "pinned", {}, false};
    s.members = {{1, "Alice", chat::member_role::member, {}}};
    {
        // The group summary is a bounded subset, not the complete member list.
        for (int count : {0, 1, 2, 3, 4, 8})
        {
            auto preview = s;
            preview.selected = 0;
            preview.status.clear();
            preview.conversations.front().username = "Subset group";
            preview.conversations.front().member_count = count;
            preview.conversations.front().announcement.clear();
            preview.conversations.front().pinned_message.reset();
            preview.members.clear();
            for (int i = 0; i < count; ++i)
            {
                preview.members.push_back({i + 1, "P" + std::to_string(i + 1),
                    i == 0 ? chat::member_role::owner : i == 1 ? chat::member_role::admin : chat::member_role::member, {}});
            }
            auto const shown = count < 3 ? count : 3;
            auto const label = "前 " + std::to_string(shown) + " 位成员（共 " + std::to_string(count) + " 位）：";
            for (int columns : {60, 70, 80, 100, 120, 160})
            {
                for (int rows : {24, 40})
                {
                    auto output = draw(preview, columns, rows);
                    ok &= expect(output.find(label) != std::string::npos,
                                 "Group preview explicitly identifies its bounded subset at every width and height");
                    ok &= expect(output.find("成员 " + std::to_string(count) + " 人 · 我的身份：") != std::string::npos &&
                                 output.find("全部成员") != std::string::npos,
                                 "Group preview retains the total count and full-members action");
                    for (int i = 0; i < count; ++i)
                    {
                        auto const role = i == 0 ? "群主" : i == 1 ? "管理员" : "成员";
                        auto const identity = "P" + std::to_string(i + 1) + " " + role;
                        ok &= expect((output.find(identity) != std::string::npos) == (i < shown),
                                     "Group preview retains the first members and roles without pretending to list the remainder");
                    }
                    if (count > 3)
                    {
                        auto full = preview;
                        full.view = page::members;
                        output = draw(full, columns, rows);
                        ok &= expect(output.find("群成员 (" + std::to_string(count) + ")") != std::string::npos &&
                                     output.find("位成员（共") == std::string::npos,
                                     "Full members remains a complete list rather than the group preview");
                        for (int i = 0; i < count; ++i)
                        {
                            ok &= expect(output.find("[P] P" + std::to_string(i + 1)) != std::string::npos,
                                         "Full members includes every member beyond the preview cap");
                        }
                    }
                }
            }
        }
        auto long_preview = s;
        long_preview.selected = 0;
        long_preview.status.clear();
        long_preview.conversations.front().username = "Subset group";
        long_preview.conversations.front().member_count = 4;
        long_preview.conversations.front().announcement.clear();
        long_preview.conversations.front().pinned_message.reset();
        long_preview.members = {{1, "中文é_" + std::string(200, 'x'), chat::member_role::owner, {}},
            {2, "P2", chat::member_role::admin, {}}, {3, "P3", chat::member_role::member, {}},
            {4, "P4", chat::member_role::member, {}}};
        for (int columns : {60, 70, 80, 100, 120, 160})
        {
            for (int rows : {24, 40})
            {
                auto output = draw(long_preview, columns, rows);
                auto const begin = output.find("前 3 位成员（共 4 位）：");
                auto const end = output.find("\r\n", begin);
                auto const line = begin == std::string::npos ? std::string{} : output.substr(begin, end - begin);
                ok &= expect(line.starts_with("前 3 位成员（共 4 位）：中文é_") && line.find("…") != std::string::npos &&
                             output.find("全部成员") != std::string::npos,
                             "Long member clipping keeps the subset label at the start and the full-members action visible");
            }
        }
    }
    {
        auto long_member = s;
        long_member.view = page::members;
        long_member.members.push_back({2, std::string(60, 'A') + "_END", chat::member_role::admin, {}});
        long_member.selected = 1;
        for (int columns : {60, 70, 80, 100, 120, 160})
        {
            auto output = draw(long_member, columns, 24);
            ok &= expect(output.find(" · 管理员") != std::string::npos,
                         "Long member identities never clip their authoritative role");
        }
    }
    ok &= expect(draw(s, 80, 24).find("邀请码") == std::string::npos, "members never see secret invite actions");
    ok &= expect(draw(s, 80, 24).find("查看完整公告") != std::string::npos, "members can read full announcement");
    ok &= expect(draw(s, 80, 24).find("查看置顶消息") != std::string::npos, "members can view pinned message");
    auto const saved_announcement = s.conversations.front().announcement;
    s.conversations.front().announcement = std::string(2000, 'A');
    ok &= expect(draw(s, 60, 20).find("查看完整公告") != std::string::npos, "long announcement preview leaves actions visible");
    s.conversations.front().announcement = saved_announcement;
    s.members.front().role = chat::member_role::owner;
    ok &= expect(draw(s, 120, 40).find("生成新邀请码") != std::string::npos, "owner management actions");
    s.view = page::help;
    ok &= expect(draw(s, 120, 40).find("Ctrl+C") != std::string::npos, "keyboard help");
    for (int columns : {60, 70, 80, 100, 120, 160})
    {
        for (int rows : {24, 45})
        {
            app application;
            application.data = s;
            application.data.view = page::contacts;
            application.command("help");
            // Events read the terminal size; the fallback stands in for it without a terminal.
            auto const fallback = ftxui::Terminal::Size();
            ftxui::Terminal::SetFallbackSize({columns, rows});
            auto component = make_ui(application, [] {});
            auto top = draw(application.data, columns, rows);
            ok &= expect(top.find("直接输入") != std::string::npos,
                         "Help opens at its first shortcut");
            for (int i = 0; i < 200; ++i) { component->OnEvent(ftxui::Event::Character('j')); }
            auto output = draw(application.data, columns, rows);
            auto const start = output.find("命令：");
            auto const end = output.find("复制：", start);
            ok &= expect(start != std::string::npos && end != std::string::npos,
                         "Keyboard scrolling reaches the complete Help command section");
            auto const commands = start == std::string::npos ? std::string{} : output.substr(start, end - start);
            for (auto command : {"new,", "chats,", "contacts,", "friend-requests,", "friend-sent,", "accept-friend,",
                                 "reject-friend,", "cancel-friend,", "filter,", "add-contact,", "profile,", "account,",
                                 "create-group,", "join,", "file,", "save,", "members,", "invite,", "rename,",
                                 "announcement,", "show-announcement,", "pinned,", "pin-message,", "unpin-message,",
                                 "link,", "link-create,", "link-revoke,", "approval,", "requests,", "avatar,",
                                 "avatar-clear,", "logout,", "quit"})
            {
                ok &= expect(commands.find(command) != std::string::npos,
                             "Every Help command remains whole and visible at the bottom");
            }
            if (rows == 24)
            {
                ok &= expect(output.find("直接输入") == std::string::npos,
                             "Help scrolls its body rather than keeping the first shortcut pinned");
            }
            for (int i = 0; i < 200; ++i) { component->OnEvent(ftxui::Event::Character('k')); }
            ok &= expect(application.data.selected == 0 && draw(application.data, columns, rows) == top,
                         "Help keyboard scrolling returns to its original viewport");
            component->OnEvent(ftxui::Event::Escape);
            ok &= expect(application.data.view == page::contacts,
                         "Escape leaves Help for its parent page");
            ftxui::Terminal::SetFallbackSize(fallback);
        }
    }
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
    long_message.text += "\nMIDDLE\n";
    for (int i = 0; i < 900; ++i) { long_message.text += "文"; }
    long_message.text += "\nTAIL";
    ok &= expect(draw(s, 60, 20, 0).find("HEAD") != std::string::npos, "long unspaced message beginning wraps");
    ok &= expect(draw(s, 60, 20, -1).find("TAIL") != std::string::npos, "long message tail scrolls into view");
    bool middle_visible = false;
    for (int line = 0; line < 100 && !middle_visible; ++line)
    { middle_visible = draw(s, 60, 20, line).find("MIDDLE") != std::string::npos; }
    ok &= expect(middle_visible, "long message middle scrolls into view");
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
            ok &= expect(text.find("新的朋友 (5)") != std::string::npos, "New friends entry remains at Contacts top while scrolling accepted rows");
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
                ok &= expect(text.find("群成员 (100)") != std::string::npos && text.find("Member" + std::to_string(index)) != std::string::npos,
                             "Hundred member navigation retains header and selected member at every width");
            }
        }
    }
    {
        app application;
        writable_conversation(application);
        auto component = make_ui(application, [] {});
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        component->OnEvent(ftxui::Event::Character("第一行"));
        component->OnEvent(ftxui::Event::Return);
        component->OnEvent(ftxui::Event::Character("second line 🙂"));
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        ok &= expect(application.data.draft == "第一行\nsecond line 🙂", "Multiline paste remains intact in the draft");
        ok &= expect(application.data.status.empty(), "Pasted Return does not submit the message");
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        component->OnEvent(ftxui::Event::Character(" kept"));
        component->OnEvent(ftxui::Event::Escape);
        component->OnEvent(ftxui::Event::Character(":quit"));
        component->OnEvent(ftxui::Event::Return);
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        ok &= expect(application.data.draft == "第一行\nsecond line 🙂 kept" && !application.exiting,
                     "Cancelled paste discards remaining input without executing shortcuts");
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        application.data.active = 20;
        application.data.draft = "other conversation";
        component->OnEvent(ftxui::Event::Character("wrong target"));
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        ok &= expect(application.data.draft == "other conversation", "Interrupted paste cannot follow a changed conversation");
        application.data.active = 10;
        component->OnEvent(ftxui::Event::Custom);
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        application.data.link = connection::reconnecting;
        component->OnEvent(ftxui::Event::Custom);
        component->OnEvent(ftxui::Event::Character("N"));
        component->OnEvent(ftxui::Event::Return);
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        ok &= expect(application.data.draft == "other conversation" && application.data.view == page::conversation,
                     "Connection loss discards pasted tail rather than opening another page");
        application.command_mode = true;
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        component->OnEvent(ftxui::Event::Character("quit"));
        component->OnEvent(ftxui::Event::Return);
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        ok &= expect(!application.exiting && application.command_text == "quit ", "Pasted newline cannot execute a command");
    }
    {
        // A large paste is inserted in batches: the text appears as the screen redraws, and the
        // total cost stays linear (character-by-character insertion took over 10 s here).
        app application;
        writable_conversation(application);
        auto component = make_ui(application, [] {});
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        ftxui::Screen screen(100, 30);
        bool visible_during_paste = false;
        for (int i = 0; i < 30000; ++i)
        {
            component->OnEvent(ftxui::Event::Character('x'));
            if (i % 512 == 511)
            {
                ftxui::Render(screen, component->Render());
                visible_during_paste |= application.data.draft.size() == static_cast<std::size_t>(i + 1);
            }
        }
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        ok &= expect(visible_during_paste, "Pasted text reaches the composer before the paste ends");
        ok &= expect(application.data.draft == std::string(30000, 'x'), "A large paste arrives complete");
    }
    {
        app application;
        auto component = make_ui(application, [] {});
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        component->OnEvent(ftxui::Event::Character("Alice"));
        component->OnEvent(ftxui::Event::Return);
        component->OnEvent(ftxui::Event::Character("Bob"));
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        ok &= expect(application.username == "Alice Bob", "Single-line fields insert pasted newline as space");
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        component->OnEvent(ftxui::Event::Character("secret"));
        component->OnEvent(ftxui::Event::Return);
        component->OnEvent(ftxui::Event::Character("password"));
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        ok &= expect(application.password == "secret password" && application.data.link == connection::signed_out,
                     "Password paste cannot trigger authentication");
    }
    // Real FTXUI input and routing: no terminal or network is needed.
    {
        app application;
        bool quit = false;
        auto component = make_ui(application, [&] { quit = true; });
        component->OnEvent(ftxui::Event::Character("张三"));
        ok &= expect(application.username == "张三", "login Unicode input");
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Character("secret-password"));
        ftxui::Screen login_screen(80, 24);
        ftxui::Render(login_screen, component->Render());
        ok &= expect(login_screen.ToString().find("登录 / 注册") != std::string::npos &&
                     login_screen.ToString().find(application.server_url) == std::string::npos,
                     "Login hides server configuration until explicitly expanded");
        ok &= expect(login_screen.ToString().find("secret-password") == std::string::npos, "password hidden");
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Return);
        ftxui::Screen settings_screen(80, 24);
        ftxui::Render(settings_screen, component->Render());
        ok &= expect(settings_screen.ToString().find(application.server_url) != std::string::npos,
                     "Server settings reveal the authoritative connection address");
        component->OnEvent(ftxui::Event::End);
        component->OnEvent(ftxui::Event::Character("/chosen"));
        component->OnEvent(ftxui::Event::Escape);
        component->OnEvent(ftxui::Event::End);
        component->OnEvent(ftxui::Event::Character("四"));
        ok &= expect(application.server_url.ends_with("/chosen") && application.username == "张三四" &&
                     application.password == "secret-password", "Closing server settings preserves values and restores username focus");
        application.data.self = {1, "Alice", {}};
        application.data.view = page::contacts;
        application.data.contacts = {{2, "Bob", {}}, {3, "张三", {}}};
        component->OnEvent(ftxui::Event::Character('j'));
        ok &= expect(application.data.selected == 1, "contact keyboard selection");
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.view == page::profile && application.data.profile.id == 2, "contact profile navigation");
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.view == page::contacts, "Esc returns from profile");
        application.command("filter bOB");
        output = draw(application.data, 80, 24);
        ok &= expect(output.find("Bob") != std::string::npos && output.find("张三") == std::string::npos,
                     "Contacts local substring search ignores ASCII case");
        ok &= expect(output.find("新的朋友 (") != std::string::npos && output.find("/ 搜索") != std::string::npos,
                     "Contacts filter keeps New friends and search hint visible");
        component->OnEvent(ftxui::Event::Character('j'));
        component->OnEvent(ftxui::Event::Character('j'));
        ok &= expect(application.data.selected == 1, "Filtered Contacts keyboard selection stays in visible rows");
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.view == page::profile && application.data.profile.id == 2, "Filtered Enter opens the visible friend's profile");
        component->OnEvent(ftxui::Event::Escape);
        component->OnEvent(ftxui::Event::Character('/'));
        ok &= expect(application.dialog.has_value(), "Contacts slash opens local friend search");
        if (application.dialog)
        {
            application.dialog->text.clear();
            application.submit_prompt();
        }
        output = draw(application.data, 80, 24);
        ok &= expect(output.find("Bob") != std::string::npos && output.find("张三") != std::string::npos, "Clear Contacts filter restores accepted friends");
        application.data.conversations = {direct};
        application.data.active = direct.id;
        application.data.friends.outgoing = {{{direct.user, direct.username, {}}, 1}};
        application.data.draft = "read-only draft";
        for (int width : {120, 70, 120, 70})
        {
            application.viewport_width = width;
            application.navigate(page::conversations);
            application.navigate(page::conversation);
            // Without a server every refresh reports an error; Esc would first clear it.
            application.dismiss_error();
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
        ok &= expect(output.find("添加好友") != std::string::npos && output.find("创建群聊") != std::string::npos &&
                     output.find("加入群聊") != std::string::npos, "New menu exposes existing three actions");
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
        ok &= expect(output.find("账号 · ") != std::string::npos && output.find("退出登录") != std::string::npos,
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
    {
        // Errors stay until the next key press; notices are not errors and are not dismissed by keys.
        app application;
        application.data.self = {1, "Alice", {}};
        application.data.link = connection::online;
        auto component = make_ui(application, [] {});
        application.data.status = "请先选择消息";
        component->OnEvent(ftxui::Event::Custom);
        ok &= expect(application.data.status_error && application.data.status == "请先选择消息",
                     "A directly set status is an error that survives redraws");
        ok &= expect(draw(application.data, 80, 24).find("请先选择消息") != std::string::npos, "An error is shown in the status bar");
        component->OnEvent(ftxui::Event::Character('j'));
        ok &= expect(application.data.status.empty() && !application.data.status_error, "The next key press dismisses an error");
        application.notify("消息已发送");
        component->OnEvent(ftxui::Event::Character('j'));
        ok &= expect(application.data.status == "消息已发送" && !application.data.status_error,
                     "A success notice is not an error and is not dismissed by keys");
        ok &= expect(chat::error_text({chat::error_kind::rpc, -32009, "Group permission denied"}) == "没有执行此操作的群权限",
                     "Server errors are shown in Chinese");
    }
    {
        // History shows a separator before each new day.
        auto days = s;
        days.view = page::conversation;
        // Noon today and noon yesterday on the local calendar, independent of DST.
        auto noon = [](int days_ago) {
            auto now = std::time(nullptr);
            std::tm day{};
            localtime_r(&now, &day);
            day.tm_mday -= days_ago; day.tm_hour = 12; day.tm_min = day.tm_sec = 0; day.tm_isdst = -1;
            return static_cast<std::int64_t>(std::mktime(&day)) * 1000;
        };
        auto const now = noon(0);
        chat::message earlier;
        earlier.id = 1; earlier.conversation = 10; earlier.from = 2; earlier.username = "peer";
        earlier.text = "YESTERDAY_BODY"; earlier.timestamp = noon(1);
        auto later = earlier;
        later.id = 2; later.text = "TODAY_BODY"; later.timestamp = now;
        days.messages = {earlier, later};
        days.message_selected = 1;
        auto output = draw(days, 120, 40);
        ok &= expect(output.find("── 昨天 ──") != std::string::npos && output.find("── 今天 ──") != std::string::npos &&
                     output.find("── 昨天 ──") < output.find("YESTERDAY_BODY") &&
                     output.find("YESTERDAY_BODY") < output.find("── 今天 ──") &&
                     output.find("── 今天 ──") < output.find("TODAY_BODY"),
                     "Day separators precede the first message of each day");
    }
    {
        // Phase 2 focus: an open, writable conversation takes typing directly.
        app application;
        writable_conversation(application);
        // Events read the terminal size, so the layout is chosen through the fallback size.
        auto const fallback = ftxui::Terminal::Size();
        ftxui::Terminal::SetFallbackSize({120, 40});
        auto component = make_ui(application, [] {});
        auto type = [&](std::string const& value) { component->OnEvent(ftxui::Event::Character(value)); };
        // Without a server each refresh reports an error, which Esc would clear first.
        auto escape = [&] { application.dismiss_error(); component->OnEvent(ftxui::Event::Escape); };
        for (auto key : {"?", ":", "/", "r", "N", "h", "i"}) { type(key); }
        ok &= expect(application.data.draft == "?:/rNhi" && application.data.view == page::conversation &&
                     !application.command_mode && application.data.composing,
                     "Printable keys, including ? : / and shortcut letters, are message text while typing");
        application.data.reply = chat::quoted_message{5, 2, "peer", "quoted", {}, false};
        escape();
        ok &= expect(!application.data.reply && application.data.composing && application.data.draft == "?:/rNhi",
                     "Esc first cancels a reply and keeps the draft");
        component->OnEvent(ftxui::Event::TabReverse);
        ok &= expect(!application.data.composing && application.data.selecting, "Shift+Tab moves from the composer to the messages");
        escape();
        ok &= expect(application.data.composing && !application.data.selecting, "Esc returns from the messages to the composer");
        component->OnEvent(ftxui::Event::ArrowUp);
        ok &= expect(application.data.composing, "Up with a draft stays in the composer");
        application.data.draft.clear();
        component->OnEvent(ftxui::Event::ArrowUp);
        ok &= expect(application.data.selecting && !application.data.composing, "Up in an empty composer selects messages");
        component->OnEvent(ftxui::Event::Tab);
        ok &= expect(application.data.composing, "Tab moves from the messages to the composer");
        component->OnEvent(ftxui::Event::PageUp);
        ok &= expect(application.data.selecting && !application.data.at_latest, "PgUp in the composer browses older messages");
        component->OnEvent(ftxui::Event::Tab);
        component->OnEvent(ftxui::Event::Tab);
        ok &= expect(application.data.view == page::conversations, "Wide Tab moves from the composer to the list");
        component->OnEvent(ftxui::Event::Tab);
        ok &= expect(application.data.view == page::conversation && application.data.selecting, "Wide Tab moves from the list to the messages");
        escape();
        application.data.draft = "保留";
        component->OnEvent(ftxui::Event::F1);
        ok &= expect(application.data.view == page::help, "F1 opens help from the composer");
        escape();
        ok &= expect(application.data.view == page::conversation && application.data.composing && application.data.draft == "保留",
                     "Leaving help returns to the composer with its draft");
        ftxui::Terminal::SetFallbackSize({70, 24});
        component->OnEvent(ftxui::Event::Tab);
        ok &= expect(application.data.view == page::conversation && application.data.selecting, "Narrow Tab moves to the messages");
        component->OnEvent(ftxui::Event::Tab);
        ok &= expect(application.data.view == page::conversation && application.data.composing, "Narrow Tab returns to the composer");
        escape();
        ok &= expect(application.data.view == page::conversations, "Esc from the composer returns to the list");
        ftxui::Terminal::SetFallbackSize({120, 40});
        application.navigate(page::conversation);
        component->OnEvent(ftxui::Event::Custom);
        application.data.status = "请先选择消息";
        component->OnEvent(ftxui::Event::Custom);
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.status.empty() && application.data.view == page::conversation && application.data.composing,
                     "Esc with an error only clears the error");
        ok &= expect(draw(application.data, 120, 40).find("\\ Enter 换行") != std::string::npos,
                     "The key bar shows how to add a line while typing");

        // Backslash before Enter adds a line; nothing else rewrites the text.
        auto reset = [&](std::string draft) {
            application.data.draft = std::move(draft);
            component->OnEvent(ftxui::Event::End);
        };
        reset("a");
        type("\\");
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "a\n", "A typed backslash then Enter becomes a line break");
        reset("");
        type("\\"); type("\\"); type("\\");
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "\\\\\n", "Only the last of several backslashes becomes a line break");
        reset("ab");
        component->OnEvent(ftxui::Event::ArrowLeft);
        type("\\");
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "a\nb", "A backslash in the middle becomes a line break in place");
        reset("abc");
        component->OnEvent(ftxui::Event::ArrowLeft);
        component->OnEvent(ftxui::Event::ArrowLeft);
        component->OnEvent(ftxui::Event::Insert);
        type("\\");
        component->OnEvent(ftxui::Event::Return);
        component->OnEvent(ftxui::Event::Insert);
        ok &= expect(application.data.draft == "a\nc", "Overwrite mode replaces only the backslash with the line break");
        reset("x");
        type("\\");
        component->OnEvent(ftxui::Event::Custom);
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "x\n", "A redraw between backslash and Enter keeps the line break");
        reset("y");
        type("\\");
        component->OnEvent(ftxui::Event::ArrowRight);
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "y\\", "Any other key, even one that does not move, leaves the backslash as typed");
        reset("");
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        type("p\\");
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "p\\", "A pasted trailing backslash is sent as it is");
        reset("z");
        type("\\");
        application.data.draft = "changed\\";
        component->OnEvent(ftxui::Event::Custom);
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "changed\\", "A program change to the draft cancels the pending line break");
        reset("q");
        component->OnEvent(ftxui::Event::Special("\x1b\r"));
        ok &= expect(application.data.draft == "q\n", "Alt+Enter adds a line");
        // Esc after a backslash that only clears an error still cancels the line break.
        reset("e");
        type("\\");
        application.data.status = "请先选择消息";
        component->OnEvent(ftxui::Event::Custom);
        component->OnEvent(ftxui::Event::Escape);
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "e\\", "Esc that clears an error also cancels a pending line break");
        // An empty paste and a pasted double backslash never become a line break.
        reset("f");
        type("\\");
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "f\\", "An empty paste cancels a pending line break");
        reset("");
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        type("g\\\\");
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        component->OnEvent(ftxui::Event::Return);
        ok &= expect(application.data.draft == "g\\\\", "A pasted double backslash stays as it is");
        // Overwrite mode gives the same text however the paste is split by redraws.
        for (bool redraw : {false, true})
        {
            reset("abcd");
            component->OnEvent(ftxui::Event::Home);
            component->OnEvent(ftxui::Event::Insert);
            component->OnEvent(ftxui::Event::Special("\x1b[200~"));
            type("X");
            if (redraw) { ftxui::Screen frame(120, 40); ftxui::Render(frame, component->Render()); }
            type("Y"); type("Z");
            component->OnEvent(ftxui::Event::Special("\x1b[201~"));
            component->OnEvent(ftxui::Event::Insert);
            ok &= expect(application.data.draft == "XYZd", "An overwrite-mode paste replaces one glyph per pasted glyph");
        }
        // A combining mark split from its base by a redraw still makes one glyph.
        for (bool redraw : {false, true})
        {
            reset("abcd");
            component->OnEvent(ftxui::Event::Home);
            component->OnEvent(ftxui::Event::Insert);
            component->OnEvent(ftxui::Event::Special("\x1b[200~"));
            type("e");
            if (redraw) { ftxui::Screen frame(120, 40); ftxui::Render(frame, component->Render()); }
            type("\xcc\x81"); type("f");
            component->OnEvent(ftxui::Event::Special("\x1b[201~"));
            component->OnEvent(ftxui::Event::Insert);
            ok &= expect(application.data.draft == "e\xcc\x81" "fcd", "Overwrite paste keeps grapheme boundaries across redraws");
        }
        // Joiners and modifiers split from their base by a redraw also make one glyph.
        for (auto [base, rest, joined] : {std::tuple{std::string("\xf0\x9f\x91\xa9\xe2\x80\x8d"), std::string("\xf0\x9f\x92\xbb"),
                                                     std::string("\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb")},
                                          std::tuple{std::string("\xf0\x9f\x91\x8d"), std::string("\xf0\x9f\x8f\xbd"),
                                                     std::string("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd")}})
        {
            reset("abcd");
            component->OnEvent(ftxui::Event::Home);
            component->OnEvent(ftxui::Event::Insert);
            component->OnEvent(ftxui::Event::Special("\x1b[200~"));
            type(base);
            { ftxui::Screen frame(120, 40); ftxui::Render(frame, component->Render()); }
            type(rest);
            component->OnEvent(ftxui::Event::Special("\x1b[201~"));
            component->OnEvent(ftxui::Event::Insert);
            ok &= expect(application.data.draft == joined + "bcd", "Overwrite paste keeps ZWJ and modifier sequences whole");
        }
        // The held last glyph reaches the draft when the composer closes during the paste.
        reset("abcd");
        component->OnEvent(ftxui::Event::Home);
        component->OnEvent(ftxui::Event::Insert);
        component->OnEvent(ftxui::Event::Special("\x1b[200~"));
        type("X"); type("Y"); type("Z");
        { ftxui::Screen frame(120, 40); ftxui::Render(frame, component->Render()); }
        // As a disconnect would: the composer closes and stays closed for the rest of the paste.
        application.stop_composing();
        application.data.selecting = true;
        component->OnEvent(ftxui::Event::Special("\x1b[201~"));
        ok &= expect(application.data.draft == "XYZd", "A paste interrupted by closing the composer keeps every pasted glyph");
        application.data.selecting = false;
        component->OnEvent(ftxui::Event::Custom);
        component->OnEvent(ftxui::Event::Insert);
        // Esc order: an open dialog closes before an error is cleared.
        application.data.draft.clear();
        application.data.status = "请先选择消息";
        component->OnEvent(ftxui::Event::Custom);
        application.command("logout");
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(!application.dialog && application.data.status == "请先选择消息", "Esc closes a dialog and keeps the error");
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.status.empty() && application.data.view == page::conversation, "The next Esc clears the error");
        // On the wide list, Esc also cancels a reply first.
        application.data.reply = chat::quoted_message{5, 2, "peer", "quoted", {}, false};
        component->OnEvent(ftxui::Event::Tab);
        ok &= expect(application.data.view == page::conversations && application.data.reply, "Tab to the list keeps the reply");
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.view == page::conversations && !application.data.reply, "Esc on the list cancels the reply first");
        application.navigate(page::conversation);
        component->OnEvent(ftxui::Event::Custom);
        // In the messages, Esc cancels a reply before returning to the composer.
        component->OnEvent(ftxui::Event::TabReverse);
        application.data.reply = chat::quoted_message{5, 2, "peer", "quoted", {}, false};
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(!application.data.reply && application.data.selecting, "Esc in the messages cancels a reply first");
        escape();
        ok &= expect(application.data.composing, "The next Esc returns to the composer");
        ftxui::Terminal::SetFallbackSize(fallback);
    }
    {
        // A notice that expires in the same batch never clears a newer error.
        app application;
        writable_conversation(application);
        application.notice_duration = std::chrono::milliseconds(0);
        application.notify("消息已发送");
        application.data.status = "发送失败";
        application.tick();
        ok &= expect(application.data.status == "发送失败", "An expiring notice leaves a newer error in place");
        application.notify("消息已发送");
        application.tick();
        ok &= expect(application.data.status.empty(), "An expired notice is cleared");
    }
    {
        // The composer shrinks before the history does, and nothing is read with no history row.
        auto const fallback = ftxui::Terminal::Size();
        app application;
        writable_conversation(application);
        application.data.conversations.front().pinned_message = chat::quoted_message{1, 2, "peer", "pinned", {}, false};
        application.data.draft = "L1\nL2\nL3\nL4";
        auto component = make_ui(application, [] {});
        auto frame = [&](int columns, int rows) {
            ftxui::Terminal::SetFallbackSize({columns, rows});
            component->OnEvent(ftxui::Event::Custom);
            ftxui::Screen screen(columns, rows);
            ftxui::Render(screen, component->Render());
            return screen.ToString();
        };
        auto roomy = frame(120, 40);
        ok &= expect(roomy.find("L1") != std::string::npos && roomy.find("L4") != std::string::npos, "A roomy composer shows four lines");
        auto tight = frame(60, 16);
        int visible = 0;
        for (auto line : {"L1", "L2", "L3", "L4"}) { visible += tight.find(line) != std::string::npos; }
        ok &= expect(visible == 1 && application.history_rows >= 1, "A tight composer returns to one line");
        application.data.reply = chat::quoted_message{1, 2, "peer", "quoted", {}, false};
        chat::message latest;
        latest.id = 3; latest.conversation = 10; latest.from = 2; latest.text = "UNSEEN";
        application.data.messages = {latest};
        frame(40, 12);
        ok &= expect(application.history_rows < 1, "Pinned, reply and composer can leave no history row at 40x12");
        // A read request is decided after the event with the current rows: none here, so the
        // (absent) client is never used.
        application.mark_visible_read();
        component->OnEvent(ftxui::Event::Custom);
        ok &= expect(!application.read_check, "A read request is decided after the event");
        ftxui::Terminal::SetFallbackSize(fallback);
    }
    {
        // Help is a look-up: the page behind it keeps its selection.
        app application;
        application.data.self = {1, "Alice", {}};
        application.data.contacts = {{2, "Bob", {}}, {3, "Carol", {}}, {4, "Dan", {}}};
        application.data.view = page::contacts;
        application.data.selected = 2;
        auto component = make_ui(application, [] {});
        component->OnEvent(ftxui::Event::F1);
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.view == page::contacts && application.data.selected == 2, "Leaving help keeps the list selection");
        component->OnEvent(ftxui::Event::F1);
        component->OnEvent(ftxui::Event::F1);
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.view == page::contacts && application.data.selected == 2, "Help opened twice still returns to the selection");
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
    ok &= check_real_draw_blank_preerase();
    return ok ? 0 : 1;
}
