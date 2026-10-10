#include "app.hpp"
#include "recent_login.hpp"
#include "ui.hpp"

#include <iostream>
#include <csignal>
#include <string_view>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

int main(int argc, char** argv)
{
    if (argc > 2)
    {
        std::cerr << "用法：chat_tui [服务器地址]\n";
        return 1;
    }
    if (argc == 2)
    {
        const std::string_view arg(argv[1]);
        if (arg == "--help")
        {
            std::cout << "用法：chat_tui [服务器地址]\n"
                         "默认：ws://127.0.0.1:18080/ws\n"
                         "在登录页输入账号和密码；登录后按 F1 查看键盘帮助，Ctrl+K 打开命令面板。\n"
                         "聊天输入框中 ? 和 : 都是正文。\n";
            return 0;
        }
        if (arg == "--version")
        {
            std::cout << "chat_tui (FTXUI 7.0.3)\n";
            return 0;
        }
        if (arg.starts_with("-"))
        {
            std::cerr << "未知选项\n";
            return 1;
        }
    }
    // Screen outlives the SDK, its callback queue and every wake source.
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(false);
    screen.ForceHandleCtrlZ(false);
    // Keep native terminal selection available for copyable text pages.
    screen.TrackMouse(false);
    chat::tui::app application([&screen] { screen.PostEvent(ftxui::Event::Custom); });
    // An address given on the command line wins; otherwise the last one that signed in.
    if (auto const recent = chat::tui::load_recent_login())
    {
        application.username = recent->username;
        if (!recent->server_url.empty()) { application.server_url = recent->server_url; }
    }
    if (argc == 2) { application.server_url = argv[1]; }
    application.on_signed_in = [](std::string const& username, std::string const& server) {
        chat::tui::save_recent_login({username, server});
    };
    auto ui = chat::tui::make_ui(application, [&screen] { screen.Exit(); });
    auto terminal = ftxui::CatchEvent(ui, [&](ftxui::Event event) {
        if (event != ftxui::Event::CtrlZ) { return false; }
        if (ui->OnEvent(event)) { return true; }
        std::cout << "\x1b[?2004l" << std::flush;
        screen.WithRestoredIO([] { std::raise(SIGTSTP); })();
        std::cout << "\x1b[?2004h" << std::flush;
        return true;
    });
    std::cout << "\x1b[?2004h" << std::flush;
    screen.Loop(terminal);
    std::cout << "\x1b[?2004l" << std::flush;
    application.shutdown();
}
