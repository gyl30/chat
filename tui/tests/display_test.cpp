#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/dom/selection.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/screen/string.hpp"
#include <iostream>
#include <string>
#include <utility>
#include <vector>
namespace {
using namespace ftxui;
int checks = 0, failed = 0;
void check(bool ok, const std::string &label) {
    ++checks;
    if (!ok) {
        ++failed;
        std::cout << "FAIL " << label << "\n";
    }
}
std::string hex(const std::string &s) {
    const char *d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}
struct glyph {
    std::string raw, display;
    int columns = 1;
};
struct fixture {
    std::string name;
    std::vector<glyph> glyphs;
};
std::string source_text(const fixture &f) {
    std::string s;
    for (const auto &g : f.glyphs)
        s += g.raw;
    return s;
}
void expect_row(const Screen &s, const fixture &f, const std::string &label,
                int row = 0) {
    int x = 0;
    for (const auto &g : f.glyphs) {
        check(s.CellAt(x, row).character == g.display,
              label + " cell@" + std::to_string(x) +
                  " expected=" + hex(g.display) +
                  " actual=" + hex(s.CellAt(x, row).character));
        check(s.CellAt(x, row).span == g.columns,
              label + " head span@" + std::to_string(x));
        for (int offset = 1; offset < g.columns; ++offset)
            check(s.CellAt(x + offset, row).character.empty() &&
                      s.CellAt(x + offset, row).span == -offset,
                  label + " continuation@" + std::to_string(x + offset));
        x += g.columns;
    }
}
struct input_fixture {
    std::string content;
    int cursor = 0;
    bool password = false;
    Component input;
    input_fixture(std::string s, bool secret = false)
        : content(std::move(s)), password(secret) {
        InputOption option;
        option.content = &content;
        option.cursor_position = &cursor;
        option.password = &password;
        option.transform = [](InputState state) { return state.element; };
        input = Input(option);
        input->TakeFocus();
    }
    Screen paint() {
        Screen s(20, 4);
        Render(s, input->Render());
        return s;
    }
};
} // namespace

