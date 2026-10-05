#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string_internal.hpp>
#include <utf8proc.h>

namespace {
int failures = 0;
void check(bool condition, const std::string& name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

void check_boundaries(const std::string& text, const std::vector<std::size_t>& expected,
                      const std::string& name) {
    check(ftxui::GlyphCount(text) == static_cast<int>(expected.size()) - 1, name + " count");
    for (std::size_t at = 0; at <= text.size(); ++at) {
        const auto next = std::upper_bound(expected.begin(), expected.end(), at);
        const auto previous = std::lower_bound(expected.begin(), expected.end(), at);
        const auto next_byte = next == expected.end() ? text.size() : *next;
        const auto previous_byte = previous == expected.begin() ? 0 : *std::prev(previous);
        check(ftxui::GlyphNext(text, at) == next_byte, name + " next " + std::to_string(at));
        check(ftxui::GlyphPrevious(text, at) == previous_byte, name + " previous " + std::to_string(at));
        check(ftxui::GlyphIterate(text, 1, at) == next_byte, name + " iterate next");
        check(ftxui::GlyphIterate(text, -1, at) == previous_byte, name + " iterate previous");
        check(ftxui::GlyphIterate(text, 0, at) == at, name + " zero offset");
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        for (int offset = -static_cast<int>(expected.size());
             offset <= static_cast<int>(expected.size()); ++offset) {
            const auto destination = std::clamp(static_cast<int>(index) + offset,
                                                0, static_cast<int>(expected.size()) - 1);
            check(ftxui::GlyphIterate(text, offset, expected[index]) == expected[destination],
                  name + " iterate boundary");
        }
    }
}

void official_boundaries(const char* path) {
    std::ifstream data(path);
    check(data.is_open(), "official fixture opens");
    std::string line;
    int cases = 0;
    while (std::getline(data, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream tokens(line);
        std::string token;
        std::string text;
        std::vector<std::size_t> boundaries;
        while (tokens >> token) {
            if (token == "÷") {
                boundaries.push_back(text.size());
            } else if (token != "×") {
                std::uint32_t codepoint = 0;
                std::istringstream number(token);
                number >> std::hex >> codepoint;
                std::array<utf8proc_uint8_t, 4> encoded{};
                const auto bytes = utf8proc_encode_char(static_cast<utf8proc_int32_t>(codepoint), encoded.data());
                check(!number.fail() && bytes > 0, "official codepoint " + token);
                if (bytes > 0) text.append(reinterpret_cast<const char*>(encoded.data()), static_cast<std::size_t>(bytes));
            }
        }
        if (boundaries.empty()) continue;
        ++cases;
        check_boundaries(text, boundaries, "official " + std::to_string(cases));
    }
    check(cases == 853, "Unicode 18 official fixture contains 853 cases");
    std::cout << "OFFICIAL_CASES=" << cases << '\n';
}

struct view {
    std::string ansi;
    int x;
    int y;
};
view render(const ftxui::Component& input) {
    ftxui::Screen screen(64, 6);
    ftxui::Render(screen, input->Render());
    return {screen.ToString(), screen.cursor().x, screen.cursor().y};
}
int occurrences(std::string_view text, std::string_view needle) {
    int count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string_view::npos; at += needle.size()) ++count;
    return count;
}

void editing_families() {
    const std::array<std::string_view, 11> clusters{
        "a", "é", "中́", "中́̈", "👩‍💻", "👋🏽", "❤️", "1️⃣", "🇨🇳", "👨‍👩‍👧‍👦", "क्ष"};
    for (const auto cluster_view : clusters) {
        const std::string cluster(cluster_view);
        const std::string original = "L" + cluster + "R";
        const auto cluster_end = 1 + static_cast<int>(cluster.size());
        for (int action = 0; action < 4; ++action) {
            std::string text = original;
            int cursor = action % 2 == 0 ? 1 : cluster_end;
            ftxui::InputOption option;
            option.cursor_position = &cursor;
            option.multiline = true;
            auto input = ftxui::Input(&text, option);
            input->TakeFocus();
            const std::array events{ftxui::Event::ArrowRight, ftxui::Event::ArrowLeft,
                                    ftxui::Event::Delete, ftxui::Event::Backspace};
            check(input->OnEvent(events[action]), cluster + " event handled");
            check(cursor == (action == 0 ? cluster_end : 1), cluster + " whole-cluster cursor");
            check(text == (action < 2 ? original : "LR"), cluster + " whole-cluster edit");
            check(render(input).ansi.find(text) != std::string::npos, cluster + " render retains raw bytes");
        }
        std::string text = original;
        int cursor = 1;
        bool insert = false;
        bool password = false;
        ftxui::InputOption option;
        option.cursor_position = &cursor;
        option.insert = &insert;
        option.password = &password;
        option.multiline = true;
        auto input = ftxui::Input(&text, option);
        input->TakeFocus();
        check(input->OnEvent(ftxui::Event::Character("X")), cluster + " overwrite handled");
        check(text == "LXR" && cursor == 2, cluster + " overwrite whole cluster");
        insert = true;
        text = original;
        cursor = static_cast<int>(text.size());
        password = true;
        const auto masked = render(input);
        check(occurrences(masked.ansi, "•") == 3 && masked.x == 3 && text == original,
              cluster + " password counts graphemes");
        password = false;
        text = original + "\n0123456789abcdefghijklmnopqrstuvwxyz";
        cursor = cluster_end;
        const auto before = render(input);
        check(input->OnEvent(ftxui::Event::ArrowDown), cluster + " down handled");
        check(cursor == static_cast<int>(original.size()) + 1 + before.x,
              cluster + " down uses library column metric");
        text = original;
        cursor = 0;
        render(input);
        ftxui::Mouse mouse;
        mouse.button = ftxui::Mouse::Left;
        mouse.motion = ftxui::Mouse::Pressed;
        mouse.x = before.x;
        mouse.y = 0;
        check(input->OnEvent(ftxui::Event::Mouse("", mouse)), cluster + " mouse handled");
        check(cursor == cluster_end, cluster + " mouse lands on byte boundary");
        text.clear(); cursor = 0;
        const auto pasted = cluster + "\r\n" + cluster + " tail";
        check(input->OnEvent(ftxui::Event::Character(pasted)), cluster + " multi-byte event handled");
        check(text == pasted && cursor == static_cast<int>(pasted.size()), cluster + " multiline raw bytes retained");
        text = "A " + cluster + " Z";
        const std::vector<int> boundaries{0, 1, 2, 2 + static_cast<int>(cluster.size()),
                                         3 + static_cast<int>(cluster.size()), static_cast<int>(text.size())};
        for (const auto start : boundaries) {
            for (const auto& event : {ftxui::Event::ArrowLeftCtrl, ftxui::Event::ArrowRightCtrl}) {
                cursor = start;
                input->OnEvent(event);
                check(std::find(boundaries.begin(), boundaries.end(), cursor) != boundaries.end(),
                      cluster + " Ctrl-word never splits cluster");
                check(text == "A " + cluster + " Z", cluster + " Ctrl-word preserves bytes");
            }
        }
    }
}

void merge_and_crlf() {
    std::string text;
    int cursor = 0;
    bool insert = true;
    bool password = false;
    ftxui::InputOption option;
    option.cursor_position = &cursor; option.insert = &insert;
    option.password = &password; option.multiline = true;
    auto input = ftxui::Input(&text, option); input->TakeFocus();
    const auto event = [&](ftxui::Event value) { check(input->OnEvent(value), "edge event handled"); };
    struct merge { const char* before; int cursor; const char* addition; const char* after; int final_cursor; };
    const std::array merges{
        merge{"👩💻", 4, "‍", "👩‍💻", 11}, merge{"eX", 1, "́", "éX", 3},
        merge{"A", 0, "؀", "؀A", 3}, merge{"🇨", 0, "🇳", "🇳🇨", 8}};
    for (const auto& value : merges) {
        text = value.before; cursor = value.cursor;
        event(ftxui::Event::Character(value.addition));
        check(text == value.after && cursor == value.final_cursor, "insertion normalizes newly merged cluster");
        event(ftxui::Event::Backspace);
        check(cursor == 0 && text == (std::string_view(value.after) == "éX" ? "X" : ""),
              "merged cluster removed in one backspace");
    }
    text = "👩‍X💻"; cursor = 7; event(ftxui::Event::Delete);
    check(text == "👩‍💻" && cursor == 11, "deletion merges neighboring cluster");
    text = "👩‍X💻"; cursor = 8; event(ftxui::Event::Backspace);
    check(text == "👩‍💻" && cursor == 11, "backspace merges neighboring cluster");
    text = "A\r\nB"; cursor = 1;
    event(ftxui::Event::ArrowRight); check(cursor == 3, "CRLF right");
    event(ftxui::Event::ArrowLeft); check(cursor == 1, "CRLF left");
    event(ftxui::Event::Delete); check(text == "AB" && cursor == 1, "CRLF delete");
    text = "A\r\nB"; cursor = 3; event(ftxui::Event::Backspace);
    check(text == "AB" && cursor == 1, "CRLF backspace");
    text = "A\r\nB"; cursor = 1; insert = false;
    event(ftxui::Event::Character("X")); check(text == "AX\r\nB" && cursor == 2, "overwrite preserves CRLF");
    insert = true; text = "A\rB"; cursor = 2;
    event(ftxui::Event::Character("\n")); check(text == "A\r\nB" && cursor == 3, "CRLF insertion merge");
    text = "ab\r\ncd\r\nef"; cursor = 1;
    for (const int expected : {5, 9}) {
        event(ftxui::Event::ArrowDown); check(cursor == expected, "CRLF down raw offset");
    }
    for (const int expected : {5, 1}) {
        event(ftxui::Event::ArrowUp); check(cursor == expected, "CRLF up raw offset");
    }
    cursor = 0; render(input);
    ftxui::Mouse mouse;
    mouse.button = ftxui::Mouse::Left; mouse.motion = ftxui::Mouse::Pressed; mouse.x = 1; mouse.y = 1;
    event(ftxui::Event::Mouse("", mouse)); check(cursor == 5, "CRLF mouse raw offset");
    password = true;
    const auto masked = render(input);
    check(occurrences(masked.ansi, "•") == 6 && masked.x == 1 && masked.y == 1,
          "CRLF password uses raw offsets and no CR bullet");
    password = false; text = "́̈A"; cursor = 0;
    event(ftxui::Event::ArrowRight); check(cursor == 4, "leading marks form one cluster");
    event(ftxui::Event::Backspace); check(text == "A" && cursor == 0, "leading marks delete together");
    text = "́̈A"; cursor = 0; event(ftxui::Event::Delete);
    check(text == "A" && cursor == 0, "leading marks forward delete together");
}

void reentrant_callbacks() {
    std::string text = "A";
    int cursor = 1;
    int changes = 0;
    ftxui::Component input;
    ftxui::InputOption option;
    option.cursor_position = &cursor;
    option.multiline = true;
    option.on_change = [&] {
        if (++changes != 1) return;
        text = "👩💻"; cursor = 4;
        check(input->OnEvent(ftxui::Event::Character("‍")), "recursive callback edit handled");
    };
    input = ftxui::Input(&text, option); input->TakeFocus();
    check(input->OnEvent(ftxui::Event::Character("X")), "outer callback edit handled");
    check(text == "👩‍💻" && cursor == 11 && changes == 2, "on_change reentry uses fresh boundaries");
    text = "A"; cursor = 1;
    option.on_change = [&] { text = "L👩‍💻R"; cursor = 12; };
    option.on_enter = [&] { check(input->OnEvent(ftxui::Event::ArrowLeft), "on_enter reentry handled"); };
    input = ftxui::Input(&text, option); input->TakeFocus();
    check(input->OnEvent(ftxui::Event::Return), "return callback handled");
    check(text == "L👩‍💻R" && cursor == 1, "on_enter sees on_change replacement, not old boundary table");
}

void integer_edges() {
    const std::string text = "L👩‍💻R";
    for (const std::size_t at : {std::size_t{0}, std::size_t{2}, text.size(), text.size() + 3}) {
        check(ftxui::GlyphIterate(text, INT_MAX, at) == text.size(), "INT_MAX saturates");
        check(ftxui::GlyphIterate(text, INT_MIN, at) == 0, "INT_MIN saturates");
    }
    check(ftxui::GlyphNext(text, text.size() + 3) == text.size(), "next past end clamps");
    check(ftxui::GlyphPrevious(text, text.size() + 3) == text.size() - 1, "previous past end clamps");
    check(ftxui::GlyphIterate("", INT_MIN) == 0 && ftxui::GlyphIterate("", INT_MAX) == 0, "empty extreme offsets");
    std::string editable = text;
    int cursor = INT_MIN;
    ftxui::InputOption option; option.cursor_position = &cursor;
    auto input = ftxui::Input(&editable, option); input->TakeFocus();
    check(input->OnEvent(ftxui::Event::ArrowRight) && cursor == 1, "negative external cursor normalizes");
    cursor = INT_MAX;
    check(input->OnEvent(ftxui::Event::ArrowLeft) && cursor == 12, "past-end external cursor normalizes");
}

void modifier_context_editing() {
    const std::array<std::string_view, 13> clusters{
        " 🏽", "A🏽", ".🏽", "\u00a0🏽", " 🏽\u0301", "👋\ufe0f🏽",
        "👋\u0301🏽", "👋\ufe0e🏽", "👋\ufe0f\ufe0f🏽", "👩🏽‍💻🏽",
        "👩‍💻🏽", "👋🏽🏾", "🏻🏼🏽🏾🏿"};
    for (const auto cluster_view : clusters) {
        const std::string cluster(cluster_view);
        check_boundaries(cluster, {0, cluster.size()}, "modifier-context raw EGC");
        for (int action = 0; action < 4; ++action) {
            std::string text = cluster;
            int cursor = action % 2 == 0 ? 0 : static_cast<int>(cluster.size());
            ftxui::InputOption option; option.cursor_position = &cursor;
            auto input = ftxui::Input(&text, option); input->TakeFocus();
            const std::array events{ftxui::Event::ArrowRight, ftxui::Event::ArrowLeft,
                                    ftxui::Event::Delete, ftxui::Event::Backspace};
            check(input->OnEvent(events[action]), "modifier-context edit handled");
            check(text == (action < 2 ? cluster : ""), "modifier-context raw bytes/edit unchanged");
            check(cursor == (action == 0 ? static_cast<int>(cluster.size()) : 0),
                  "modifier-context whole-EGC byte cursor");
        }
    }
}
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: unicode_test GraphemeBreakTest.txt\n"; return 1; }
    check(std::string_view(utf8proc_unicode_version()) == "18.0.0",
          "modifier data and segmentation both use Unicode18.0.0");
    check(std::string_view(utf8proc_version()) == "2.12.0",
          "pinned utf8proc API version");
    official_boundaries(argv[1]);
    check_boundaries("", {0}, "empty");
    check_boundaries(std::string("A\xff", 2) + "́B", {0, 1, 2, 4, 5}, "invalid byte separates combining");
    check_boundaries(std::string("\xc0\xaf", 2), {0, 1, 2}, "overlong UTF8 raw bytes");
    check_boundaries(std::string("\xf0\x9f", 2), {0, 1, 2}, "truncated UTF8 raw bytes");
    check_boundaries(std::string("\xed\xa0\x80", 3), {0, 1, 2, 3}, "surrogate UTF8 raw bytes");
    if (failures != 0) {
        std::cerr << "Ordinary boundary failures=" << failures << "; skipping extreme offsets and Input phases.\n";
        return 1;
    }
    editing_families();
    merge_and_crlf();
    reentrant_callbacks();
    integer_edges();
    modifier_context_editing();
    std::cout << "UNICODE_TEST_FAILURES=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
