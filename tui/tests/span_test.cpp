#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/selection.hpp>
#include <ftxui/screen/screen.hpp>

namespace {
int failures = 0;
int assertions = 0;

void check(bool condition, const std::string& name) {
    ++assertions;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

void paint(const ftxui::Element& element, ftxui::Screen& screen, ftxui::Box box) {
    element->ComputeRequirement();
    element->SetBox(box);
    element->Render(screen);
}

std::string draw(const ftxui::Element& element, int columns, int rows) {
    ftxui::Screen screen(columns, rows);
    ftxui::Render(screen, element);
    return screen.ToString();
}

std::string draw_canvas(const ftxui::Canvas& canvas, int columns) {
    return draw(ftxui::canvas(canvas), columns, 1);
}

void surface_spans() {
    ftxui::Screen screen(4, 1);
    check(screen.SetGlyph(0, 0, "中́̈", 2) && screen.SetGlyph(2, 0, "R", 1) &&
              screen.SetGlyph(3, 0, "T", 1), "typed surface accepts complete spans");
    check(screen.ToString() == "中́̈RT", "typed surface preserves combining bytes");
    check(screen.CellAt(0, 0).span == 2 && screen.CellAt(1, 0).span == -1 &&
              screen.CellAt(1, 0).character.empty(), "typed surface records head and continuation");
    check(screen.SetGlyph(0, 0, screen.CellAt(0, 0).character, 2) &&
              screen.ToString() == "中́̈RT", "glyph write accepts its own string alias");

    ftxui::Cell styled;
    styled.character = "中";
    styled.bold = true;
    styled.foreground_color = ftxui::Color::Red;
    check(screen.SetCell(0, 0, styled, 2), "typed cell accepts a complete styled span");
    check(screen.CellAt(0, 0).bold && screen.CellAt(1, 0).bold &&
              screen.CellAt(0, 0).foreground_color == ftxui::Color::Red &&
              screen.CellAt(1, 0).foreground_color == ftxui::Color::Red,
          "typed cell applies style to the full span");
    check(screen.SetCell(0, 0, screen.CellAt(0, 0), 2) &&
              screen.CellAt(0, 0).character == "中" && screen.CellAt(1, 0).span == -1 &&
              screen.CellAt(1, 0).bold, "cell write accepts its own alias and style");
    screen.CellAt(0, 0).underlined = true;
    check(screen.CellAt(0, 0).span == 2 && screen.CellAt(1, 0).span == -1,
          "style-only cell access preserves the span");
    check(screen.SetCell(1, 0, ftxui::Cell(), 1), "erase through a typed cell write");
    check(screen.CellAt(0, 0).character.empty() && screen.CellAt(0, 0).span == 0 &&
              screen.CellAt(1, 0).character.empty() && screen.CellAt(1, 0).span == 1 &&
              screen.CellAt(2, 0).character == "R" && screen.CellAt(3, 0).character == "T",
          "erasing a continuation clears the old whole glyph but keeps neighbours");

    ftxui::Screen bounds(3, 1);
    check(bounds.SetGlyph(0, 0, "中", 2) && bounds.SetGlyph(2, 0, "R", 1), "bounds fixture");
    const auto before = bounds.ToString();
    check(!bounds.SetGlyph(2, 0, "中", 2) && !bounds.SetGlyph(-1, 0, "中", 2) &&
              bounds.ToString() == before, "rejected edge writes are atomic");
    bounds.stencil = {1, 1, 0, 0};
    check(!bounds.SetGlyph(0, 0, "中", 2) && bounds.ToString() == before,
          "rejected stencil write leaves the source intact");
    check(bounds.SetGlyph(1, 0, "X", 1) && bounds.ToString() == " XR",
          "continuation overlay clears its head outside the stencil");
    bounds.Clear();
    check(bounds.ToString() == "   ", "surface clear resets all span data");
    // Character/span mutation through a raw Cell reference is not a write API.
}

void text_spans() {
    for (const std::string_view cluster : {"中", "中́", "中́̈"}) {
        ftxui::Screen full(2, 1);
        ftxui::Render(full, ftxui::text(cluster));
        check(full.ToString() == cluster && full.CellAt(0, 0).span == 2 &&
                  full.CellAt(1, 0).span == -1, "text preserves a full CJK cluster");
        check(draw(ftxui::text(cluster), 1, 1) == " ", "text clips a whole CJK cluster");
    }
    ftxui::Screen left(2, 1);
    paint(ftxui::text("中R"), left, {-1, 1, 0, 0});
    check(left.ToString() == " R", "text left clipping keeps the next visible glyph");
    check(draw(ftxui::text("R中"), 2, 1) == "R ", "text right clipping blanks the half glyph");
    check(draw(ftxui::vtext("中R"), 2, 2) == "中\r\nR ", "vertical text uses one whole glyph per row");
    check(draw(ftxui::vtext("中R"), 1, 2) == " \r\nR", "vertical text clips CJK without losing the next row");
    check(draw(ftxui::text("中\r\nR"), 2, 2) == "中\r\nR ", "CRLF is one row boundary");
    for (int selected_cell : {0, 1}) {
        ftxui::Screen screen(2, 1);
        auto element = ftxui::text("中́");
        ftxui::Selection selection(selected_cell, 0, selected_cell, 0);
        ftxui::Render(screen, element.get(), selection);
        check(selection.GetParts() == "中́" && screen.CellAt(0, 0).inverted &&
                  screen.CellAt(1, 0).inverted, "half-cell selection copies and styles the whole visible glyph");
    }
}

void text_source_ownership() {
    using namespace ftxui;
    // The caller's temporary is destroyed before the node is rendered.
    const auto short_text = text(std::string("AR"));
    const auto short_vertical = vtext(std::string("AR"));
    check(draw(short_text, 2, 1) == "AR", "Text owns a destroyed temporary SSO source");
    check(draw(short_vertical, 1, 2) == "A\r\nR", "VText owns a destroyed temporary SSO source");

    // This one-column EGC is longer than an inline string buffer. Expectations
    // are raw known source bytes, not calculated by a display/width helper.
    std::string long_cluster = "e";
    for (int mark = 0; mark < 32; ++mark) { long_cluster += "́"; }
    Element owned_text;
    Element owned_vertical;
    {
        std::string caller = long_cluster + "\r\nR";
        owned_text = text(caller);
        owned_vertical = vtext(caller);
        caller.assign(caller.size(), 'X');
    }
    check(draw(owned_text, 1, 2) == long_cluster + "\r\nR",
          "Text owns long EGC source after caller mutation/destruction and keeps CRLF rows");
    check(draw(owned_vertical, 2, 1) == long_cluster + "R",
          "VText owns long EGC source after caller mutation/destruction and keeps CRLF columns");
    owned_text->ComputeRequirement();
    owned_text->SetBox({0, 0, 0, 1});
    Selection selected(0, 0, 0, 0);
    owned_text->Select(selected);
    check(selected.GetParts() == long_cluster,
          "Text selection copies the original long EGC rather than the caller's mutated bytes");
    check(owned_text->requirement().min_x == 1 && owned_text->requirement().min_y == 2 &&
              owned_vertical->requirement().min_x == 2 && owned_vertical->requirement().min_y == 1,
          "owned-source offsets preserve independent horizontal/vertical line requirements");
}

void frame_selection() {
    using namespace ftxui;
    for (bool reverse_order : {false, true}) {
        auto hidden = reverse_order ? text("中\nA") | yframe | xframe
                                    : text("中\nA") | xframe | yframe;
        Screen screen(1, 1);
        Selection selection(9, 9, -3, -3);
        Render(screen, hidden.get(), selection);
        check(screen.ToString() == " " && selection.GetParts().empty(),
              "nested frames do not copy an invisible half glyph or hidden row");

        auto content = vbox({text("T"), hbox({text("A"), text("中") | focus, text("R")}), text("B")});
        auto visible = reverse_order ? content | yframe | xframe : content | xframe | yframe;
        Screen positive(2, 1);
        Selection whole(9, 9, -3, -3);
        Render(positive, visible.get(), whole);
        check(whole.GetParts() == "中" && positive.CellAt(0, 0).character == "中" &&
                  positive.CellAt(1, 0).span == -1, "nested frames copy only the fully visible focused glyph");
    }
    for (bool reverse : {false, true}) {
        auto element = hbox({text("中") | xframe | size(WIDTH, EQUAL, 1), text("R")});
        Screen screen(2, 1);
        Selection selection(reverse ? 1 : 0, 0, reverse ? 0 : 1, 0);
        Render(screen, element.get(), selection);
        check(selection.GetParts() == "R" && screen.CellAt(0, 0).character == " " &&
                  screen.CellAt(1, 0).character == "R", "frame viewport does not leak into a sibling");
    }
    for (auto container : {hbox({text("中")}), vbox({text("中")}), flexbox({text("中")})}) {
        auto element = container | xframe;
        Screen screen(1, 1);
        Selection selection(0, 0, 0, 0);
        Render(screen, element.get(), selection);
        check(selection.GetParts().empty(), "container saturation retains the frame viewport");
    }
}

void single_cell_decorators() {
    using namespace ftxui;
    Cell wide;
    wide.character = "中";
    wide.span = 2;
    check(draw(separatorCharacter("中"), 3, 1) == "   ", "wide custom separator is a blank single-cell decoration");
    check(draw(separator(wide), 3, 1) == "   ", "wide Cell separator is a blank single-cell decoration");
    check(draw(text("R") | borderWith(wide), 3, 3) == "   \r\n R \r\n   ",
          "wide custom border stays inside its single-cell thickness");
    check(draw(separatorCharacter("AB"), 3, 1) == "   ", "multiple graphemes do not form one decoration cell");
    check(draw(separatorCharacter("X"), 3, 1) == "XXX", "ASCII custom separator is unchanged");
    check(draw(separatorCharacter("é"), 2, 1) == "éé", "one-cell combining separator preserves its bytes");
    Cell ascii;
    ascii.character = "X";
    check(draw(separator(ascii), 3, 1) == "XXX", "ASCII Cell separator is unchanged");
    check(draw(text("R") | borderWith(ascii), 3, 3) == "XXX\r\nXRX\r\nXXX", "ASCII custom border is unchanged");
    wide.foreground_color = Color::Red;
    wide.bold = true;
    Screen styled(1, 1);
    Render(styled, separator(wide));
    check(styled.CellAt(0, 0).character == " " && styled.CellAt(0, 0).bold &&
              styled.CellAt(0, 0).foreground_color == Color::Red, "invalid decoration preserves Cell style");
}

void canvas_spans() {
    using namespace ftxui;
    Canvas text_canvas(6, 4);
    text_canvas.DrawText(0, 0, "中́R");
    check(draw_canvas(text_canvas, 3) == "中́R", "Canvas DrawText preserves the full CJK cluster");
    text_canvas.DrawText(2, 0, "X");
    check(draw_canvas(text_canvas, 3) == " XR", "Canvas DrawText replaces a continuation atomically");
    Canvas clipped(2, 4);
    clipped.DrawText(0, 0, "中");
    check(draw_canvas(clipped, 1) == " ", "Canvas DrawText clips a whole wide glyph");
    Canvas cells(6, 4);
    Cell wide;
    wide.character = "中";
    wide.span = 2;
    cells.DrawCell(0, 0, wide);
    Cell ascii;
    ascii.character = "X";
    cells.DrawCell(2, 0, ascii);
    check(draw_canvas(cells, 3) == " X ", "Canvas DrawCell replaces a continuation atomically");

    Surface source(3, 1);
    check(source.SetGlyph(0, 0, "中", 2) && source.SetGlyph(2, 0, "R", 1), "Canvas source fixture");
    const std::array<Box, 4> stencils{{{0, 2, 0, 0}, {0, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}}};
    const std::array<std::string_view, 4> expected{"中R", " XX", "X X", "中X"};
    for (std::size_t i = 0; i < stencils.size(); ++i) {
        source.stencil = stencils[i];
        Canvas target(6, 4);
        target.DrawText(0, 0, "XXX");
        target.DrawSurface(0, 0, source);
        check(draw_canvas(target, 3) == expected[i], "Canvas imports only complete source-stencil spans " + std::to_string(i));
    }
    source.stencil = {0, 2, 0, 0};
    Canvas left(6, 4);
    left.DrawSurface(-2, 0, source);
    check(draw_canvas(left, 3) == " R ", "Canvas imported span clips atomically at the left target edge");
    Canvas right(4, 4);
    right.DrawSurface(2, 0, source);
    check(draw_canvas(right, 2) == "  ", "Canvas imported span clips atomically at the right target edge");
    Canvas full(6, 4);
    full.DrawText(0, 0, "中R");
    check(draw(canvas(full), 1, 1) == " ", "Canvas DOM viewport does not serialize a half glyph");
}

void modern_emoji_spans() {
    using namespace ftxui;
    // Literal allocation from the measured Wez Unicode9 / tmux VS16-off
    // contract. These are not universal terminal widths or helper oracles.
    for (const std::string_view cluster : {"👩‍💻", "👩🏽"}) {
        Screen full(3, 1);
        auto node = text(std::string(cluster) + "R");
        Render(full, node);
        check(full.ToString() == std::string(cluster) + "R" &&
                  full.CellAt(0, 0).span == 2 && full.CellAt(1, 0).span == -1 &&
                  full.CellAt(2, 0).character == "R", "modern emoji has complete two-cell span and literal R neighbour");
        check(draw(text(cluster), 1, 1) == " ", "modern emoji clips as a whole, never partial raw bytes");
        for (int selected_cell : {0, 1}) {
            Screen selected_screen(2, 1);
            Selection selected(selected_cell, 0, selected_cell, 0);
            auto selected_node = text(cluster);
            Render(selected_screen, selected_node.get(), selected);
            check(selected.GetParts() == cluster && selected_screen.CellAt(0, 0).inverted &&
                      selected_screen.CellAt(1, 0).inverted, "modern emoji partial-cell selection copies raw EGC and styles whole span");
        }
        check(full.SetGlyph(1, 0, "X", 1) && full.ToString() == " XR",
              "modern emoji continuation overwrite clears whole old glyph without eating R");
        full.Clear();
        check(full.ToString() == "   ", "modern emoji clear leaves no stale raw glyph or span");
    }

    const std::array<std::string_view, 2> bodies{"👩‍💻", "👩‍💻�"};
    const std::array<int, 2> columns{2, 3};
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const std::string body(bodies[i]);
        const int width = columns[i];
        Cell raw;
        raw.character = body;  // Legacy mutable-cell compatibility: span0.
        Canvas cells(2 * (width + 1), 4);
        cells.DrawCell(0, 0, raw);
        cells.DrawText(2 * width, 0, "R");
        check(draw_canvas(cells, width + 1) == body + "R",
              "Canvas DrawCell span0 keeps literal full display and adjacent R");
        // Synthetic legacy mutable-cell fixture: no typed span metadata,
        // including no stale negative continuations. Uses the public API,
        // not a production test hook.
        for (int x = 0; x < width; ++x)
            cells.Style(2 * x, 0, [](Cell& cell) { cell.span = 0; });
        check(draw_canvas(cells, width + 1) == body + "R",
              "Canvas node span0 fallback uses the same literal display allocation");
        check(draw(canvas(cells), width - 1, 1) == std::string(width - 1, ' '),
              "Canvas node clips the complete fallback span at its viewport");
        Canvas typed(2 * (width + 1), 4);
        typed.DrawCell(0, 0, raw);
        typed.DrawText(2 * width, 0, "R");
        typed.DrawText(0, 0, "X");
        check(draw_canvas(typed, width + 1) == "X" + std::string(width - 1, ' ') + "R",
              "Canvas wide-to-short redraw clears old tails and preserves R");
        for (int x = 1; x < width; ++x)
            check(typed.GetCell(x, 0).span >= 0,
                  "Canvas wide-to-short clears negative continuation metadata");
        typed.DrawText(2, 0, "Y");
        check(draw_canvas(typed, width + 1) == "XY" + std::string(width - 2, ' ') + "R",
              "Canvas subsequent tail write never clears the new short head");

        Canvas edge(2 * (width - 1), 4);
        edge.DrawCell(0, 0, raw);
        check(draw_canvas(edge, width - 1) == std::string(width - 1, ' '),
              "Canvas DrawCell rejects a partial span0 glyph at the last column");

        Surface source(width + 1, 1);
        source.CellAt(0, 0).character = body;
        for (int x = 1; x < width; ++x) source.CellAt(x, 0).character.clear();
        source.CellAt(width, 0).character = "R";
        Canvas imported(2 * (width + 1), 4);
        imported.DrawSurface(0, 0, source);
        check(draw_canvas(imported, width + 1) == body + "R",
              "Canvas DrawSurface span0 preserves complete display and literal R position");
        source.stencil = {0, width - 2, 0, 0};
        Canvas stencil(2 * (width + 1), 4);
        stencil.DrawText(0, 0, std::string(width + 1, 'X'));
        stencil.DrawSurface(0, 0, source);
        check(draw_canvas(stencil, width + 1) == std::string(width - 1, ' ') + "XX",
              "Canvas DrawSurface clips a whole span0 glyph to its source stencil");
    }
}

void sibling_writers() {
    using namespace ftxui;
    for (int at : {0, 1}) {
        Screen separator_screen(3, 1);
        check(separator_screen.SetGlyph(0, 0, "中", 2) && separator_screen.SetGlyph(2, 0, "R", 1), "separator overlay fixture");
        paint(separatorCharacter("X"), separator_screen, {at, at, 0, 0});
        check(separator_screen.ToString() == (at == 0 ? "X R" : " XR"), "separator replaces the whole old span");
    }
    Screen gauge_screen(3, 1);
    check(gauge_screen.SetGlyph(0, 0, "中", 2) && gauge_screen.SetGlyph(2, 0, "R", 1), "gauge overlay fixture");
    paint(gauge(0.F), gauge_screen, {1, 1, 0, 0});
    check(gauge_screen.ToString() == "  R" && gauge_screen.CellAt(1, 0).span == 1, "gauge blank replaces a continuation");

    Screen graph_screen(3, 1);
    check(graph_screen.SetGlyph(0, 0, "中", 2) && graph_screen.SetGlyph(2, 0, "R", 1), "graph overlay fixture");
    paint(graph([](int columns, int height) { return std::vector<int>(static_cast<std::size_t>(columns), height + 1); }),
          graph_screen, {1, 1, 0, 0});
    check(graph_screen.ToString() == " █R" && graph_screen.CellAt(1, 0).span == 1, "graph replaces a continuation with a block");

    Screen border_screen(5, 3);
    for (int row = 0; row < 3; ++row) {
        check(border_screen.SetGlyph(0, row, "中", 2) && border_screen.SetGlyph(4, row, "R", 1), "border overlay fixture");
    }
    paint(text("") | border, border_screen, {1, 3, 0, 2});
    check(border_screen.ToString() == " ╭─╮R\r\n │ │R\r\n ╰─╯R", "border replaces old continuations without eating its neighbours");

    Screen vertical(2, 1);
    check(vertical.SetGlyph(0, 0, "中", 2), "vertical scrollbar overlay fixture");
    paint(emptyElement() | vscroll_indicator, vertical, {0, 1, 0, 4});
    check(vertical.ToString() == " ┃" && vertical.CellAt(1, 0).span == 1, "vertical scrollbar replaces a continuation");

    Screen horizontal(2, 1);
    check(horizontal.SetGlyph(0, 0, "中", 2), "horizontal scrollbar overlay fixture");
    paint(emptyElement() | hscroll_indicator, horizontal, {0, 4, 0, 0});
    check(horizontal.ToString() == "─╴" && horizontal.CellAt(0, 0).span == 1 &&
              horizontal.CellAt(1, 0).span == 1, "horizontal scrollbar writes independent typed cells");
}
}

int main() {
    surface_spans();
    text_spans();
    text_source_ownership();
    frame_selection();
    single_cell_decorators();
    canvas_spans();
    modern_emoji_spans();
    sibling_writers();
    std::cout << "SPAN_ASSERTIONS=" << assertions << " SPAN_FAILURES=" << failures << '\n';
    return failures ? 1 : 0;
}