int main() {
    const std::string circle = "◌", replacement = "�";
    // Expectations are explicit public display policy, not candidate
    // implementation.
    const std::vector<fixture> fixtures = {
        {"lone-mark", {{"\u0301", circle + "\u0301"}}},
        {"two-marks-one-EGC", {{"\u0301\u0308", circle + "\u0301\u0308"}}},
        {"leading-mark", {{"\u0301", circle + "\u0301"}, {"A", "A"}}},
        {"leading-mark-normal-CJK",
         {{"\u0301\u0308", circle + "\u0301\u0308"},
          {"e\u0301", "e\u0301"},
          {"界", "界", 2},
          {"Z", "Z"}}},
        {"normal-combining", {{"e\u0301", "e\u0301"}, {"A", "A"}}},
        {"orphan-skin-light", {{"\U0001f3fb", replacement}, {"A", "A"}}},
        {"orphan-skin-medium-light", {{"\U0001f3fc", replacement}, {"A", "A"}}},
        {"orphan-skin-medium", {{"\U0001f3fd", replacement}, {"A", "A"}}},
        {"orphan-skin-medium-dark", {{"\U0001f3fe", replacement}, {"A", "A"}}},
        {"orphan-skin-dark", {{"\U0001f3ff", replacement}, {"A", "A"}}},
        {"normal-skin-light", {{"👋\U0001f3fb", "👋\U0001f3fb", 2}, {"A", "A"}}},
        {"normal-skin-medium-light", {{"👋\U0001f3fc", "👋\U0001f3fc", 2}, {"A", "A"}}},
        {"normal-skin-medium", {{"👋\U0001f3fd", "👋\U0001f3fd", 2}, {"A", "A"}}},
        {"normal-skin-medium-dark", {{"👋\U0001f3fe", "👋\U0001f3fe", 2}, {"A", "A"}}},
        {"normal-skin-dark", {{"👋\U0001f3ff", "👋\U0001f3ff", 2}, {"A", "A"}}},
        {"orphan-skin-cluster", {{"\U0001f3fb\U0001f3fc\U0001f3fd\U0001f3fe\U0001f3ff", replacement}, {"A", "A"}}},
        // UTS#51 local modifier relation, not "any base in the EGC".
        // Literal expected display/columns are independent of generated data.
        {"space-skin-light", {{" \U0001f3fb", " " + replacement, 2}, {"A", "A"}}},
        {"space-skin-medium-light", {{" \U0001f3fc", " " + replacement, 2}, {"A", "A"}}},
        {"space-skin-medium", {{" \U0001f3fd", " " + replacement, 2}, {"A", "A"}}},
        {"space-skin-medium-dark", {{" \U0001f3fe", " " + replacement, 2}, {"A", "A"}}},
        {"space-skin-dark", {{" \U0001f3ff", " " + replacement, 2}, {"A", "A"}}},
        {"letter-skin", {{"A🏽", "A" + replacement, 2}, {"Z", "Z"}}},
        {"punctuation-skin", {{".🏽", "." + replacement, 2}, {"Z", "Z"}}},
        {"NBSP-skin", {{"\u00a0🏽", "\u00a0" + replacement, 2}, {"Z", "Z"}}},
        {"space-skin-mark", {{" 🏽\u0301", " " + replacement + "\u0301", 2}, {"Z", "Z"}}},
        {"space-mark-unmodified", {{" \u0301", " \u0301"}, {"Z", "Z"}}},
        {"normal-woman-skin", {{"👩🏽", "👩🏽", 2}, {"Z", "Z"}}},
        {"normal-hand-VS16-skin", {{"👋\ufe0f🏽", "👋\ufe0f🏽", 2}, {"Z", "Z"}}},
        {"interrupted-hand-mark", {{"👋\u0301🏽", "👋\u0301" + replacement, 3}, {"Z", "Z"}}},
        {"interrupted-hand-VS15", {{"👋\ufe0e🏽", "👋\ufe0e" + replacement, 3}, {"Z", "Z"}}},
        {"interrupted-hand-two-VS16", {{"👋\ufe0f\ufe0f🏽", "👋\ufe0f\ufe0f" + replacement, 3}, {"Z", "Z"}}},
        // Width2 is the literal original-X11 R-distance expectation for the
        // measured modern host, not a DisplayWidth-derived expectation.
        {"normal-woman-ZWJ", {{"👩‍💻", "👩‍💻", 2}, {"Z", "Z"}}},
        // Replacements break the display into emoji EGC2 + carrier EGC1.
        {"local-ZWJ-first-valid-last-malformed", {{"👩🏽‍💻🏽", "👩🏽‍💻" + replacement, 3}, {"Z", "Z"}}},
        {"local-ZWJ-earlier-base-not-enough", {{"👩‍💻🏽", "👩‍💻" + replacement, 3}, {"Z", "Z"}}},
        {"local-ZWJ-last-valid", {{"👩‍👩🏽", "👩‍👩🏽", 2}, {"Z", "Z"}}},
        {"local-ZWJ-both-valid", {{"👩🏽‍👩🏽", "👩🏽‍👩🏽", 2}, {"Z", "Z"}}},
        {"second-modifier-unattached", {{"👋🏽🏾", "👋🏽" + replacement, 3}, {"Z", "Z"}}},
        // Structural ED13 membership is not the narrower RGI sequence set.
        {"modifier-base-family-non-RGI", {{"👪🏽", "👪🏽", 2}, {"Z", "Z"}}},
        // Pinned Unicode18 property allocation, not a measured old-host claim.
        // Old terminal Unicode-version compatibility still needs native proof.
        {"Unicode18-modifier-base", {{"\U0001faf9🏽", "\U0001faf9🏽", 2}, {"Z", "Z"}}},
        // Policy-only extended cases: no new native pass is claimed for these.
        {"regional-indicator-pair", {{"🇨🇳", "🇨🇳", 2}, {"R", "R"}}},
        {"heart-VS15", {{"❤\ufe0e", "❤\ufe0e", 1}, {"R", "R"}}},
        {"heart-VS16", {{"❤️", "❤️", 1}, {"R", "R"}}},
        {"keycap-VS16", {{"1️⃣", "1️⃣", 1}, {"R", "R"}}},
        // Native DSR measured dotted circle + orphan Mc U0903 as two columns,
        // so the one-cell orphan policy renders replacement, not that carrier.
        // Raw U0903 stays exact in copy/edit checks; base+Mc shaping is not certified.
        {"spacing-mark-CCC0", {{"\u0903", replacement}, {"A", "A"}}},
        {"ZWJ-only", {{"\u200d", replacement}, {"A", "A"}}},
        {"ZWNJ-only", {{"\u200c", replacement}, {"A", "A"}}},
        {"word-joiner", {{"\u2060", replacement}, {"A", "A"}}},
        {"zero-width-space", {{"A", "A"}, {"\u200b", replacement}, {"B", "B"}}},
        {"ESC-control", {{std::string(1, '\x1b'), replacement}, {"A", "A"}}},
        {"unit-separator-control",
         {{std::string(1, '\x1f'), replacement}, {"A", "A"}}},
        {"bidi-override-control", {{"\u202e", replacement}, {"A", "A"}}}};
    for (const auto &f : fixtures) {
        const std::string source = source_text(f);
        int columns = 0;
        for (const auto &g : f.glyphs)
            columns += g.columns;
        check(DisplayWidth(source) == columns, f.name + " public DisplayWidth");
        std::vector<int> expected_map;
        std::vector<std::string> expected_glyphs;
        for (size_t i = 0; i < f.glyphs.size(); ++i) {
            for (int offset = 0; offset < f.glyphs[i].columns; ++offset) {
                expected_map.push_back(i);
                expected_glyphs.push_back(offset == 0 ? f.glyphs[i].raw : "");
            }
        }
        check(CellToGlyphIndex(source) == expected_map,
              f.name + " public column-to-EGC map");
        check(Utf8ToGlyphs(source) == expected_glyphs,
              f.name + " public raw glyph slots");
        auto node = text(source);
        node->ComputeRequirement();
        node->SetBox({0, 19, 0, 3});
        Screen s(20, 4);
        node->Render(s);
        check(node->requirement().min_x == columns,
              f.name + " Text display columns");
        expect_row(s, f, f.name + " Text");
        const std::string terminal_bytes = s.ToString();
        std::string literal_display;
        for (const auto &g : f.glyphs)
            literal_display += g.display;
        check(terminal_bytes.find(literal_display) != std::string::npos,
              f.name + " serializer keeps literal complete display adjacency");
        for (const auto &g : f.glyphs) {
            if (g.display == g.raw)
                check(terminal_bytes.find(g.raw) != std::string::npos,
                      f.name + " serializer preserves complete legal bytes");
            if (g.display == replacement) {
                // Serializer-generated ANSI is legitimate; reject the user
                // ESC+A payload rather than rejecting every serializer ESC
                // byte.
                const std::string dangerous =
                    g.raw == std::string(1, '\x1b') ? g.raw + "A" : g.raw;
                check(terminal_bytes.find(dangerous) == std::string::npos,
                      f.name +
                          " terminal serialization omits raw control/format");
            }
        }
        Selection all(0, 0, columns - 1, 0);
        node->Select(all);
        check(all.GetParts() == source,
              f.name + " Text copy raw exact expected=" + hex(source) +
                  " actual=" + hex(all.GetParts()));
        int selected_x = 0;
        for (const auto &g : f.glyphs) {
            Selection one(selected_x, 0, selected_x + g.columns - 1, 0);
            node->Select(one);
            check(one.GetParts() == g.raw, f.name + " per-EGC raw copy");
            for (int offset = 0; offset < g.columns; ++offset) {
                Selection cell(selected_x + offset, 0, selected_x + offset, 0);
                node->Select(cell);
                check(cell.GetParts() == g.raw,
                      f.name + " partial-cell raw EGC copy");
            }
            selected_x += g.columns;
        }
        input_fixture input(source);
        int byte = 0, column = 0;
        for (size_t boundary = 0; boundary <= f.glyphs.size(); ++boundary) {
            input.cursor = byte;
            const Screen painted = input.paint();
            check(painted.cursor().x == column && painted.cursor().y == 0,
                  f.name + " Input boundary=" + std::to_string(boundary) +
                      " actualCursor=" + std::to_string(painted.cursor().x) +
                      "," + std::to_string(painted.cursor().y) +
                      " expected=" + std::to_string(column) + ",0");
            expect_row(painted, f,
                       f.name + " Input three fragments boundary=" +
                           std::to_string(boundary));
            check(input.cursor == byte && input.content == source,
                  f.name + " render source/cursor unchanged");
            if (boundary < f.glyphs.size()) {
                byte += f.glyphs[boundary].raw.size();
                column += f.glyphs[boundary].columns;
            }
        }
        // Mouse starts from a rendered state; clicks are at known EGC
        // starts/end.
        byte = 0;
        column = 0;
        for (size_t boundary = 0; boundary <= f.glyphs.size(); ++boundary) {
            input.cursor = 0;
            input.paint();
            Mouse mouse;
            mouse.button = Mouse::Left;
            mouse.motion = Mouse::Pressed;
            mouse.x = column;
            mouse.y = 0;
            input.input->OnEvent(Event::Mouse("", mouse));
            const Screen painted = input.paint();
            check(input.cursor == byte,
                  f.name + " mouse byte boundary=" + std::to_string(boundary) +
                      " actual=" + std::to_string(input.cursor) +
                      " expected=" + std::to_string(byte));
            check(painted.cursor().x == column,
                  f.name + " mouse rendered column");
            check(input.content == source, f.name + " mouse source unchanged");
            if (boundary < f.glyphs.size()) {
                byte += f.glyphs[boundary].raw.size();
                column += f.glyphs[boundary].columns;
            }
        }
        input_fixture password(source, true);
        byte = 0;
        for (size_t boundary = 0; boundary <= f.glyphs.size(); ++boundary) {
            password.cursor = byte;
            const Screen painted = password.paint();
            check(painted.cursor().x == static_cast<int>(boundary),
                  f.name + " password boundary column");
            for (size_t i = 0; i < f.glyphs.size(); ++i)
                check(painted.CellAt(i, 0).character == "•",
                      f.name + " password bullet count");
            check(password.content == source,
                  f.name + " password source unchanged");
            if (boundary < f.glyphs.size())
                byte += f.glyphs[boundary].raw.size();
        }
    }
    check(string_width("\u0301") == 0 && string_width("\u0301\u0308") == 0,
          "intrinsic zero-only marks remain zero");
    check(string_width("e\u0301") == 1 && string_width("界") == 2,
          "intrinsic existing normal/CJK width policy unchanged");
    check(DisplayWidth("") == 0 && DisplayWidth("0123456789") == 10 &&
              DisplayWidth(std::string(80, 'A')) == 80,
          "ASCII all_of fast path is string length, never capped at2");
    // Literal widths from the independent original ASCII-R physical evidence,
    // to be archived as docs/images/experience/tui-modern-width-proof.json.
    // This implementation is not the oracle; this is not a full host matrix.
    const std::vector<std::pair<std::string, int>> native_cases = {
        {"AB", 2}, {"👩‍💻", 2}, {"1️⃣", 1}, {"❤️", 1},
        {"👩🏽", 2}, {"👩‍💻�", 3}, {"中\u0301", 2}};
    for (const auto &[body, columns] : native_cases) {
        const std::string source = "L" + body + "R END";
        auto node = text(source);
        Screen s(14, 1);
        Render(s, node);
        check(s.CellAt(1 + columns, 0).character == "R",
              "native literal neighboring R column raw=" + hex(body));
        check(s.ToString().find(source) != std::string::npos,
              "native row serializes exact raw EGC and adjacent ASCII");
        Selection selected(0, 0, columns + 5, 0);
        node->Select(selected);
        check(selected.GetParts() == source, "native row copy exact raw bytes");
    }
    // The old mutable-cell compatibility fallback must not resurrect width5.
    Screen fallback(5, 1);
    fallback.CellAt(0, 0).character = "👩‍💻";
    fallback.CellAt(1, 0).character.clear();
    fallback.CellAt(2, 0).character = "R";
    check(fallback.ToString().find("👩‍💻R") != std::string::npos,
          "span0 serializer fallback uses display width2, raw bytes unchanged");
    // Clipped wide groups must not be partially displayed or copied; unrelated
    // zero-carrier source must stay exactly selectable at nonzero origin.
    for (bool clip_left : {false, true}) {
        auto node = text("\u0301界Z");
        node->ComputeRequirement();
        node->SetBox({3, 6, 1, 1});
        Screen s(12, 3);
        s.stencil = clip_left ? Box{5, 6, 1, 1} : Box{3, 4, 1, 1};
        node->Render(s);
        Selection selected(3, 1, 6, 1);
        auto clipped = selected.Clip(s.stencil);
        node->Select(clipped);
        check(selected.GetParts() == (clip_left ? "Z" : "\u0301"),
              "nonzero-origin partial-wide selected copy");
        const int fragment = clip_left ? 5 : 4;
        check(s.CellAt(fragment, 1).character == " " &&
                  s.CellAt(fragment, 1).span == 1,
              "nonzero-origin partial-wide blank");
        if (!clip_left)
            check(s.CellAt(3, 1).character == circle + "\u0301",
                  "nonzero-origin carrier retained");
    }
    for (const std::string &newline :
         {std::string("\n"), std::string("\r\n")}) {
        const std::string source = "\u0301A" + newline + "BC";
        const int first_end = 3, second_start = first_end + newline.size();
        input_fixture input(source);
        for (int position :
             {0, 2, 3, second_start, second_start + 1, second_start + 2}) {
            input.cursor = position;
            const Screen s = input.paint();
            const bool second = position >= second_start;
            const int x = second          ? position - second_start
                          : position == 0 ? 0
                          : position == 2 ? 1
                                          : 2;
            check(s.cursor().x == x && s.cursor().y == (second ? 1 : 0),
                  "newline=" + hex(newline) +
                      " cursor position=" + std::to_string(position));
            check(input.content == source, "newline render bytes unchanged");
        }
        input.cursor = 2;
        input.paint();
        input.input->OnEvent(Event::ArrowDown);
        check(input.cursor == second_start + 1,
              "UpDown preserves carrier column=1 down newline=" + hex(newline));
        input.paint();
        input.input->OnEvent(Event::ArrowUp);
        check(input.cursor == 2,
              "UpDown preserves carrier column=1 up newline=" + hex(newline));
        check(input.content == source, "UpDown source unchanged");
        auto node = text(source);
        node->ComputeRequirement();
        check(node->requirement().min_x == 2 && node->requirement().min_y == 2,
              "newline Text geometry");
        node->SetBox({0, 19, 0, 3});
        Screen s(20, 4);
        node->Render(s);
        check(s.CellAt(0, 0).character == circle + "\u0301" &&
                  s.CellAt(1, 0).character == "A" &&
                  s.CellAt(0, 1).character == "B" &&
                  s.CellAt(1, 1).character == "C",
              "newline paints second row without CR replacement");
    }
    std::cout << "SUMMARY assertions=" << checks << " failures=" << failed
              << "\n";
    return failed ? 1 : 0;
}
