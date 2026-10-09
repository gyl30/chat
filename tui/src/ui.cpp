#include "ui.hpp"
#include "server_address.hpp"
#include "app.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <ctime>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <utility>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

namespace chat::tui
{
namespace
{
using namespace ftxui;
struct shortcut { std::string_view key, description; };
constexpr std::array shortcuts{
    shortcut{"直接输入", "打开会话后直接输入；输入框中所有字符都是正文"},
    shortcut{"Enter", "发送消息 / 打开所选 / 确认"},
    shortcut{"\\ 后 Enter", "换行：只对刚键入的 \\ 生效；先按 → 等任意键再 Enter 可发送结尾的 \\"},
    shortcut{"Alt+Enter", "换行（已在 tmux 中验证；WezTerm 默认用它切换全屏）"},
    shortcut{"Esc", "依次：清除错误 / 取消回复或编辑 / 回到输入框 / 返回列表"},
    shortcut{"Tab / Shift+Tab", "宽屏在列表、消息、输入框之间切换；窄屏在消息与输入框之间切换"},
    shortcut{"↑（输入框为空）", "选择消息"},
    shortcut{"PgUp（输入框中）", "转到消息并加载更早的消息，草稿保留"},
    shortcut{"Ctrl+K", "命令面板：搜索并执行命令，草稿保留；:命令 照常可用"},
    shortcut{"F1；不在输入框时也可按 ?", "帮助"},
    shortcut{":history", "聊天记录：全部、图片、文件、链接；也可在命令面板选择"},
    shortcut{"Ctrl+C / :quit", "安全退出"},
    shortcut{"以下按键在消息或列表中使用", ""},
    shortcut{"j / ↓，k / ↑", "移动选择"},
    shortcut{"Enter（消息、成员）", "打开操作菜单：只列出当前可用的操作，菜单中字母直接执行"},
    shortcut{"i", "回到输入框"},
    shortcut{"r / e / d", "回复 / 编辑 / 删除（需确认）"},
    shortcut{"a", "表情回应（0 取消）"},
    shortcut{"y / s", "显示可复制文本 / 保存附件"},
    shortcut{"/", "搜索当前会话"},
    shortcut{"PgUp / PgDn", "加载更早的消息 / 加载更多搜索结果或申请"},
    shortcut{"G", "回到最新消息"},
    shortcut{"[ / ]", "在长消息内逐行滚动"},
    shortcut{"m / p", "免打扰 / 置顶会话"},
    shortcut{"h / c / u", "聊天 / 联系人 / 账号"},
    shortcut{"N（Shift+n）", "新建：添加好友 / 创建群聊 / 加入群聊"},
    shortcut{"g", "当前群聊的操作"},
    shortcut{"A / O / D（成员页）", "设置或取消管理员 / 转让群主 / 移除成员"},
    shortcut{"y / n（申请页）", "接受 / 拒绝所选申请"},
    shortcut{"y / n / x（新的朋友）", "接受 / 拒绝收到的申请，撤回发出的申请"},
    shortcut{"/（联系人、选择好友）", "按名称筛选好友"},
    shortcut{"Space / Enter（选择好友）", "勾选成员 / 完成选择"},
    shortcut{":", "输入命令（:help 查看命令列表）"},
};
struct menu_action { std::string label, command; };
std::vector<menu_action> actions(state const& s)
{
    if (s.view == page::new_action)
    { return {{"添加好友", "add-contact"}, {"创建群聊", "create-group"}, {"加入群聊", "join"}}; }
    if (s.view == page::profile)
    {
        std::vector<menu_action> items{{"显示可复制的用户名", "copy-user"}};
        if (s.profile.id == s.self.id)
        {
            items.push_back({"设置头像（PNG/JPEG 文件路径）", "avatar"});
            items.push_back({"清除头像", "avatar-clear"});
            items.push_back({"退出登录", "logout"});
        }
        else if (s.is_contact(s.profile.id))
        {
            items.push_back({"发消息", "message"});
            items.push_back({"删除好友", "remove-contact"});
        }
        else if (s.friendship(s.profile.id) == friendship_state::outgoing_pending)
        { items.push_back({"撤回好友申请", "cancel-friend"}); }
        else if (s.friendship(s.profile.id) == friendship_state::incoming_pending)
        {
            items.push_back({"接受好友申请", "accept-friend"});
            items.push_back({"拒绝好友申请", "reject-friend"});
        }
        else { items.push_back({"添加好友", "add"}); }
        return items;
    }
    if (s.view != page::group) { return {}; }
    auto c = s.active_conversation();
    if (!c || c->kind != conversation_kind::group) { return {}; }
    std::vector<menu_action> items{{"全部成员", "members"}};
    if (!c->announcement.empty()) { items.push_back({"查看完整公告", "show-announcement"}); }
    if (c->pinned_message) { items.push_back({"查看置顶消息", "pinned"}); }
    if (s.self_role() != member_role::member)
    {
        items.push_back({"邀请好友入群", "invite"});
        items.push_back({"修改群名称", "rename"});
        items.push_back({"编辑或清除公告", "announcement"});
        items.push_back({"置顶所选消息", "pin-message"});
        if (c->pinned_message) { items.push_back({"取消置顶消息", "unpin-message"}); }
        items.push_back({"查看邀请码", "link"});
        items.push_back({"生成新邀请码", "link-create"});
        items.push_back({"撤销邀请码", "link-revoke"});
        items.push_back({c->join_approval ? "关闭入群审批" : "开启入群审批", "approval"});
        items.push_back({"入群申请", "requests"});
    }
    if (s.self_role() != member_role::owner) { items.push_back({"退出群聊", "leave"}); }
    return items;
}
std::string first_glyph(std::string const& name)
{
    auto glyphs = Utf8ToGlyphs(name);
    return glyphs.empty() ? "?" : glyphs.front();
}
Element wrapped_text(std::string const& value, int width)
{
    width = std::max(1, width);
    std::string output;
    int column = 0;
    // Use FTXUI's glyphs and terminal cell widths for wrapping.
    for (auto const& glyph : Utf8ToGlyphs(value))
    {
        if (glyph.empty()) { continue; } // reserved second cell of a wide glyph
        if (glyph == "\n" || glyph == "\r\n") { output += glyph; column = 0; continue; }
        auto const cells = DisplayWidth(glyph);
        if (column && column + cells > width) { output += '\n'; column = 0; }
        output += glyph;
        column += cells;
    }
    return text(output);
}
Element help_content(int width, int* line_count = nullptr)
{
    Elements rows;
    if (line_count) { *line_count = 1; } // The separator contributes one row.
    auto wrap = [width, line_count, &rows](std::string_view value) {
        std::string output;
        int column = 0;
        int lines = 1;
        std::size_t start = 0;
        while (start < value.size())
        {
            auto const end = value.find(' ', start);
            auto const word = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
            if (!word.empty())
            {
                auto const cells = DisplayWidth(word);
                if (column && column + 1 + cells > width)
                { if (!line_count) { output += '\n'; } column = 0; ++lines; }
                else if (column) { if (!line_count) { output += ' '; } ++column; }
                if (cells <= width - column) { if (!line_count) { output += word; } column += cells; }
                else
                {
                    // Chinese text has no spaces to break at.
                    for (auto const& glyph : Utf8ToGlyphs(std::string(word)))
                    {
                        if (glyph.empty()) { continue; }
                        auto const glyph_cells = DisplayWidth(glyph);
                        if (column && column + glyph_cells > width)
                        { if (!line_count) { output += '\n'; } column = 0; ++lines; }
                        if (!line_count) { output += glyph; }
                        column += glyph_cells;
                    }
                }
            }
            if (end == std::string_view::npos) { break; }
            start = end + 1;
        }
        if (line_count) { *line_count += lines; return; }
        rows.push_back(text(output));
    };
    for (auto const& shortcut : shortcuts)
    { wrap(std::string(shortcut.key) + "  " + std::string(shortcut.description)); }
    if (!line_count) { rows.push_back(separator()); }
    wrap("命令：new, chats, contacts, friend-requests, friend-sent, accept-friend, reject-friend, cancel-friend, filter, add-contact, profile, account, create-group, join, file, save, members, invite, rename, announcement, show-announcement, pinned, pin-message, unpin-message, link, link-create, link-revoke, approval, requests, avatar, avatar-clear, logout, quit");
    wrap("复制：在可复制文本页用终端自带的选择功能复制。");
    return line_count ? Element{} : vbox(std::move(rows));
}
Element preview_text(std::string value, int width)
{
    // Headers and quotes are previews: embedded line breaks must not consume
    // the history or composer. Full text remains available on the copy page.
    for (char& c : value) { if (c == '\n' || c == '\r' || c == '\t') { c = ' '; } }
    width = std::max(1, width);
    if (DisplayWidth(value) > width)
    {
        std::string clipped;
        int column = 0;
        for (auto const& glyph : Utf8ToGlyphs(value))
        {
            if (glyph.empty()) { continue; }
            auto cells = DisplayWidth(glyph);
            if (column + cells >= width) { break; }
            clipped += glyph;
            column += cells;
        }
        value = std::move(clipped) + "…";
    }
    return text(value) | size(HEIGHT, EQUAL, 1);
}
std::string user_label(std::string const& name) { return "[" + first_glyph(name) + "] " + name; }
std::tm local_time(std::int64_t value)
{
    // Protocol timestamps are Unix milliseconds.
    auto const seconds = static_cast<std::time_t>(value / 1000);
    std::tm time{};
    localtime_r(&seconds, &time);
    return time;
}
std::string format_time(std::tm const& time, char const* format)
{
    std::array<char, 32> output{};
    std::strftime(output.data(), output.size(), format, &time);
    return output.data();
}
std::string timestamp(std::int64_t value)
{
    if (value <= 0) { return {}; }
    return format_time(local_time(value), "%m-%d %H:%M");
}
std::string clock_time(std::int64_t value) { return value > 0 ? format_time(local_time(value), "%H:%M") : std::string{}; }
// Calendar days since an arbitrary epoch, for comparing local dates.
// Days since 1970-01-01 of the local calendar date. Counted from the date itself, not from
// seconds: around a daylight saving change a local day is 23 or 25 hours long.
int day_number(std::tm const& time)
{
    auto const date = std::chrono::year{time.tm_year + 1900} / (time.tm_mon + 1) / time.tm_mday;
    return static_cast<int>(std::chrono::sys_days{date}.time_since_epoch().count());
}
int today() { auto now = std::time(nullptr); std::tm time{}; localtime_r(&now, &time); return day_number(time); }
std::string day_label(std::int64_t value)
{
    auto const time = local_time(value);
    auto const days = today() - day_number(time);
    if (days == 0) { return "今天"; }
    if (days == 1) { return "昨天"; }
    auto now = std::time(nullptr); std::tm current{}; localtime_r(&now, &current);
    return format_time(time, time.tm_year == current.tm_year ? "%m月%d日" : "%Y年%m月%d日");
}
// The time column of the chat list: today's time, yesterday, a weekday this week, else the date.
std::string list_time(std::int64_t value)
{
    if (value <= 0) { return {}; }
    auto const time = local_time(value);
    auto const days = today() - day_number(time);
    if (days == 0) { return format_time(time, "%H:%M"); }
    if (days == 1) { return "昨天"; }
    if (days > 1 && days < 7)
    {
        constexpr std::array<char const*, 7> weekdays{"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        return weekdays[static_cast<std::size_t>(time.tm_wday)];
    }
    auto now = std::time(nullptr); std::tm current{}; localtime_r(&now, &current);
    return format_time(time, time.tm_year == current.tm_year ? "%m-%d" : "%Y-%m-%d");
}
std::string file_size(std::int64_t bytes)
{
    std::array<char, 32> output{};
    if (bytes < 1024) { return std::to_string(bytes) + " B"; }
    if (bytes < 1024 * 1024) { std::snprintf(output.data(), output.size(), "%.1f KB", static_cast<double>(bytes) / 1024); }
    else { std::snprintf(output.data(), output.size(), "%.1f MB", static_cast<double>(bytes) / (1024 * 1024)); }
    return output.data();
}
std::string presence_label(state const& s, std::int64_t id)
{
    if (!s.is_contact(id)) { return {}; }
    auto p = s.presences.find(id);
    if (p == s.presences.end()) { return {}; }
    if (p->second.online) { return "在线"; }
    return p->second.last_seen ? "最后在线 " + timestamp(p->second.last_seen) : "离线";
}
std::string role_label(member_role role)
{
    switch (role) { case member_role::owner: return "群主"; case member_role::admin: return "管理员"; default: return "成员"; }
}
std::string link_label(connection link)
{
    switch (link)
    {
        case connection::online: return "● 已连接";
        case connection::connecting: return "○ 正在连接…";
        case connection::authenticating: return "○ 正在认证…";
        case connection::reconnecting: return "○ 正在重连…";
        default: return "○ 未登录";
    }
}
Element selected(Element item, bool value, bool active = true)
{
    if (!value) { return item; }
    return active ? item | inverted | focus : item | focus;
}
Element scroll(Elements items)
{
    if (items.empty()) { items.push_back(text("暂无内容") | dim); }
    return vbox(std::move(items)) | vscroll_indicator | yframe | flex;
}
Element conversation_list(state const& s, int width)
{
    Elements rows;
    if (s.conversations.empty())
    {
        rows.push_back(text("暂无聊天"));
        rows.push_back(text("按 N 添加好友或创建群聊") | dim);
    }
    for (std::size_t i = 0; i < s.conversations.size(); ++i)
    {
        auto const& c = s.conversations[i];
        bool const current = s.conversation_selected == static_cast<int>(i);
        // A text mark as well as reverse video, so the focus survives monochrome terminals.
        std::string const mark = current && s.view == page::conversations ? "> " : "  ";
        auto label = mark + c.username;
        if (c.kind == conversation_kind::direct)
        {
            auto presence = presence_label(s, c.user);
            if (!presence.empty()) { label += " · " + presence; }
        }
        // Text marks, not only color, so they survive monochrome terminals.
        std::string flags;
        if (c.pinned) { flags += " 置顶"; }
        if (c.muted) { flags += " 免打扰"; }
        auto const when = " " + list_time(c.last.timestamp);
        auto const unread = c.unread ? " " + std::to_string(c.unread) : std::string{};
        auto summary = c.last.deleted ? "消息已删除" : c.last.attachment ? "[文件] " + c.last.attachment->filename : c.last.text;
        auto const name_width = width - DisplayWidth(when) - DisplayWidth(flags);
        auto unread_mark = text(unread);
        if (!unread.empty() && !c.muted) { unread_mark = unread_mark | bold; }
        rows.push_back(selected(vbox({
            hbox({preview_text(label, name_width) | bold | flex, text(flags) | dim, text(when) | dim}),
            hbox({text("  "), preview_text(summary, width - 2 - DisplayWidth(unread)) | dim | flex, unread_mark})}),
            current, s.view == page::conversations));
    }
    if (s.next_conversations) { rows.push_back(text("↓ 更多会话") | dim); }
    return vbox({text("聊天") | bold, separator(), scroll(std::move(rows))}) | flex;
}
// A new group starts with another sender, another day, after five quiet minutes, or after a
// deleted message. Only the heading is shared; every message stays selectable on its own.
bool starts_group(std::vector<message> const& values, std::size_t index)
{
    if (index == 0) { return true; }
    auto const& previous = values[index - 1];
    auto const& current = values[index];
    return previous.from != current.from || previous.deleted ||
        day_number(local_time(previous.timestamp)) != day_number(local_time(current.timestamp)) ||
        current.timestamp - previous.timestamp > 5 * 60 * 1000;
}
std::string message_heading(state const& s, message const& m)
{
    // Search results span days, so they keep the date; history has day separators instead.
    return (m.from == s.self.id ? "我" : m.username) + " " +
        (s.view == page::search ? timestamp(m.timestamp) : clock_time(m.timestamp));
}
Element message_item(state const& s, message const& m, bool highlighted, int width, int scroll_line, bool heading = true)
{
    auto const content_width = std::max(1, width * 3 / 4);
    Elements lines;
    if (heading) { lines.push_back(preview_text(message_heading(s, m), content_width) | bold); }
    if (m.reply)
    {
        lines.push_back(preview_text("↪ " + m.reply->username + ": " + (m.reply->deleted ? "消息已删除" : m.reply->text), content_width) | dim);
    }
    if (m.deleted) { lines.push_back(text("消息已删除") | dim); }
    else
    {
        // The edited mark belongs to its own message, wherever the shared heading is.
        auto const edited = m.edited_at ? std::string("（已编辑）") : std::string{};
        if (!m.text.empty()) { lines.push_back(wrapped_text(m.text + edited, content_width)); }
        if (m.attachment)
        {
            auto const& a = *m.attachment;
            lines.push_back(text(std::string(a.media_type.starts_with("image/") ? "[图片] " : "[文件] ") + a.filename + " · " + file_size(a.size) +
                                 (m.text.empty() ? edited : std::string{})));
        }
        std::string reaction_text;
        for (auto const& reaction : m.reactions)
        {
            reaction_text += reaction.emoji + " " + std::to_string(reaction.users.size());
            if (std::ranges::find(reaction.users, s.self.id) != reaction.users.end()) { reaction_text += "*"; }
            reaction_text += "  ";
        }
        if (!reaction_text.empty()) { lines.push_back(text(reaction_text)); }
        if (!m.mentions.empty())
        {
            std::string mentions = "提及：";
            for (auto const& value : m.mentions) { mentions += " @" + value.username; }
            lines.push_back(text(mentions) | dim);
        }
    }
    if (m.from == s.self.id)
    {
        auto c = s.active_conversation();
        auto read_count = std::ranges::count_if(s.read_positions, [&](auto const& p) {
            if (p.user == s.self.id || p.message < m.id) { return false; }
            return !c || c->kind != conversation_kind::group ||
                std::ranges::find(s.members, p.user, &conversation_member::id) != s.members.end();
        });
        lines.push_back(text(c && c->kind == conversation_kind::group ? "已读 " + std::to_string(read_count) + " 人" : read_count ? "已读" : "已发送") | dim);
    }
    auto item = vbox(std::move(lines)) | size(WIDTH, LESS_THAN, content_width);
    if (highlighted)
    {
        item->ComputeRequirement();
        auto const last_line = std::max(1, item->requirement().min_y - 1);
        float position = scroll_line < 0 ? 1.f : static_cast<float>(std::clamp(scroll_line, 0, last_line)) / last_line;
        // While typing, the newest message keeps the view at the bottom without a highlight.
        if ((s.view == page::conversation && !s.composing) || s.view == page::search) { item = item | inverted; }
        item = item | focusPositionRelative(0.f, position);
    }
    return m.from == s.self.id ? hbox({filler(), item}) : hbox({item, filler()});
}
// The chat history; rows is how many lines it gets on screen.
Element history(state const& s, int width, int rows, int message_scroll)
{
    Elements items;
    struct placed { int start = 0, height = 0; int message = -1; bool heading = false; };
    std::vector<placed> layout;
    int total = 0, focus = 0;
    auto add = [&](Element item, int message = -1, bool heading = false) {
        item->ComputeRequirement();
        auto const height = item->requirement().min_y;
        if (item->requirement().focused.enabled) { focus = total + item->requirement().focused.box.y_min; }
        layout.push_back({total, height, message, heading});
        total += height;
        items.push_back(std::move(item));
    };
    if (s.messages.empty()) { add(text("暂无消息") | dim); }
    if (s.history_more) { add(text("PgUp 加载更早的消息") | dim); }
    int day = 0;
    for (std::size_t i = 0; i < s.messages.size(); ++i)
    {
        auto const& m = s.messages[i];
        if (auto const when = m.timestamp; when > 0 && day_number(local_time(when)) != day)
        {
            day = day_number(local_time(when));
            add(hbox({filler(), text("── " + day_label(when) + " ──") | dim, filler()}));
        }
        auto const heading = starts_group(s.messages, i);
        // Groups are separated by a blank line; messages within a group follow each other.
        if (heading && i > 0) { add(text("")); }
        add(message_item(s, m, s.message_selected == static_cast<int>(i), width, message_scroll, heading),
            static_cast<int>(i), heading);
    }
    auto list = scroll(std::move(items));
    if (rows < 2) { return list; }
    // The first visible row, as the frame scrolls: the focus is centered, then clamped.
    auto const top = std::clamp(focus - (rows - 1) / 2, 0, std::max(0, total - rows));
    auto const at = std::ranges::find_if(layout, [&](placed const& p) { return p.start <= top && top < p.start + p.height; });
    // A message whose heading is above the view gets its sender and time pinned at the top.
    if (at == layout.end() || at->message < 0 || (at->heading && top == at->start)) { return list; }
    auto const& m = s.messages[static_cast<std::size_t>(at->message)];
    // flex keeps the history shrinkable inside the chat column, as the plain list is.
    return dbox({list, vbox({hbox({preview_text(message_heading(s, m), width) | bold, filler()}) | clear_under, filler()})}) | flex;
}
std::string send_hint(state const& s, chat::conversation const& c)
{
    if (s.link != connection::online) { return "正在等待连接…"; }
    if (c.kind == conversation_kind::direct) { return s.friendship_hint(c.user); }
    return "当前会话不可发送消息";
}
int wrapped_lines(std::string const& value, int width)
{
    return std::max(1, (DisplayWidth(value) + std::max(1, width) - 1) / std::max(1, width));
}
// Rows above and below the history in the chat page, including the outer frame,
// header, separators and status line.
int chat_chrome(state const& s, int width, bool typing)
{
    auto const* c = s.active_conversation();
    if (!c) { return 0; }
    int rows = 6 + 3;  // frame, app header, separators and status; chat title and both history separators
    if (c->pinned_message || !c->announcement.empty()) { ++rows; }
    if (typing) { ++rows; }
    if (!s.can_send()) { return rows + wrapped_lines(send_hint(s, *c), width); }
    if (s.reply) { ++rows; }
    if (s.editing) { ++rows; }
    return rows;
}
// The composer grows with its lines up to four, but returns to one line while that
// would leave the history fewer than eight rows.
int composer_lines(state const& s, int width, int height, bool typing)
{
    auto const wanted = std::clamp(static_cast<int>(std::ranges::count(s.draft, '\n')) + 1, 1, 4);
    return height - chat_chrome(s, width, typing) - wanted < 8 ? 1 : wanted;
}
Element conversation_view(state const& s, Element input, std::string typing, int width, int height, int message_scroll)
{
    auto c = s.active_conversation();
    if (!c) { return text("选择一个会话，按 Enter 打开") | center | flex; }
    auto detail = c->kind == conversation_kind::group ? " · " + std::to_string(c->member_count) + " 位成员" : presence_label(s, c->user);
    if (c->kind == conversation_kind::direct && !detail.empty()) { detail = " · " + detail; }
    Elements items{hbox({preview_text(c->username, width - DisplayWidth(detail)) | bold | flex, text(detail) | dim})};
    // Pinned message and announcement share one line; the full announcement is in the group page.
    if (c->pinned_message)
    {
        std::string const more = c->announcement.empty() ? "" : " 另有公告 · 见群信息";
        items.push_back(hbox({preview_text("置顶：" + (c->pinned_message->deleted ? "消息已删除" : c->pinned_message->text), width - DisplayWidth(more)) | dim | flex, text(more) | dim}));
    }
    else if (!c->announcement.empty()) { items.push_back(preview_text("公告：" + c->announcement, width) | dim); }
    items.push_back(separator());
    auto const rows = height - chat_chrome(s, width, !typing.empty()) -
        (input ? composer_lines(s, width, height, !typing.empty()) : s.can_send() ? 1 : 0);
    items.push_back(history(s, width, rows, message_scroll));
    if (!typing.empty()) { items.push_back(text(typing) | dim); }
    if (s.at_latest) { items.push_back(separator()); }
    else { items.push_back(preview_text("── 正在浏览历史 · G 回到最新，新消息保持未读 ──", width) | dim); }
    if (!s.can_send()) { items.push_back(wrapped_text(send_hint(s, *c), width)); }
    else
    {
        if (s.reply) { items.push_back(preview_text("回复 " + s.reply->username + "：" + s.reply->text, width) | dim); }
        if (s.editing) { items.push_back(text("正在编辑消息 · Esc 保留草稿") | dim); }
        if (input)
        {
            auto const lines = composer_lines(s, width, height, !typing.empty());
            items.push_back(hbox({text("› "), input | flex}) | size(HEIGHT, EQUAL, lines));
        }
        else { items.push_back(preview_text(s.composing ? "› " + s.draft : s.draft.empty() ? "按 i 输入消息" : "按 i 继续输入 · " + s.draft, width)); }
    }
    return vbox(std::move(items)) | flex;
}
Element secondary(state const& s, int width, int message_scroll)
{
    Elements rows;
    std::string title;
    std::string hint;
    switch (s.view)
    {
        case page::new_action:
        {
            title = "新建";
            auto items = actions(s);
            for (std::size_t i = 0; i < items.size(); ++i)
            { rows.push_back(selected(text(items[i].label), s.selected == static_cast<int>(i))); }
            break;
        }
        case page::contacts:
        case page::users:
        case page::pick_contacts:
        {
            title = s.view == page::contacts ? "联系人" : s.view == page::users ? "查找用户" : "选择好友";
            if (s.view == page::users && !s.users.empty()) { hint = "Enter 查看资料"; }
            if (s.view == page::pick_contacts) { hint = "Space 勾选 · Enter 下一步"; }
            if (s.view == page::users && s.users.empty()) { rows.push_back(text("没有找到匹配的用户") | dim); }
            if (s.view == page::contacts && s.contacts.empty()) { rows.push_back(text("还没有好友") | dim); }
            if (s.view == page::contacts) { rows.push_back(preview_text("/ 搜索 · " + s.contacts_query, width) | dim); }
            if (s.view == page::pick_contacts)
            {
                rows.push_back(text("已选 " + std::to_string(s.picked_contacts.size()) + " 人 · / 搜索 · " + s.pick_query));
                // Selected friends remain removable even while the current filter hides them.
                std::string picked = "已选：";
                for (auto id : s.picked_contacts)
                {
                    auto found = std::ranges::find(s.contacts, id, &user::id);
                    if (found != s.contacts.end()) { picked += found->username + "；"; }
                }
                rows.push_back(preview_text(picked, width));
                auto values = s.pick_candidates();
                for (std::size_t i = 0; i < values.size(); ++i)
                {
                    auto const& value = *values[i];
                    auto checked = std::ranges::find(s.picked_contacts, value.id) != s.picked_contacts.end();
                    rows.push_back(selected(text(std::string(checked ? "[x] " : "[ ] ") + user_label(value.username)), s.selected == static_cast<int>(i)));
                }
            }
            else
            {
                auto contacts = s.visible_contacts();
                auto count = s.view == page::users ? s.users.size() : contacts.size();
                for (std::size_t i = 0; i < count; ++i)
                {
                    auto const& value = s.view == page::users ? s.users[i] : *contacts[i];
                    auto offset = s.view == page::contacts ? 1 : 0;
                    auto label = user_label(value.username);
                    auto presence = presence_label(s, value.id);
                    if (!presence.empty()) { label += " · " + presence; }
                    rows.push_back(selected(text(label), s.selected == static_cast<int>(i) + offset));
                }
            }
            if (s.view == page::contacts && !s.contacts_query.empty())
            {
                // Beyond the friends already listed, the same words can find anyone on the server.
                auto const index = static_cast<int>(s.visible_contacts().size()) + 1;
                rows.push_back(selected(preview_text("查找用户“" + s.contacts_query + "”", width), s.selected == index));
            }
            if (s.view == page::contacts)
            {
                return vbox({text(title) | bold, separator(),
                    selected(text("新的朋友 (" + std::to_string(s.friends.incoming.size()) + ")"), s.selected == 0),
                    separator(), scroll(std::move(rows))}) | flex;
            }
            break;
        }
        case page::friend_requests:
        case page::friend_sent:
        {
            // One list in two groups, as in Qt: received requests, then sent ones.
            title = "新的朋友";
            hint = "y 接受 · n 拒绝（收到）· x 撤回（发出）· Enter 查看资料";
            int index = 0;
            for (auto [label, requests] : {std::pair{"收到", &s.friends.incoming}, std::pair{"发出", &s.friends.outgoing}})
            {
                rows.push_back(text(std::string(label) + " (" + std::to_string(requests->size()) + ")") | bold);
                if (requests->empty()) { rows.push_back(text("  暂无") | dim); }
                for (auto const& request : *requests)
                {
                    rows.push_back(selected(text("  " + user_label(request.user.username) + " " + timestamp(request.created_at)),
                                            s.selected == index));
                    ++index;
                }
            }
            break;
        }
        case page::profile:
        {
            title = std::string(s.profile.id == s.self.id ? "账号 · " : "资料 · ") + user_label(s.profile.username);
            rows.push_back(text(std::string("头像：") + (s.profile.avatar.present ? "已设置" : "默认")));
            auto presence = presence_label(s, s.profile.id);
            if (!presence.empty()) { rows.push_back(text(presence)); }
            if (s.profile.id != s.self.id)
            {
                auto relation = s.friendship(s.profile.id);
                rows.push_back(text(relation == friendship_state::accepted ? "好友" :
                    relation == friendship_state::outgoing_pending ? "等待对方确认" :
                    relation == friendship_state::incoming_pending ? "收到好友申请" : "不是好友"));
            }
            rows.push_back(separator());
            auto items = actions(s);
            for (std::size_t i = 0; i < items.size(); ++i) { rows.push_back(selected(text(items[i].label), s.selected == static_cast<int>(i))); }
            break;
        }
        case page::members:
            title = "群成员 (" + std::to_string(s.members.size()) + ")";
            if (s.self_role() == member_role::owner) { hint = "A 设置或取消管理员 · O 转让给管理员 · D 移除"; }
            else if (s.self_role() == member_role::admin) { hint = "D 移除所选成员"; }
            for (std::size_t i = 0; i < s.members.size(); ++i)
            {
                auto const& m = s.members[i];
                auto role = " · " + role_label(m.role) + (m.id == s.self.id ? "（我）" : "");
                rows.push_back(selected(hbox({preview_text(user_label(m.username), width - DisplayWidth(role)) | flex,
                    text(role) | dim}), s.selected == static_cast<int>(i)));
            }
            break;
        case page::requests:
            title = "待处理的入群申请";
            if (!s.requests.empty()) { hint = "y 接受 · n 拒绝"; }
            if (s.requests.empty()) { rows.push_back(text("暂无入群申请") | dim); }
            for (std::size_t i = 0; i < s.requests.size(); ++i)
            {
                auto const& value = s.requests[i];
                rows.push_back(selected(text(user_label(value.applicant.username) + " " + timestamp(value.created_at)), s.selected == static_cast<int>(i)));
            }
            if (s.next_requests) { rows.push_back(text("PgDn 加载更多申请") | dim); }
            break;
        case page::search:
            title = "搜索：" + s.search_query;
            hint = s.search_results.empty() ? "/ 重新搜索" : "Enter 或 y 显示可复制文本";
            rows.push_back(text("显示已加载的结果，重新搜索可获取最新匹配") | dim);
            if (s.search_results.empty()) { rows.push_back(text("已加载的结果中没有匹配项") | dim); }
            for (std::size_t i = 0; i < s.search_results.size(); ++i) { rows.push_back(message_item(s, s.search_results[i], s.selected == static_cast<int>(i), width, message_scroll)); rows.push_back(text("")); }
            if (s.search_more) { rows.push_back(text("PgDn 加载更早的结果") | dim); }
            break;
        case page::group:
        {
            auto c = s.active_conversation();
            title = c ? "群聊 · " + c->username : "群聊";
            if (c)
            {
                rows.push_back(text("成员 " + std::to_string(s.members.size()) + " 人 · 我的身份：" + role_label(s.self_role())));
                auto const preview_count = std::min<std::size_t>(3, s.members.size());
                std::string preview = "前 " + std::to_string(preview_count) + " 位成员（共 " + std::to_string(s.members.size()) + " 位）：";
                for (std::size_t i = 0; i < preview_count; ++i)
                { preview += (i ? "、" : "") + s.members[i].username + " " + role_label(s.members[i].role); }
                rows.push_back(preview_text(preview, width));
                if (c->pinned_message) { rows.push_back(preview_text("置顶：" + c->pinned_message->text, width)); }
                rows.push_back(wrapped_text("公告：" + (c->announcement.empty() ? "（无）" : c->announcement), width) | size(HEIGHT, LESS_THAN, 3));
                rows.push_back(text(c->join_approval ? "入群审批：已开启" : "入群审批：已关闭"));
                rows.push_back(separator());
            }
            auto items = actions(s);
            for (std::size_t i = 0; i < items.size(); ++i) { rows.push_back(selected(text(items[i].label), s.selected == static_cast<int>(i))); }
            break;
        }
        case page::history:
        {
            auto c = s.active_conversation();
            title = "聊天记录" + (c ? " · " + c->username : std::string{});
            hint = "←→ 分类 · Enter 定位到聊天 · / 搜索 · PgDn 加载更早";
            Elements tabs;
            constexpr std::array<char const*, 4> categories{"全部", "图片", "文件", "链接"};
            for (int i = 0; i < 4; ++i)
            {
                auto tab = text(std::string(i == s.history_category ? "[" : " ") + categories[static_cast<std::size_t>(i)] +
                                (i == s.history_category ? "]" : " "));
                tabs.push_back(i == s.history_category ? tab | bold : tab | dim);
                tabs.push_back(text(" "));
            }
            rows.push_back(hbox(std::move(tabs)));
            auto const entries = s.history_entries();
            if (entries.empty()) { rows.push_back(text("已加载的消息中没有这一类") | dim); }
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                auto const& m = *entries[i];
                auto body = m.attachment ? (m.attachment->media_type.starts_with("image/") ? "[图片] " : "[文件] ") + m.attachment->filename
                                         : m.text;
                rows.push_back(selected(preview_text(timestamp(m.timestamp) + "  " + (m.from == s.self.id ? "我" : m.username) + "：" + body, width),
                                        s.selected == static_cast<int>(i)));
            }
            if (s.history_more) { rows.push_back(text("PgDn 加载更早的消息") | dim); }
            break;
        }
        case page::help:
            title = "键盘帮助";
            hint = "j/k 滚动 · Esc 返回";
            rows.push_back(help_content(width));
            break;
        case page::copy:
            title = "可复制文本";
            hint = "用终端的选择功能复制 · Esc 返回";
            rows.push_back(wrapped_text(s.copy_text, width));
            break;
        default: title = "Chat"; break;
    }
    Elements panel{(s.view == page::profile ? wrapped_text(title, width) : preview_text(title, width)) | bold};
    if (!hint.empty()) { panel.push_back(text(hint) | dim); }
    panel.push_back(separator());
    if (s.view == page::help || s.view == page::copy)
    {
        auto body = vbox(std::move(rows));
        body->ComputeRequirement();
        auto const last_line = std::max(1, body->requirement().min_y - 1);
        body = body | focusPositionRelative(0.f, static_cast<float>(std::clamp(s.selected, 0, last_line)) / last_line) | vscroll_indicator | yframe | flex;
        panel.push_back(body);
    }
    else { panel.push_back(scroll(std::move(rows))); }
    return vbox(std::move(panel)) | flex;
}
bool people_page(page view) { return view == page::contacts || view == page::profile || view == page::friend_requests || view == page::friend_sent; }
bool group_page(page view) { return view == page::group || view == page::members || view == page::requests; }
// The contacts list as the left column of the wide layout. It is highlighted only on the
// contacts page; elsewhere the selection belongs to the right side.
Element contacts_column(state const& s, int width)
{
    bool const active = s.view == page::contacts;
    Elements rows;
    auto const contacts = s.visible_contacts();
    if (contacts.empty()) { rows.push_back(text(s.contacts.empty() ? "还没有好友" : "没有匹配的好友") | dim); }
    for (std::size_t i = 0; i < contacts.size(); ++i)
    {
        auto label = contacts[i]->username;
        auto const presence = presence_label(s, contacts[i]->id);
        if (!presence.empty()) { label += " · " + presence; }
        rows.push_back(selected(preview_text((active && s.selected == static_cast<int>(i) + 1 ? "> " : "  ") + label, width),
                                active && s.selected == static_cast<int>(i) + 1));
    }
    if (active && !s.contacts_query.empty())
    {
        auto const index = static_cast<int>(contacts.size()) + 1;
        rows.push_back(selected(preview_text("查找用户“" + s.contacts_query + "”", width), s.selected == index));
    }
    auto requests = "新的朋友 (" + std::to_string(s.friends.incoming.size()) + ")";
    return vbox({text("联系人") | bold, preview_text("/ 搜索 · " + s.contacts_query, width) | dim, separator(),
                 selected(text(requests), active && s.selected == 0), separator(), scroll(std::move(rows))}) | flex;
}
// The selected contact at a glance; Enter opens the profile with its actions.
Element contact_card(state const& s, int width)
{
    auto const* person = s.selected_user();
    if (!person)
    {
        return vbox({text(s.selected == 0 ? "新的朋友：收到和发出的好友申请" : "选择一位联系人查看资料") | dim, text(""),
                     text("Enter 打开") | dim}) | flex;
    }
    Elements rows{preview_text(user_label(person->username), width) | bold};
    auto const presence = presence_label(s, person->id);
    if (!presence.empty()) { rows.push_back(text(presence)); }
    rows.push_back(text(std::string("头像：") + (person->avatar.present ? "已设置" : "默认")));
    rows.push_back(text("好友"));
    rows.push_back(separator());
    rows.push_back(text("Enter 打开资料：发消息、删除好友") | dim);
    return vbox(std::move(rows)) | flex;
}
Element render_impl(state const& s, int width, int height, Element compose = {}, std::string typing = {}, int message_scroll = -1)
{
    if (state::layout(width, height) == layout_mode::too_small) { return text("终端太小（至少 40×12）") | center; }
    if (!s.self.id)
    {
        return vbox({text("用户名"), text(""), text("密码"), text(""),
                     hbox({text("登录") | bold, text("  "), text("注册账号"), text("  "), text("设置")}),
                     paragraph(s.status), text("Tab 切换 · ←→ 选按钮 · Enter 确认 · Ctrl+C 退出") | dim}) |
            size(WIDTH, LESS_THAN, std::min(44, width - 4)) | center;
    }
    Element content;
    if (s.view == page::conversations || s.view == page::conversation)
    {
        if (state::layout(width, height) == layout_mode::wide)
        {
            content = hbox({conversation_list(s, 29) | size(WIDTH, EQUAL, 30), separator(), conversation_view(s, compose, std::move(typing), width - 34, height, message_scroll)}) | flex;
        }
        else { content = s.view == page::conversations ? conversation_list(s, width - 3) : conversation_view(s, compose, std::move(typing), width - 3, height, message_scroll); }
    }
    else if (state::layout(width, height) == layout_mode::wide && people_page(s.view))
    {
        // People stay listed on the left; the right shows the selected contact or the open page.
        auto right = s.view == page::contacts ? contact_card(s, width - 34) : secondary(s, width - 34, message_scroll);
        content = hbox({contacts_column(s, 29) | size(WIDTH, EQUAL, 30), separator(), right | flex}) | flex;
    }
    else if (state::layout(width, height) == layout_mode::wide && group_page(s.view))
    {
        // Group information opens beside the chat list, as a panel rather than a new screen.
        content = hbox({conversation_list(s, 29) | size(WIDTH, EQUAL, 30), separator(), secondary(s, width - 34, message_scroll) | flex}) | flex;
    }
    else { content = secondary(s, width - 3, message_scroll); }
    auto link = " " + link_label(s.link);
    std::string const help = " Esc 返回 · F1 帮助";
    std::string keys = "h 聊天 · c 联系人 · u 账号 · N 新建 · : 命令";
    if (s.view == page::conversation && s.composing) { keys = "Enter 发送 · \\ Enter 换行 · Shift+Tab 选择消息"; }
    else if (s.view == page::conversation) { keys = "↑↓ 选择 · Enter 操作 · r 回复 · e 编辑 · y 复制 · i 输入"; }
    else if (s.view == page::conversations) { keys = "Enter 打开 · Tab 切换焦点 · N 新建 · : 命令"; }
    auto status = s.status.empty() ? preview_text(keys, width - 2 - DisplayWidth(help)) | dim
                                   : preview_text(s.status, width - 2 - DisplayWidth(help));
    if (s.status_error) { status = status | bold; }
    return vbox({hbox({preview_text("Chat · " + s.self.username, width - 2 - DisplayWidth(link)) | bold | flex, text(link)}), separator(), content, separator(), hbox({status | flex, text(help) | dim})}) | border;
}
// One command palette entry; an empty command inserts a line break in the composer.
struct palette_entry
{
    std::string label; std::string command; bool line_break = false;
    bool operator==(palette_entry const&) const = default;
};
std::vector<palette_entry> palette_entries(state const& s)
{
    std::vector<palette_entry> entries;
    auto const* c = s.active_conversation();
    bool const chat = s.view == page::conversation && c;
    if (chat && s.can_send()) { entries.push_back({"插入换行", "", true}); entries.push_back({"发送文件", "file"}); }
    if (chat)
    {
        entries.push_back({"搜索聊天记录", "search"});
        entries.push_back({"聊天记录（图片、文件、链接）", "history"});
        entries.push_back({c->muted ? "取消免打扰" : "免打扰", "mute"});
        entries.push_back({c->pinned ? "取消置顶会话" : "置顶会话", "pin"});
        if (c->kind == conversation_kind::group)
        {
            entries.push_back({"群信息", "group"});
            entries.push_back({"群成员", "members"});
            if (!c->announcement.empty()) { entries.push_back({"查看完整公告", "show-announcement"}); }
            if (c->pinned_message) { entries.push_back({"查看置顶消息", "pinned"}); }
            if (s.self_role() != member_role::member)
            {
                entries.push_back({"邀请好友入群", "invite"});
                entries.push_back({"查看邀请码", "link"});
                entries.push_back({"入群申请", "requests"});
            }
            if (s.self_role() != member_role::owner) { entries.push_back({"退出群聊", "leave"}); }
        }
    }
    for (auto const& entry : std::initializer_list<palette_entry>{
             {"聊天", "chats"}, {"联系人", "contacts"}, {"新的朋友", "friend-requests"}, {"账号", "account"},
             {"添加好友", "add-contact"}, {"创建群聊", "create-group"}, {"加入群聊", "join"}, {"帮助", "help"},
             {"重新连接", "reconnect"}, {"退出登录", "logout"}, {"退出程序", "quit"}})
    { entries.push_back(entry); }
    return entries;
}
// Case-insensitive subsequence match, so "sf" finds "search" and "发文" finds 发送文件.
bool fuzzy_match(std::string const& query, std::string const& value)
{
    auto lower = [](std::string text) {
        for (char& c : text) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
        return text;
    };
    auto const wanted = Utf8ToGlyphs(lower(query));
    auto const glyphs = Utf8ToGlyphs(lower(value));
    std::size_t at = 0;
    for (auto const& glyph : wanted)
    {
        if (glyph.empty() || glyph == " ") { continue; }
        while (at < glyphs.size() && glyphs[at] != glyph) { ++at; }
        if (at == glyphs.size()) { return false; }
        ++at;
    }
    return true;
}
std::vector<palette_entry> palette_matches(state const& s, std::string const& query)
{
    // ":name args" runs a command line as before, so existing commands keep working.
    if (!query.empty() && query.front() == ':') { return {{"执行命令 " + query, query.substr(1)}}; }
    std::vector<palette_entry> found;
    for (auto& entry : palette_entries(s))
    { if (fuzzy_match(query, entry.label) || fuzzy_match(query, entry.command)) { found.push_back(std::move(entry)); } }
    return found;
}
std::size_t selection_count(state const& s, int width)
{
    switch (s.view)
    {
        case page::conversations: return s.conversations.size();
        case page::contacts: return s.visible_contacts().size() + 1 + (s.contacts_query.empty() ? 0 : 1);
        case page::pick_contacts: return s.pick_candidates().size();
        case page::friend_requests:
        case page::friend_sent: return s.friends.incoming.size() + s.friends.outgoing.size();
        case page::users: return s.users.size();
        case page::members: return s.members.size();
        case page::requests: return s.requests.size();
        case page::search: return s.search_results.size();
        case page::history: return s.history_entries().size();
        case page::new_action: case page::profile: case page::group: return actions(s).size();
        case page::help:
        {
            // The help text is fixed, so its height per width is computed once.
            static std::unordered_map<int, int> heights;
            auto [found, added] = heights.try_emplace(width, 0);
            if (added) { help_content(width, &found->second); }
            return static_cast<std::size_t>(found->second);
        }
        case page::copy:
        {
            auto body = wrapped_text(s.copy_text, width);
            body->ComputeRequirement();
            return static_cast<std::size_t>(std::max(1, body->requirement().min_y));
        }
        default: return 0;
    }
}
class terminal_ui final : public ComponentBase
{
public:
    terminal_ui(app& value, std::function<void()> quit) : app_(value), quit_(std::move(quit))
    {
        InputOption single;
        single.multiline = false;
        // Each field's cursor and insert mode are kept here, so a paste can edit the text at once.
        auto own = [](InputOption option, field_state& field) {
            option.cursor_position = &field.cursor;
            option.insert = &field.insert;
            return option;
        };
        username_ = Input(&app_.username, "用户名", own(single, username_field_));
        auto password_option = own(single, password_field_);
        password_option.password = true;
        password_option.on_enter = [this] { app_.login(); };
        password_ = Input(&app_.password, "密码", password_option);
        ButtonOption action;
        action.transform = [](EntryState const& item) {
            auto body = text((item.focused ? "> " : "  ") + item.label);
            if (item.label == "登录") { body |= bold; }
            if (item.focused) { body |= inverted; }
            return body;
        };
        login_ = Button("登录", [this] { app_.login(); }, action);
        register_ = Button("注册账号", [this] { app_.login(true); }, action);
        server_settings_ = Button("设置", [this] { open_settings(); }, action);
        // Settings: "IP:port" and a TLS box; the URL is made from them.
        auto address_option = own(single, address_field_);
        address_option.on_enter = [this] { save_settings(); };
        address_ = Input(&settings_address_, "127.0.0.1:18080", address_option);
        CheckboxOption tls_option = CheckboxOption::Simple();
        tls_ = Checkbox("TLS（wss://）", &settings_tls_, tls_option);
        settings_ok_ = Button("确定", [this] { save_settings(); }, action);
        settings_cancel_ = Button("取消", [this] { close_settings(); }, action);
        settings_form_ = Container::Vertical({address_, tls_, settings_ok_, settings_cancel_});
        // One focus chain (Tab and ↑↓ go through every field); the three buttons are drawn in a row.
        login_form_ = Container::Vertical({username_, password_, login_, register_, server_settings_});
        // The settings form takes the keys while open; one of the two is active at a time.
        Add(Container::Tab({login_form_, settings_form_}, &login_tab_));
        auto compose_option = single;
        compose_option.multiline = true;
        compose_option.on_change = [this] { app_.compose_changed(); };
        compose_option.cursor_position = &compose_cursor_;
        compose_option.insert = &compose_insert_;
        compose_ = Input(&app_.data.draft, "输入消息", compose_option);
        Add(compose_);
        command_ = Input(&app_.command_text, "命令", own(single, command_field_));
        Add(command_);
        prompt_ = Input(&prompt_text_, "", own(single, prompt_field_));
        Add(prompt_);
        palette_input_ = Input(&palette_query_, "输入命令或关键字", own(single, palette_field_));
        Add(palette_input_);
        // Pasted text the UI still holds goes into the draft before that draft is put away.
        app_.before_input_change = [this] { flush_paste(true); };
        // With the last account filled in, only its password is left to type.
        (app_.username.empty() ? username_ : password_)->TakeFocus();
        username_field_.cursor = static_cast<int>(app_.username.size());
    }
    ~terminal_ui() override { app_.before_input_change = nullptr; }
    Element OnRender() override
    {
        flush_paste();
        validate_palette();
        validate_menu();
        if (app_.data.self.id) { app_.sync_focus(); }
        follow_draft();
        auto terminal = Terminal::Size();
        update_viewport(terminal);
        sync_message_scroll();
        auto& s = app_.data;
        Element page;
        if (state::layout(terminal.dimx, terminal.dimy) == layout_mode::too_small)
        { return render(s, terminal.dimx, terminal.dimy); }
        if (!s.self.id)
        {
            auto const width = std::min(44, terminal.dimx - 4);
            auto const roomy = terminal.dimy >= 20;
            Elements fields;
            fields.push_back(text("用户名"));
            fields.push_back(username_->Render());
            if (roomy) { fields.push_back(text("")); }
            fields.push_back(text("密码"));
            fields.push_back(password_->Render());
            if (roomy) { fields.push_back(text("")); }
            fields.push_back(hbox({login_->Render(), text("  "), register_->Render(), text("  "), server_settings_->Render()}));
            if (!s.status.empty())
            { fields.push_back(wrapped_text(s.status, width)); }
            else if (s.link != connection::signed_out)
            { fields.push_back(text(link_label(s.link)) | bold); }
            if (roomy) { fields.push_back(text("")); }
            if (roomy || s.status.empty())
            { fields.push_back(paragraph("Tab 切换 · ←→ 选按钮 · Enter 确认 · Ctrl+C 退出") | dim); }
            page = vbox(std::move(fields)) | size(WIDTH, EQUAL, width) | center;
            if (settings_open_)
            {
                page = dbox({page, vbox({text("服务器") | bold, separator(), text("地址（IP:端口）"), address_->Render(),
                                         tls_->Render(),
                                         // An error is about what was saved; typing or pasting a change hides it.
                                         settings_error_.empty() || settings_error_for_ != std::pair{settings_address_, settings_tls_}
                                             ? text("") : paragraph(settings_error_) | bold,
                                         hbox({settings_ok_->Render(), text("  "), settings_cancel_->Render()}),
                                         text("Tab 切换 · Space 勾选 · Enter 确认 · Esc 取消") | dim}) |
                                       size(WIDTH, GREATER_THAN, 34) | border | clear_under | center});
            }
        }
        else
        {
            auto const typing = app_.typing_text();
            update_history_rows(terminal);
            page = render_impl(s, terminal.dimx, terminal.dimy, s.composing ? compose_->Render() : Element{}, typing, message_scroll_);
        }
        if (palette_open_) { page = dbox({overlays(std::move(page)), palette_view()}); return page; }
        return overlays(std::move(page));
    }
    Element palette_view()
    {
        auto const entries = palette_matches(app_.data, palette_query_);
        Elements rows{text("命令面板") | bold, hbox({text("> "), palette_input_->Render() | flex}), separator()};
        if (entries.empty()) { rows.push_back(text("没有匹配的命令") | dim); }
        auto const shown = std::min<std::size_t>(entries.size(), 10);
        for (std::size_t i = 0; i < shown; ++i)
        {
            auto const& entry = entries[i];
            auto row = hbox({text((static_cast<int>(i) == palette_selected_ ? "> " : "  ") + entry.label) | flex,
                             text(entry.command.empty() ? "" : " :" + entry.command) | dim});
            rows.push_back(static_cast<int>(i) == palette_selected_ ? row | inverted : row);
        }
        rows.push_back(separator());
        rows.push_back(text("↑↓ 选择 · Enter 执行 · :命令 直接执行 · Esc 关闭") | dim);
        return vbox(std::move(rows)) | size(WIDTH, EQUAL, 52) | border | clear_under | center;
    }
    Element overlays(Element page)
    {
        if (app_.dialog)
        {
            sync_prompt();
            return dbox({page, vbox({paragraph(app_.dialog->title) | bold, separator(), prompt_->Render(), text(app_.dialog->confirmation ? "输入 y 后按 Enter 确认 · Esc 取消" : "Enter 确认 · Esc 取消")}) | border | clear_under | center});
        }
        if (app_.command_mode)
        { return dbox({page, vbox({text("命令"), command_->Render(), text("Enter 执行 · Esc 取消")}) | border | clear_under | center}); }
        if (menu_)
        {
            auto const& items = menu_->items;
            Elements rows{preview_text(menu_->title, 40) | bold, separator()};
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                auto const& item = items[i];
                auto label = std::string(item.key ? std::string(1, item.key) + "  " : "   ") + item.label;
                if (!item.disabled.empty()) { label += "（" + item.disabled + "）"; }
                auto row = text((static_cast<int>(i) == menu_->selected ? "> " : "  ") + label);
                if (!item.disabled.empty()) { row = row | dim; }
                rows.push_back(static_cast<int>(i) == menu_->selected ? row | inverted : row);
            }
            rows.push_back(separator());
            rows.push_back(text("Enter 执行 · 字母直接执行 · Esc 关闭") | dim);
            return dbox({page, vbox(std::move(rows)) | border | clear_under | center});
        }
        return page;
    }
    bool OnEvent(Event event) override
    {
        follow_draft();
        // An error stays until the next key press; status changes are classified after handling.
        if (event != Event::Custom)
        {
            // Esc only clears a shown error, so it does not also leave the page.
            // An open dialog or command line is closed first; the error waits for the next Esc.
            bool const overlay = app_.dialog || app_.command_mode || menu_ || palette_open_;
            bool const clear_only = event == Event::Escape && app_.data.status_error && app_.data.self.id &&
                                    !pasting_ && !overlay;
            if (event != Event::Escape || !overlay) { app_.dismiss_error(); }
            if (clear_only) { newline_.reset(); app_.observe_status(); return true; }
        }
        auto const handled = handle(event);
        if (app_.data.self.id) { app_.sync_focus(); }
        follow_draft();
        // Read receipts use the history rows of the current size and state, after this event's
        // queued results are applied. A paste in progress is settled first.
        if (!pasting_)
        {
            // One size snapshot for both the minimum-size check and the visible history rows.
            auto const terminal = Terminal::Size();
            update_viewport(terminal);
            update_history_rows(terminal);
            if (app_.read_check) { app_.check_read(); }
        }
        app_.observe_status();
        return handled;
    }
private:
    bool handle(Event event)
    {
        update_viewport(Terminal::Size());
        // Pasted text belongs to the target as it was before queued results change it.
        app_.drain([this] { flush_paste(); });
        bool const had_menu = menu_.has_value();
        bool const had_palette = palette_open_;
        validate_palette();
        validate_menu();
        if (app_.data.self.id) { app_.sync_focus(); }
        // Queued results may have switched to another chat's draft: this key starts after it.
        follow_draft();
        // A key meant for a menu that just closed because its target changed does nothing else.
        // Paste markers still keep the paste protocol in step; pasted text without a target is
        // dropped by the paste branch below.
        bool const paste_marker = event == Event::Special("\x1b[200~") || event == Event::Special("\x1b[201~");
        bool const overlay_closed = (had_menu && !menu_ && !palette_open_) || (had_palette && !palette_open_);
        if (overlay_closed && event != Event::Custom && event != Event::CtrlC && !paste_marker) { return true; }
        sync_message_scroll();
        auto& s = app_.data;
        if (event == Event::CtrlC)
        {
            app_.shutdown();
            quit_();
            return true;
        }
        if (app_.exiting) { quit_(); return true; }
        if (event == Event::Special("\x1b[200~"))
        {
            if (!pasting_)
            {
                pasting_ = true;
                // A paste that began while a menu was open belongs to no input, even if that
                // menu closed in this event and the composer has just come back.
                paste_input_ = palette_open_ ? palette_input_ : had_menu || had_palette ? Component{} : input();
                paste_conversation_ = s.active;
                paste_buffer_.clear();
                paste_at_.reset();
            }
            newline_.reset();
            return true;
        }
        if (event == Event::Special("\x1b[201~"))
        { flush_paste(true); pasting_ = false; paste_input_.reset(); newline_.reset(); return true; }
        if (pasting_ && event != Event::Custom)
        {
            if (paste_input_ != input() || paste_conversation_ != s.active) { paste_buffer_.clear(); paste_input_.reset(); }
            if (paste_input_)
            {
                // Inserting character by character rescans the whole text each time, so a large
                // paste is collected and inserted at once.
                if (event == Event::Escape) { flush_paste(true); paste_input_.reset(); }
                else if (event.is_character()) { paste_buffer_ += event.character(); }
                else if (event == Event::Return) { paste_buffer_ += paste_input_ == compose_ ? '\n' : ' '; }
                else if (event == Event::Tab) { paste_buffer_ += ' '; }
            }
            return true;
        }
        if (event == Event::Custom)
        {
            // A pending backslash newline survives redraws, but not a program change to its text.
            if (newline_ && (!s.composing || newline_->conversation != s.active || newline_->draft != s.draft ||
                             newline_->cursor != compose_cursor_)) { newline_.reset(); }
            // An open settings form keeps the keys (and its field) across background events.
            if (!s.self.id && !settings_open_) { login_form_->TakeFocus(); }
            else if (s.composing) { compose_->TakeFocus(); }
            auto terminal = Terminal::Size();
            if (state::layout(terminal.dimx, terminal.dimy) != layout_mode::too_small) { app_.mark_visible_read(); }
            return true;
        }
        // Only Enter can use a backslash typed just before it; every other key ends that chance.
        auto const newline = std::exchange(newline_, std::nullopt);
        // The palette opens over anything and takes every key while open.
        if (palette_open_) { return palette_event(event); }
        if (event == Event::CtrlK && s.self.id) { open_palette(); return true; }
        if (app_.dialog)
        {
            sync_prompt();
            if (event == Event::Escape) { app_.cancel_prompt(); prompt_active_ = false; return true; }
            if (event == Event::Return)
            {
                app_.dialog->text = prompt_text_;
                app_.submit_prompt();
                prompt_active_ = false;
                return true;
            }
            return prompt_->OnEvent(event);
        }
        prompt_active_ = false;
        if (app_.command_mode)
        {
            command_->TakeFocus();
            if (event == Event::Escape) { app_.command_mode = false; return true; }
            if (event == Event::Return)
            {
                auto command = std::exchange(app_.command_text, {});
                app_.command_mode = false;
                if (command == "account" || command == ":account") { open_account_menu(); return true; }
                app_.command(std::move(command));
                if (app_.exiting) { quit_(); }
                return true;
            }
            return command_->OnEvent(event);
        }
        if (menu_) { return menu_event(event); }
        if (!s.self.id)
        {
            if (s.link != connection::signed_out)
            {
                if (event == Event::Escape) { app_.logout(); }
                return true;
            }
            if (settings_open_) { return settings_event(event); }
            // The buttons share a row, so ←→ move between them as Tab does.
            if (login_->Focused() || register_->Focused() || server_settings_->Focused())
            {
                if (event == Event::ArrowRight && !server_settings_->Focused()) { return login_form_->OnEvent(Event::Tab); }
                if (event == Event::ArrowLeft && !login_->Focused()) { return login_form_->OnEvent(Event::TabReverse); }
            }
            return login_form_->OnEvent(event);
        }
        bool const wide = state::layout(app_.viewport_width, app_.viewport_height) == layout_mode::wide;
        if (s.composing)
        {
            // The composer owns every printable character, including ? : / and letters.
            compose_->TakeFocus();
            if (event == Event::Escape)
            {
                // One level at a time: a reply or edit first, then back to the list.
                if (s.editing || s.reply) { s.editing = 0; s.reply.reset(); return true; }
                app_.stop_composing();
                app_.back();
                return true;
            }
            if (event == Event::TabReverse || (event == Event::ArrowUp && s.draft.empty()) || (event == Event::Tab && !wide))
            { s.selecting = true; app_.stop_composing(); return true; }
            if (event == Event::Tab) { app_.navigate(page::conversations); return true; }
            if (event == Event::F1) { app_.command("help"); return true; }
            // Reading older history is browsing the messages, so PgUp moves there.
            if (event == Event::PageUp)
            { s.selecting = true; app_.stop_composing(); s.at_latest = false; app_.history(true); return true; }
            if (event == Event::Return)
            {
                if (newline && newline->conversation == s.active && newline->draft == s.draft &&
                    newline->cursor == compose_cursor_ && compose_cursor_ > 0 &&
                    static_cast<std::size_t>(compose_cursor_) <= s.draft.size() && s.draft[compose_cursor_ - 1] == '\\')
                {
                    // Replace exactly that backslash; the text on both sides stays as it is.
                    s.draft[compose_cursor_ - 1] = '\n';
                    app_.compose_changed();
                    return true;
                }
                app_.send();
                return true;
            }
            if (event == Event::Special("\x1b\r") || event == Event::Special("\x1b\n"))
            { return compose_->OnEvent(Event::Character("\n")); }
            auto const handled = compose_->OnEvent(event);
            if (event == Event::Character("\\")) { newline_ = newline_mark{s.active, compose_cursor_, s.draft}; }
            return handled;
        }
        if (event == Event::F1) { app_.command("help"); return true; }
        if (event == Event::Escape)
        {
            // A reply or edit is cancelled first, wherever the focus is; the draft stays.
            if ((s.view == page::conversation || s.view == page::conversations) && (s.editing || s.reply))
            { s.editing = 0; s.reply.reset(); return true; }
            // From the messages Esc returns to the composer when there is one.
            if (s.view == page::conversation && s.selecting && s.can_send()) { s.selecting = false; return true; }
            app_.back();
            return true;
        }
        if (event == Event::Character(':')) { app_.command_mode = true; app_.command_text.clear(); command_->TakeFocus(); return true; }
        if (event == Event::Tab || event == Event::TabReverse)
        {
            // Wide: list -> messages -> composer -> list. Narrow: messages <-> composer; Esc reaches the list.
            bool const forward = event == Event::Tab;
            if (s.view == page::conversations && s.active)
            { app_.navigate(page::conversation); s.selecting = forward || !s.can_send(); }
            else if (s.view == page::conversation)
            {
                if ((forward || !wide) && s.can_send()) { s.selecting = false; }
                else if (wide) { app_.navigate(page::conversations); }
            }
            return true;
        }
        if (s.view == page::history)
        {
            int category = s.history_category;
            if (event == Event::ArrowRight) { category = (category + 1) % 4; }
            else if (event == Event::ArrowLeft) { category = (category + 3) % 4; }
            for (char key : {'1', '2', '3', '4'}) { if (event == Event::Character(key)) { category = key - '1'; } }
            if (category != s.history_category) { s.history_category = category; s.selected = 0; return true; }
            if (event == Event::PageDown) { app_.history(true); return true; }
            if (event == Event::Character('/')) { app_.command("search"); return true; }
            if (event == Event::Return)
            {
                // Back to the chat with that message selected.
                auto const entries = s.history_entries();
                if (s.selected < 0 || static_cast<std::size_t>(s.selected) >= entries.size()) { return true; }
                auto const id = entries[static_cast<std::size_t>(s.selected)]->id;
                app_.back();
                auto found = std::ranges::find(s.messages, id, &message::id);
                if (found != s.messages.end())
                {
                    s.message_selected = static_cast<int>(found - s.messages.begin());
                    s.at_latest = found + 1 == s.messages.end();
                    s.selecting = true;
                }
                return true;
            }
        }
        if (event == Event::Return && s.view == page::conversation) { open_message_menu(); return true; }
        if (event == Event::ArrowDown || event == Event::Character('j')) { move(1); return true; }
        if (event == Event::ArrowUp || event == Event::Character('k')) { move(-1); return true; }
        if (event == Event::Return)
        {
            auto options = actions(s);
            if (!options.empty() && s.selected >= 0 && static_cast<std::size_t>(s.selected) < options.size()) { app_.command(options[s.selected].command); }
            else if (s.view == page::pick_contacts) { app_.finish_pick(); }
            else if (s.view == page::members) { open_member_menu(); }
            else { app_.activate(); }
            return true;
        }
        if (event == Event::Character(' ') && s.view == page::pick_contacts) { app_.toggle_pick(); return true; }
        if (event == Event::PageUp)
        {
            if (s.view == page::conversation) { s.at_latest = false; app_.history(true); }
            else if (s.view == page::search) { app_.search(s.search_query, true); }
            return true;
        }
        if (event == Event::PageDown)
        {
            if (s.view == page::search) { app_.search(s.search_query, true); }
            else if (s.view == page::requests) { app_.requests(true); }
            else if (s.view == page::conversations) { app_.conversations(true); }
            return true;
        }
        if ((event == Event::Character('[') || event == Event::Character(']')) &&
            (s.view == page::conversation || s.view == page::search))
        {
            auto const* message = s.selected_message();
            if (!message) { return true; }
            auto width = app_.viewport_width - (s.view == page::conversation && state::layout(app_.viewport_width, app_.viewport_height) == layout_mode::wide ? 34 : 3);
            // Same height as drawn: a message within a group has no heading line.
            auto const index = static_cast<std::size_t>(s.message_selected);
            auto const heading = s.view != page::conversation || index >= s.messages.size() || starts_group(s.messages, index);
            auto item = message_item(s, *message, false, width, -1, heading);
            item->ComputeRequirement();
            int last = std::max(0, item->requirement().min_y - 1);
            int current = message_scroll_ < 0 ? last : message_scroll_;
            current = std::clamp(current + (event == Event::Character('[') ? -1 : 1), 0, last);
            message_scroll_ = current == last ? -1 : current;
            if (s.view == page::conversation)
            {
                s.at_latest = message_scroll_ < 0 && !s.messages.empty() &&
                    s.message_selected == static_cast<int>(s.messages.size()) - 1;
                app_.mark_visible_read();
            }
            return true;
        }
        if (event == Event::Character('G') && s.view == page::conversation)
        { message_scroll_ = -1; app_.command("latest"); return true; }
        if (s.view == page::members)
        {
            if (event == Event::Character('A')) { app_.command("admin"); return true; }
            if (event == Event::Character('O')) { app_.command("transfer"); return true; }
            if (event == Event::Character('D')) { app_.command("kick"); return true; }
        }
        if ((s.view == page::contacts || s.view == page::pick_contacts) && event == Event::Character('/'))
        { app_.command("filter"); return true; }
        if (s.view == page::friend_requests || s.view == page::friend_sent)
        {
            // y and n answer a received request; x withdraws a sent one.
            auto const [request, received] = s.friend_request_at(s.selected);
            if (request && received && event == Event::Character('y')) { app_.command("accept-friend"); return true; }
            if (request && received && event == Event::Character('n')) { app_.command("reject-friend"); return true; }
            if (request && !received && event == Event::Character('x')) { app_.command("cancel-friend"); return true; }
        }
        if (s.view == page::requests)
        {
            if (event == Event::Character('y')) { app_.command("accept"); return true; }
            if (event == Event::Character('n')) { app_.command("reject"); return true; }
        }
        if (event == Event::Character('N')) { app_.command("new"); return true; }
        if (event == Event::Character('h')) { app_.command("chats"); return true; }
        if (event == Event::Character('u')) { open_account_menu(); return true; }
        if (event == Event::Character('i'))
        {
            if (s.view != page::conversation) { return false; }
            app_.command("compose");
            compose_->TakeFocus();
            return true;
        }
        for (auto const& [key, command] : std::array<std::pair<char, const char*>, 12>{{
            {'r', "reply"}, {'e', "edit"}, {'d', "delete"}, {'a', "reaction"},
            {'y', "copy"}, {'s', "save"}, {'/', "search"}, {'m', "mute"}, {'p', "pin"},
            {'c', "contacts"}, {'g', "group"}, {'?', "help"}}})
        {
            if (event == Event::Character(key))
            {
                if (std::string_view("redays/").find(key) != std::string_view::npos &&
                    s.view != page::conversation && s.view != page::search) { return false; }
                if ((key == 'm' || key == 'p') && s.view != page::conversation && s.view != page::conversations) { return false; }
                app_.command(command);
                if (s.composing) { compose_->TakeFocus(); }
                return true;
            }
        }
        return false;
    }
private:
    Component input()
    {
        if (palette_open_) { return palette_input_; }
        if (app_.dialog) { sync_prompt(); return prompt_; }
        if (app_.command_mode) { return command_; }
        if (app_.data.composing) { return compose_; }
        if (!app_.data.self.id && app_.data.link == connection::signed_out)
        {
            if (settings_open_) { return address_->Focused() ? address_ : Component{}; }
            for (auto const& field : {username_, password_}) { if (field->Focused()) { return field; } }
        }
        return {};
    }
    // The menu is bound to its message or member, not to a list position.
    // The entries are fixed when the menu opens: a key never changes meaning (pin into unpin,
    // promote into demote) because the target changed meanwhile; such a change closes the menu.
    struct menu_state
    {
        enum class kind { message, member, account } kind;
        std::int64_t target = 0;
        std::string title;
        std::vector<menu_item> items;
        int selected = 0;
        // The connection state the items were offered for: offline the permission matrix still
        // offers local actions (copy, profile), so a menu closes when this changes, not merely
        // because it is offline.
        connection link = connection::signed_out;
    };
    std::optional<menu_state> menu_;

