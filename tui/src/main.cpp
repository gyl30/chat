#include "app.hpp"
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
        std::cerr << "Usage: chat_tui [server-url]\n";
        return 1;
    }
    if (argc == 2)
    {
        const std::string_view arg(argv[1]);
        if (arg == "--help")
        {
            std::cout << "Usage: chat_tui [server-url]\n"
                         "Default: ws://127.0.0.1:18080/ws\n"
                         "Enter credentials in the login page. Press ? for keyboard help.\n";
            return 0;
        }
        if (arg == "--version")
        {
            std::cout << "chat_tui (FTXUI 7.0.3)\n";
            return 0;
        }
        if (arg.starts_with("-"))
        {
            std::cerr << "Unknown option\n";
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
    if (argc == 2) { application.server_url = argv[1]; }
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
