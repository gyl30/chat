#include <iostream>
#include <string>
#include <string_view>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

int main(int argc, char** argv)
{
    std::string server_url = "ws://127.0.0.1:18080/ws";
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
                         "Ctrl+C / Esc: quit\n";
            return 0;
        }
        if (arg == "--version")
        {
            std::cout << "chat_tui (FTXUI 7.0.3)\n";
            return 0;
        }
        if (arg.starts_with("-"))
        {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
        server_url = arg;
    }
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(false);
    auto page = ftxui::Renderer([&] {
        return ftxui::vbox({
            ftxui::text("Chat TUI") | ftxui::bold,
            ftxui::separator(),
            ftxui::text(server_url),
            ftxui::text("Terminal client foundation"),
            ftxui::text("Ctrl+C / Esc: quit"),
        }) | ftxui::border;
    });
    page |= ftxui::CatchEvent([&](ftxui::Event event) {
        if (event == ftxui::Event::CtrlC || event == ftxui::Event::Escape)
        {
            screen.Exit();
            return true;
        }
        return false;
    });
    screen.Loop(page);
}
