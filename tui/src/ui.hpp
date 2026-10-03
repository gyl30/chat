#pragma once

#include "state.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>

namespace chat::tui
{
class app;
// Pure rendering also supports deterministic tests without a terminal.
ftxui::Element render(state const& data, int width, int height, int message_scroll = -1);
ftxui::Component make_ui(app& application, std::function<void()> quit);
}
