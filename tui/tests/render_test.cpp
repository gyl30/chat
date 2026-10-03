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
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}
}

int main()
{
    bool ok = true;
    ok &= expect(ftxui::string_width("张 三") == 5, "CJK uses terminal cells");
    for (const auto& [columns, rows] : {std::pair{80, 24}, {100, 30}, {120, 40}, {60, 20}, {20, 4}})
    {
        ftxui::Screen screen(columns, rows);
        auto page = ftxui::vbox({ftxui::text("Chat"), ftxui::text("张 三: hello")}) | ftxui::border;
        ftxui::Render(screen, page);
        const auto rendered = screen.ToString();
        ok &= expect(rendered.find("Chat") != std::string::npos, "title remains visible");
        ok &= expect(rendered.find("张 三") != std::string::npos, "Unicode remains visible");
    }
    // FTXUI text must not pass untrusted terminal escapes to the output.
    ftxui::Screen screen(50, 1);
    ftxui::Render(screen, ftxui::text("hello\x1b[2J\a\xc2\x9b"));
    ok &= expect(screen.ToString().find('\x1b') == std::string::npos, "ESC is filtered");
    ok &= expect(screen.ToString().find('\a') == std::string::npos, "BEL is filtered");
    ok &= expect(screen.ToString().find("\xc2\x9b") == std::string::npos, "C1 control is filtered");
    return ok ? 0 : 1;
}