    // The actions the target allows now.
    std::vector<menu_item> current_items() const
    {
        auto const& s = app_.data;
        if (!menu_) { return {}; }
        if (menu_->kind == menu_state::kind::account) { return s.self.id == menu_->target ? s.account_actions() : std::vector<menu_item>{}; }
        if (menu_->kind == menu_state::kind::message)
        {
            auto found = std::ranges::find(s.messages, menu_->target, &message::id);
            return found == s.messages.end() ? std::vector<menu_item>{} : s.message_actions(*found);
        }
        auto found = std::ranges::find(s.members, menu_->target, &conversation_member::id);
        return found == s.members.end() ? std::vector<menu_item>{} : s.member_actions(*found);
    }
    void open_message_menu()
    {
        auto const* message = app_.data.selected_message();
        if (!message) { app_.data.status = "请先选择消息"; return; }
        auto preview = message->deleted ? std::string("消息已删除") : message->attachment && message->text.empty()
            ? message->attachment->filename : message->text;
        for (char& c : preview) { if (c == '\n') { c = ' '; } }
        menu_ = menu_state{menu_state::kind::message, message->id, message_heading(app_.data, *message) + " · " + preview,
                           app_.data.message_actions(*message), 0};
        menu_->link = app_.data.link;
        app_.menu_open = true;
    }
    void open_member_menu()
    {
        auto const& s = app_.data;
        if (s.selected < 0 || static_cast<std::size_t>(s.selected) >= s.members.size()) { return; }
        auto const& member = s.members[static_cast<std::size_t>(s.selected)];
        menu_ = menu_state{menu_state::kind::member, member.id, "成员 · " + member.username, s.member_actions(member), 0};
        menu_->link = app_.data.link;
        app_.menu_open = true;
    }
    void open_palette()
    {
        palette_open_ = true;
        app_.palette_open = true;
        palette_query_.clear();
        palette_seen_query_.clear();
        palette_selected_ = 0;
        choose_palette_entry();
        palette_conversation_ = app_.data.active;
        palette_link_ = app_.data.link;
        palette_account_ = app_.data.self.id;
        palette_input_->TakeFocus();
    }
    // The palette closes when the session or connection it was opened in ends, or the open
    // conversation goes away; its query changing (typed or pasted) resets the selection.
    void validate_palette()
    {
        if (!palette_open_) { return; }
        auto const& s = app_.data;
        if (s.self.id != palette_account_ || s.link != palette_link_ || s.active != palette_conversation_) { close_palette(); return; }
        if (palette_query_ != palette_seen_query_)
        {
            palette_seen_query_ = palette_query_; palette_selected_ = 0;
            choose_palette_entry();
            return;
        }
        // The highlighted command is the one Enter runs. When the list changes underneath (a role,
        // the page or the conversation's state), it stays on that command wherever it moved; if
        // it is gone, the palette closes rather than run whatever now sits in its row.
        if (!palette_choice_) { choose_palette_entry(); return; }
        auto const entries = palette_matches(s, palette_query_);
        auto const found = std::ranges::find(entries, *palette_choice_);
        auto const index = found - entries.begin();
        if (found == entries.end() || index >= 10)
        {
            close_palette();
            app_.data.status = "可用命令已变化，请重新选择";
            return;
        }
        palette_selected_ = static_cast<int>(index);
    }
    // Remembers which command is highlighted, not just its row.
    void choose_palette_entry()
    {
        auto const entries = palette_matches(app_.data, palette_query_);
        auto const count = static_cast<int>(std::min<std::size_t>(entries.size(), 10));
        if (!count) { palette_choice_.reset(); palette_selected_ = 0; return; }
        palette_selected_ = std::clamp(palette_selected_, 0, count - 1);
        palette_choice_ = entries[static_cast<std::size_t>(palette_selected_)];
    }
    // Closing returns to whatever was open before: the composer with its draft and cursor, a
    // menu or a dialog.
    // resume: open a conversation a request asked for while the palette was open. Running a
    // palette command is a newer choice instead, as opening the account menu is.
    void close_palette(bool resume = true)
    {
        palette_open_ = false;
        app_.palette_open = false;
        if (resume) { app_.open_pending(); }
        app_.mark_visible_read();
        // A dialog or command line beneath gets its keys back, with its text and cursor as they were.
        if (app_.dialog) { prompt_->TakeFocus(); }
        else if (app_.command_mode) { command_->TakeFocus(); }
    }
    bool palette_event(Event const& event)
    {
        if (event == Event::Escape || event == Event::CtrlK) { close_palette(); return true; }
        auto const entries = palette_matches(app_.data, palette_query_);
        auto const count = static_cast<int>(std::min<std::size_t>(entries.size(), 10));
        if (event == Event::ArrowDown) { palette_selected_ = count ? (palette_selected_ + 1) % count : 0; choose_palette_entry(); return true; }
        if (event == Event::ArrowUp) { palette_selected_ = count ? (palette_selected_ + count - 1) % count : 0; choose_palette_entry(); return true; }
        if (event == Event::Return)
        {
            if (!count) { return true; }
            auto const entry = entries[static_cast<std::size_t>(std::clamp(palette_selected_, 0, count - 1))];
            close_palette(false);
            app_.claim_destination();
            run_palette_entry(entry);
            return true;
        }
        palette_input_->TakeFocus();
        palette_input_->OnEvent(event);
        validate_palette();
        return true;
    }
    void run_palette_entry(palette_entry const& entry)
    {
        auto& s = app_.data;
        if (!entry.line_break)
        {
            // A command from the palette replaces a dialog or command line left open beneath
            // it, so a new dialog starts empty instead of inheriting the old input.
            if (app_.dialog) { app_.cancel_prompt(); }
            prompt_active_ = false;
            app_.command_mode = false;
            if (entry.command == "account") { open_account_menu(); return; }
            app_.command(entry.command);
            if (app_.exiting) { quit_(); }
            return;
        }
        // Insert a line break at the composer's cursor. The text is edited directly so overwrite
        // mode cannot replace a glyph with it.
        if (s.view != page::conversation || !s.can_send()) { return; }
        s.selecting = false;
        app_.sync_focus();
        auto const at = static_cast<std::size_t>(std::clamp(compose_cursor_, 0, static_cast<int>(s.draft.size())));
        s.draft.insert(at, "\n");
        compose_cursor_ = static_cast<int>(at) + 1;
        app_.compose_changed();
        compose_->TakeFocus();
    }
    // The account is a small menu over the current page, not a page of its own.
    void open_account_menu()
    {
        auto const& s = app_.data;
        if (!s.self.id) { return; }
        // The menu is the newest choice: a conversation an earlier request would open stays closed.
        app_.claim_destination();
        menu_ = menu_state{menu_state::kind::account, s.self.id, "账号 · " + s.self.username, s.account_actions(), 0};
        menu_->link = app_.data.link;
        app_.menu_open = true;
    }
    // The server settings over the login page, filled from the current address.
    void open_settings()
    {
        auto const parts = split_server_url(app_.server_url).value_or(server_address{"127.0.0.1:18080", false, "/ws"});
        settings_address_ = parts.host_port;
        settings_tls_ = parts.tls;
        settings_path_ = parts.path;
        settings_error_.clear();
        address_field_.cursor = static_cast<int>(settings_address_.size());
        settings_open_ = true;
        login_tab_ = 1;
        address_->TakeFocus();
    }
    void close_settings()
    {
        settings_open_ = false;
        login_tab_ = 0;
        server_settings_->TakeFocus();
    }
    void save_settings()
    {
        auto url = join_server_url(settings_address_, settings_tls_, settings_path_);
        if (!url) { settings_error_ = url.error(); settings_error_for_ = {settings_address_, settings_tls_}; return; }
        app_.server_url = std::move(*url);
        close_settings();
        app_.notify("服务器：" + app_.server_url);
    }
    bool settings_event(Event const& event)
    {
        if (event == Event::Escape) { close_settings(); return true; }
        // The two buttons share a row.
        if (event == Event::ArrowRight && settings_ok_->Focused()) { settings_cancel_->TakeFocus(); return true; }
        if (event == Event::ArrowLeft && settings_cancel_->Focused()) { settings_ok_->TakeFocus(); return true; }
        return settings_form_->OnEvent(event);
    }
    void close_menu()
    {
        menu_.reset();
        app_.menu_open = false;
        // The chat is visible again: decide reading anew at the end of this event.
        app_.mark_visible_read();
    }
    // A menu outlives neither its page, its connection nor its target.
    void validate_menu()
    {
        if (!menu_) { return; }
        auto const& s = app_.data;
        auto const page_ok = menu_->kind == menu_state::kind::account || (menu_->kind == menu_state::kind::message ? s.view == page::conversation : s.view == page::members);
        auto const link_ok = menu_->kind == menu_state::kind::account || s.link == menu_->link;
        if (!page_ok || !link_ok || app_.dialog || app_.command_mode) { close_menu(); return; }
        auto const now = current_items();
        if (now.empty()) { close_menu(); app_.data.status = "操作对象已不存在，菜单已关闭"; return; }
        auto same = [](menu_item const& a, menu_item const& b) {
            return a.command == b.command && a.label == b.label && a.disabled == b.disabled;
        };
        if (!std::ranges::equal(now, menu_->items, same)) { close_menu(); app_.data.status = "操作对象已变化，菜单已关闭"; }
    }
    bool menu_event(Event const& event)
    {
        auto const items = menu_->items;
        if (event == Event::Escape) { close_menu(); return true; }
        auto const count = static_cast<int>(items.size());
        menu_->selected = std::clamp(menu_->selected, 0, count - 1);
        if (event == Event::ArrowDown || event == Event::Character('j')) { menu_->selected = (menu_->selected + 1) % count; return true; }
        if (event == Event::ArrowUp || event == Event::Character('k')) { menu_->selected = (menu_->selected + count - 1) % count; return true; }
        std::optional<menu_item> chosen;
        if (event == Event::Return) { chosen = items[static_cast<std::size_t>(menu_->selected)]; }
        else if (event.is_character())
        {
            auto found = std::ranges::find_if(items, [&](menu_item const& item) { return item.key && event == Event::Character(item.key); });
            if (found != items.end()) { chosen = *found; }
        }
        if (!chosen) { return true; }
        if (!chosen->disabled.empty()) { app_.data.status = chosen->disabled; return true; }
        run_menu_item(*chosen);
        return true;
    }
    void run_menu_item(menu_item const& chosen)
    {
        auto& s = app_.data;
        auto const target = *menu_;
        close_menu();
        // Act on the bound target as it is now: it must still exist and still allow this action.
        std::vector<menu_item> now;
        if (target.kind == menu_state::kind::account)
        {
            if (s.self.id != target.target) { return; }
            now = s.account_actions();
        }
        else if (target.kind == menu_state::kind::message)
        {
            auto found = std::ranges::find(s.messages, target.target, &message::id);
            if (found == s.messages.end()) { s.status = "消息已不存在，操作已取消"; return; }
            s.message_selected = static_cast<int>(found - s.messages.begin());
            now = s.message_actions(*found);
        }
        else
        {
            auto found = std::ranges::find(s.members, target.target, &conversation_member::id);
            if (found == s.members.end()) { s.status = "该成员已不在群中，操作已取消"; return; }
            s.selected = static_cast<int>(found - s.members.begin());
            now = s.member_actions(*found);
        }
        // The same action, in the same direction, must still be allowed.
        auto still = std::ranges::find_if(now, [&](menu_item const& item) { return item.command == chosen.command && item.label == chosen.label; });
        if (still == now.end() || !still->disabled.empty()) { s.status = "该操作当前不可用，已取消"; return; }
        app_.command(chosen.command);
    }
    void update_history_rows(ftxui::Dimensions terminal)
    {
        auto const& s = app_.data;
        if (s.view != page::conversation || terminal.dimx <= 0 || terminal.dimy <= 0) { return; }
        // Read receipts need at least one history row on screen.
        auto const typing = !app_.typing_text().empty();
        auto const wide = state::layout(terminal.dimx, terminal.dimy) == layout_mode::wide;
        auto const width = terminal.dimx - (wide ? 34 : 3);
        // Without send permission the hint (counted in the chrome) replaces the composer.
        app_.history_rows = terminal.dimy - chat_chrome(s, width, typing) -
            (s.composing ? composer_lines(s, width, terminal.dimy, typing) : s.can_send() ? 1 : 0);
    }
    // Called at each redraw and before queued results (final = false), and when the paste ends.
    void flush_paste(bool final = false)
    {
        if (paste_buffer_.empty()) { return; }
        if (!paste_input_ || paste_input_ != input() || paste_conversation_ != app_.data.active)
        { paste_buffer_.clear(); return; }
        auto const target = edit_target_of(paste_input_);
        if (!target.text) { paste_buffer_.clear(); return; }
        auto& text = *target.text;
        // Each batch goes right after the bytes the previous one put in: the drawn cursor snaps to
        // whole glyphs, and a pasted piece that joins the text after it (a flag's first half
        // before another flag) would otherwise move it past that text and reorder the paste.
        // The first batch starts where the field's cursor is, moved to a glyph boundary as the
        // field itself would before an edit: a cursor kept from an earlier text (a reused prompt)
        // can point inside a multi-byte character until the field next draws.
        auto const at = paste_at_ ? std::min(*paste_at_, text.size())
                                  : glyph_boundary_at_or_after(text, static_cast<std::size_t>(std::max(0, *target.cursor)));
        std::string pasted;
        std::size_t replaced = 0;
        if (*target.insert) { pasted = std::exchange(paste_buffer_, {}); }
        else
        {
            // Overwrite replaces one glyph of the text after the cursor per pasted glyph, up to the
            // end of its line. Glyphs are counted in the pasted text and the original text each, so
            // a pasted flag replaces one glyph even where typing its two halves one by one would
            // join the first to the text after it. The last pasted glyph may continue in the next
            // batch (a combining mark, ZWJ or modifier), so until the paste ends it waits: the
            // result is then the same however the paste was split.
            auto glyphs = Utf8ToGlyphs(paste_buffer_);
            std::erase(glyphs, std::string{});
            std::string rest;
            if (!final && !glyphs.empty()) { rest = glyphs.back(); glyphs.pop_back(); }
            paste_buffer_ = std::move(rest);
            auto tail = Utf8ToGlyphs(std::string_view(text).substr(at));
            std::erase(tail, std::string{});
            std::size_t next = 0;
            for (auto const& glyph : glyphs)
            {
                pasted += glyph;
                if (next < tail.size() && tail[next] != "\n" && tail[next] != "\r\n" &&
                    !(tail[next] == "\r" && next + 1 < tail.size() && tail[next + 1] == "\n"))
                { replaced += tail[next++].size(); }
            }
        }
        // One edit for the whole batch keeps a large paste linear in its size.
        text.replace(at, replaced, pasted);
        paste_at_ = at + pasted.size();
        *target.cursor = static_cast<int>(*paste_at_);
        if (paste_input_ == compose_) { app_.compose_changed(); }
    }
    // The first glyph boundary at or after a byte offset, as an input field moves its cursor.
    static std::size_t glyph_boundary_at_or_after(std::string_view text, std::size_t at)
    {
        std::size_t boundary = 0;
        for (auto const& glyph : Utf8ToGlyphs(text))
        {
            if (boundary >= at) { return boundary; }
            boundary += glyph.size();
        }
        return text.size();
    }
    struct field_state { int cursor = 0; bool insert = true; };
    struct edit_target { std::string* text = nullptr; int* cursor = nullptr; bool* insert = nullptr; };
    edit_target edit_target_of(Component const& field)
    {
        if (field == compose_) { return {&app_.data.draft, &compose_cursor_, &compose_insert_}; }
        if (field == palette_input_) { return {&palette_query_, &palette_field_.cursor, &palette_field_.insert}; }
        if (field == prompt_) { return {&prompt_text_, &prompt_field_.cursor, &prompt_field_.insert}; }
        if (field == command_) { return {&app_.command_text, &command_field_.cursor, &command_field_.insert}; }
        if (field == address_) { return {&settings_address_, &address_field_.cursor, &address_field_.insert}; }
        if (field == username_) { return {&app_.username, &username_field_.cursor, &username_field_.insert}; }
        if (field == password_) { return {&app_.password, &password_field_.cursor, &password_field_.insert}; }
        return {};
    }
    field_state address_field_, username_field_, password_field_, command_field_, prompt_field_, palette_field_;
    // A conversation's draft comes back with the cursor after it, ready to go on typing or
    // to delete; moving between the messages and the composer of one chat keeps the cursor.
    void follow_draft()
    {
        auto const& s = app_.data;
        if (!s.composing || s.active == draft_conversation_) { return; }
        draft_conversation_ = s.active;
        compose_cursor_ = static_cast<int>(s.draft.size());
    }
    void sync_message_scroll()
    {
        auto const& s = app_.data;
        auto const* current = s.selected_message();
        auto const id = current ? current->id : 0;
        auto const index = static_cast<std::size_t>(std::max(0, s.message_selected));
        bool const heading = s.view != page::conversation || index >= s.messages.size() || starts_group(s.messages, index);
        if (scroll_conversation_ != s.active || scroll_message_ != id || scroll_page_ != s.view)
        {
            message_scroll_ = -1;
            scroll_conversation_ = s.active;
            scroll_message_ = id;
            scroll_page_ = s.view;
        }
        // An older page can join the message to the group before it (or split it off), which
        // removes (or adds) its heading line: keep the same text line in view.
        else if (heading != scroll_heading_ && message_scroll_ >= 0)
        { message_scroll_ = std::max(0, message_scroll_ + (heading ? 1 : -1)); }
        scroll_heading_ = heading;
    }
    void update_viewport(ftxui::Dimensions dimensions)
    {
        // Headless component tests may have no terminal dimensions. Production
        // resize events update visibility before any queued SDK result is used.
        if (dimensions.dimx > 0 && dimensions.dimy > 0)
        {
            app_.viewport_width = dimensions.dimx;
            app_.viewport_height = dimensions.dimy;
        }
    }
    void sync_prompt()
    {
        if (!prompt_active_)
        {
            prompt_text_ = app_.dialog->text;
            // A default value is usually replaced or extended: start after it.
            prompt_field_.cursor = static_cast<int>(prompt_text_.size());
            prompt_active_ = true;
            prompt_->TakeFocus();
        }
    }
    void move(int delta)
    {
        auto& s = app_.data;
        if (s.view == page::conversation || s.view == page::search) { message_scroll_ = -1; app_.select_message(delta); app_.mark_visible_read(); return; }
        s.focus_sent = false;  // the person chose a row; a pending ":friend-sent" no longer moves it
        auto count = selection_count(s, app_.viewport_width - 3);
        auto& index = s.view == page::conversations ? s.conversation_selected : s.selected;
        index = count ? std::clamp(index + delta, 0, static_cast<int>(count) - 1) : 0;
        if (s.view == page::conversations && delta > 0 && static_cast<std::size_t>(index + 1) >= count && s.next_conversations) { app_.conversations(true); }
    }
    app& app_;
    std::function<void()> quit_;
    Component address_, tls_, settings_ok_, settings_cancel_, settings_form_;
    std::string settings_address_;
    bool settings_tls_ = false;
    std::string settings_path_ = "/ws";
    std::string settings_error_;
    std::pair<std::string, bool> settings_error_for_;
    bool settings_open_ = false;
    int login_tab_ = 0;
    Component username_, password_, login_, register_, server_settings_, login_form_, compose_, command_, prompt_;
    bool pasting_ = false;
    Component paste_input_;
    std::string paste_buffer_;
    int compose_cursor_ = 0;
    std::int64_t draft_conversation_ = 0;
    Component palette_input_;
    std::string palette_query_;
    bool palette_open_ = false;
    int palette_selected_ = 0;
    std::string palette_seen_query_;
    std::optional<palette_entry> palette_choice_;
    std::int64_t palette_conversation_ = 0, palette_account_ = 0;
    connection palette_link_ = connection::signed_out;
    bool compose_insert_ = true;
    struct newline_mark { std::int64_t conversation; int cursor; std::string draft; };
    std::optional<newline_mark> newline_;
    std::int64_t paste_conversation_ = 0;
    // Byte offset in the draft where the next batch of an inserting paste goes.
    std::optional<std::size_t> paste_at_;
    std::string prompt_text_;
    bool prompt_active_ = false;
    int message_scroll_ = -1;
    std::int64_t scroll_conversation_ = 0, scroll_message_ = 0;
    bool scroll_heading_ = true;
    page scroll_page_ = page::conversations;
};
}

ftxui::Element render(state const& data, int width, int height, int message_scroll) { return render_impl(data, width, height, {}, {}, message_scroll); }
ftxui::Component make_ui(app& application, std::function<void()> quit)
{
    return std::make_shared<terminal_ui>(application, std::move(quit));
}
}
