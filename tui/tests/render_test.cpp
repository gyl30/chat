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
        ok &= expect(output.find("添加联系人后可发送消息") != std::string::npos, "read-only direct prompt");
    }
    ok &= expect(draw(s, 20, 4).find("Terminal too small") != std::string::npos, "too small fallback");
    s.view = page::conversations;
    ok &= expect(draw(s, 60, 20).find("Conversations") != std::string::npos, "narrow list navigation");
    ok &= expect(draw(s, 60, 20).find("添加联系人") == std::string::npos, "narrow list does not squeeze conversation");
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
    s.view = page::profile;
    s.profile = {3, "stranger", {}};
    output = draw(s, 80, 24);
    ok &= expect(output.find("Add contact") != std::string::npos, "non-contact add action");
    ok &= expect(output.find("Message") == std::string::npos, "non-contact cannot message");
    s.presences.emplace(3, chat::presence{3, true, 0});
    ok &= expect(draw(s, 80, 24).find("online") == std::string::npos, "non-contact presence hidden");
    s.view = page::group;
    s.members = {{1, "Alice", chat::member_role::member, {}}};
    ok &= expect(draw(s, 80, 24).find("invitation link") == std::string::npos, "members never see secret link actions");
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
        ok &= expect(application.data.view == page::profile && application.data.profile.id == 3, "contact profile navigation");
        component->OnEvent(ftxui::Event::Escape);
        ok &= expect(application.data.view == page::contacts, "Esc returns from profile");
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
