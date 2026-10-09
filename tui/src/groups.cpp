#include "app.hpp"

#include <chat/invite.hpp>
#include <chat/text.hpp>
#include <algorithm>
#include <string_view>

namespace chat::tui
{
void app::members()
{
    assert_ui();
    auto const* current = data.active_conversation();
    if (!current || current->kind != conversation_kind::group)
    { data.members.clear(); return; }
    if (!online()) { return; }
    auto const id = data.active;
    auto const view = view_;
    auto const request = ++members_request_;
    client_->get_members(id, callback([this, id, view, request](auto value) {
        if (data.active != id || view_ != view || request != members_request_) { return; }
        if (!value) { error(value.error()); return; }
        auto selected_id = data.view == page::members && data.selected >= 0 &&
            static_cast<std::size_t>(data.selected) < data.members.size() ? data.members[data.selected].id : 0;
        data.members = std::move(*value);
        if (data.self_role() == member_role::member)
        {
            if (data.view == page::requests)
            {
                data.requests.clear();
                data.next_requests.reset();
                requests_busy_ = requests_again_ = false;
                navigate(page::group);
                notify("群角色已更新，申请管理已关闭");
            }
            else if (data.view == page::pick_contacts && (pick_action_ == "invite" || pick_action_ == "inviting"))
            {
                data.picked_contacts.clear();
                pick_action_.clear();
                navigate(page::group);
                notify("群角色已更新，联系人邀请已关闭");
            }
        }
        if (data.view == page::members)
        {
            auto found = std::ranges::find(data.members, selected_id, &conversation_member::id);
            data.selected = found == data.members.end() ? 0 : static_cast<int>(found - data.members.begin());
        }
    }));
}

void app::requests(bool more)
{
    assert_ui();
    auto const* current = data.active_conversation();
    if (!current || current->kind != conversation_kind::group || data.view != page::requests ||
        data.self_role() == member_role::member || !online()) { return; }
    if (requests_busy_) { if (!more) { requests_again_ = true; } return; }
    if (more && !data.next_requests) { return; }
    auto const id = data.active;
    auto const view = view_;
    auto cursor = more ? data.next_requests : std::nullopt;
    requests_busy_ = true;
    client_->get_group_join_requests(id, cursor, callback([this, id, view, more, cursor](auto value) {
        if (data.active != id || view_ != view || data.view != page::requests) { return; }
        requests_busy_ = false;
        if (requests_again_) { requests_again_ = false; requests(); return; }
        if (!value) { error(value.error()); return; }
        if (more && data.next_requests != cursor) { return; }
        auto selected_id = data.selected >= 0 && static_cast<std::size_t>(data.selected) < data.requests.size()
            ? data.requests[data.selected].applicant.id : 0;
        if (!more) { data.requests.clear(); }
        for (auto& item : value->requests)
        {
            auto found = std::ranges::find_if(data.requests, [&](auto const& old) { return old.applicant.id == item.applicant.id; });
            if (found == data.requests.end()) { data.requests.push_back(std::move(item)); }
            else { *found = std::move(item); }
        }
        data.next_requests = value->next;
        auto found = std::ranges::find_if(data.requests, [&](auto const& item) { return item.applicant.id == selected_id; });
        data.selected = found == data.requests.end() ? 0 : static_cast<int>(found - data.requests.begin());
    }));
}

void app::group_done(bool left)
{
    notify(left ? "已退出群聊" : "群聊已更新");
    if (left)
    {
        stop_composing();
        data.select_conversation(0);
        navigate(page::conversations);
    }
    if (left) { conversations(); }
    else { changed(data.active, false); }
}

void app::toggle_pick()
{
    assert_ui();
    auto candidates = data.pick_candidates();
    if (data.view != page::pick_contacts || data.selected < 0 ||
        static_cast<std::size_t>(data.selected) >= candidates.size()) { return; }
    auto const id = candidates[data.selected]->id;
    if (id == data.self.id) { return; }
    if (pick_action_ == "invite" && std::ranges::find(data.members, id, &conversation_member::id) != data.members.end())
    { data.status = "该联系人已在群聊中"; return; }
    auto found = std::ranges::find(data.picked_contacts, id);
    if (found == data.picked_contacts.end()) { data.picked_contacts.push_back(id); }
    else { data.picked_contacts.erase(found); }
    data.status.clear();
}

void app::finish_pick()
{
    assert_ui();
    if (data.view != page::pick_contacts || !online()) { return; }
    auto picked = data.picked_contacts;
    std::erase_if(picked, [this](auto id) { return !data.is_contact(id) || id == data.self.id; });
    auto const view = view_;
    if (pick_action_ == "create")
    {
        if (picked.empty()) { data.status = "请至少选择一位联系人创建群聊"; return; }
        ask("创建群聊 · 下一步：群名称", group_title_, [this, view, picked = std::move(picked)](std::string title) mutable {
            if (!valid_group_title(title)) { data.status = "群名称须为 1–256 UTF-8 字节且不能只有空白或含 NUL"; return; }
            group_title_ = title;
            confirm("创建群聊「" + title + "」 · " + std::to_string(picked.size() + 1) + " 位成员？",
                [this, view, title = std::move(title), picked = std::move(picked)]() mutable {
                    if (view_ != view || data.view != page::pick_contacts || !online()) { return; }
                    std::erase_if(picked, [this](auto id) { return !data.is_contact(id); });
                    if (picked.empty()) { data.status = "所选好友关系已变更，请重新选择"; return; }
                    pick_action_ = "creating";
                    client_->create_group(std::move(title), std::move(picked), callback([this, view, destination = destination_](auto value) {
                        if (view_ != view || data.view != page::pick_contacts) { return; }
                        if (!value) { pick_action_ = "create"; error(value.error()); return; }
                        // The group exists either way; only opening it waits on no newer choice.
                        if (destination != destination_) { pick_action_.clear(); data.picked_contacts.clear(); notify("群聊已创建"); conversations(); return; }
                        pick_action_.clear(); data.picked_contacts.clear(); data.pick_query.clear();
                        navigate(page::conversations); pending_open_ = *value;
                        notify("群聊已创建"); conversations();
                    }));
                });
        });
    }
    else if (pick_action_ == "invite")
    {
        if (data.self_role() == member_role::member) { data.status = "仅群主和管理员可邀请联系人"; return; }
        std::erase_if(picked, [this](auto id) { return std::ranges::find(data.members, id, &conversation_member::id) != data.members.end(); });
        if (picked.empty()) { data.status = "请至少选择一位尚未入群的联系人"; return; }
        auto const id = data.active;
        pick_action_ = "inviting";
        client_->invite_group_members(id, std::move(picked), callback([this, id, view](auto value) {
            if (data.active != id || view_ != view || data.view != page::pick_contacts) { return; }
            if (!value) { pick_action_ = "invite"; error(value.error()); return; }
            pick_action_.clear();
            data.picked_contacts.clear();
            navigate(page::members);
            group_done();
        }));
    }
}

void app::group_command(std::string const& name, std::string argument)
{
    assert_ui();
    if (!online()) { return; }
    if (name == "create-group")
    {
        group_title_ = std::move(argument);
        pick_action_ = "create";
        data.picked_contacts.clear(); data.pick_query.clear();
        navigate(page::pick_contacts);
        contacts();
        return;
    }
    if (name == "join")
    {
        if (argument.empty())
        {
            ask("加入群聊 · 输入邀请码，例如 K7QM-3XWP", {}, [this](std::string token) { group_command("join", std::move(token)); });
            return;
        }
        // Any case is accepted, with spaces or a dash between the two groups.
        auto token = normalize_invite_token(argument);
        if (!token) { data.status = "邀请码无效，请检查后重新输入"; return; }
        auto const view = view_;
        client_->join_group(std::move(*token), callback([this, view, destination = destination_](auto value) {
            if (view_ != view) { return; }
            if (!value) { error(value.error()); return; }
            if (value->state == group_join_state::pending)
            {
                // Approval opens the group later only if nothing newer was chosen meanwhile.
                if (destination == destination_) { join_destinations_[value->conversation] = destination_; }
                else { join_destinations_.erase(value->conversation); }
                notify("申请已提交，等待管理员审批");
                return;
            }
            if (destination != destination_) { notify("已加入群聊"); conversations(); return; }
            navigate(page::conversations);
            pending_open_ = value->conversation;
            notify("已加入群聊");
            conversations();
        }));
        return;
    }
    auto const* current = data.active_conversation();
    if (!current || current->kind != conversation_kind::group)
    { data.status = "请先打开群聊"; return; }
    auto const id = data.active;
    auto const role = data.self_role();
    if (name == "group" || name == "members")
    {
        navigate(name == "group" ? page::group : page::members);
        members();
        return;
    }
    if (name == "show-announcement")
    {
        data.copy_text = current->announcement.empty() ? "暂无群公告" : current->announcement;
        navigate(page::copy);
        return;
    }
    if (name == "pinned")
    {
        if (!current->pinned_message) { data.status = "暂无群置顶消息"; return; }
        auto const quote = *current->pinned_message;
        auto found = std::ranges::find(data.messages, quote.id, &chat::message::id);
        if (found != data.messages.end())
        {
            auto index = static_cast<int>(found - data.messages.begin());
            navigate(page::conversation);
            data.message_selected = index;
            data.at_latest = index == static_cast<int>(data.messages.size()) - 1;
        }
        else
        {
            data.copy_text = quote.username + ": " + (quote.deleted ? "消息已删除" : quote.text);
            navigate(page::copy);
        }
        return;
    }
    auto const view = view_;
    auto done = [this, id, view](bool left = false) {
        return callback([this, id, view, left](auto value) {
            if (data.active != id || view_ != view) { return; }
            if (!value) { error(value.error()); return; }
            group_done(left);
        });
    };
    if (name == "leave")
    {
        if (role == member_role::owner) { data.status = "群主须先转让群主身份再退出"; return; }
        confirm("退出当前群聊？", [this, id, view, done] {
            if (data.active != id || view_ != view || !online()) { return; }
            client_->leave_group(id, done(true));
        });
        return;
    }
    if (role == member_role::member) { data.status = "仅群主和管理员可执行该操作"; return; }
    if (name == "invite")
    {
        pick_action_ = "invite";
        data.picked_contacts.clear(); data.pick_query.clear();
        navigate(page::pick_contacts);
        contacts();
        members();
    }
    else if (name == "rename")
    {
        if (argument.empty())
        {
            ask("修改群名称", current->username, [this, id, view](std::string title) {
                if (data.active == id && view_ == view) { group_command("rename", std::move(title)); }
            });
            return;
        }
        if (!valid_group_title(argument)) { data.status = "群名称无效"; return; }
        client_->rename_group(id, std::move(argument), done());
    }
    else if (name == "announcement")
    {
        if (argument.empty())
        {
            ask("编辑公告 · 留空清除", current->announcement, [this, id, view, done](std::string text) {
                if (data.active == id && view_ == view && online()) { client_->set_group_announcement(id, std::move(text), done()); }
            });
        }
        else { client_->set_group_announcement(id, std::move(argument), done()); }
    }
    else if (name == "admin" || name == "transfer" || name == "kick")
    {
        if (data.view != page::members || data.selected < 0 || static_cast<std::size_t>(data.selected) >= data.members.size())
        { data.status = "请在群成员页选择成员"; return; }
        auto member = data.members[data.selected];
        if (member.id == data.self.id) { data.status = "不能对自己执行此操作"; return; }
        if (name == "admin")
        {
            if (role != member_role::owner) { data.status = "仅群主可设置管理员"; return; }
            if (member.role == member_role::owner) { data.status = "不能修改群主角色"; return; }
            auto enable = member.role != member_role::admin;
            if (enable && std::ranges::count(data.members, member_role::admin, &conversation_member::role) >= 3)
            { data.status = "最多可设置 3 位管理员"; return; }
            client_->set_group_admin(id, member.id, enable, done());
        }
        else if (name == "transfer")
        {
            if (role != member_role::owner || member.role != member_role::admin)
            { data.status = "群主只能将群主身份转让给当前管理员"; return; }
            confirm("将群主身份转让给 " + member.username + "？", [this, id, view, user = member.id, done] {
                if (data.active == id && view_ == view && online()) { client_->transfer_group_owner(id, user, done()); }
            });
        }
        else
        {
            if (member.role == member_role::owner || (role == member_role::admin && member.role == member_role::admin))
            { data.status = "没有移除此成员的权限"; return; }
            confirm("移除群成员 " + member.username + "？", [this, id, view, user = member.id, done] {
                if (data.active == id && view_ == view && online()) { client_->remove_group_member(id, user, done()); }
            });
        }
    }
    else if (name == "pin-message")
    {
        auto const* message = data.selected_message();
        if (!message || message->deleted) { data.status = "请选择未删除的消息"; return; }
        client_->pin_group_message(id, message->id, done());
    }
    else if (name == "unpin-message") { client_->unpin_group_message(id, done()); }
    else if (name == "link" || name == "link-create" || name == "link-revoke")
    {
        auto show = callback([this, id, view](auto value) {
            if (data.active != id || view_ != view) { return; }
            if (!value) { error(value.error()); return; }
            if (!*value) { data.status = "当前没有邀请码"; return; }
            data.copy_text = format_invite_token(**value);
            navigate(page::copy);
        });
        if (name == "link") { client_->get_group_invite(id, std::move(show)); }
        else if (name == "link-create") { client_->create_group_invite(id, std::move(show)); }
        else
        {
            confirm("撤销群邀请码？", [this, id, view] {
                if (data.active != id || view_ != view || !online()) { return; }
                client_->revoke_group_invite(id, callback([this, id, view](auto value) {
                    if (data.active != id || view_ != view) { return; }
                    if (!value) { error(value.error()); return; }
                    data.copy_text.clear();
                    notify("邀请码已撤销");
                }));
            });
        }
    }
    else if (name == "approval") { client_->set_group_join_approval(id, !current->join_approval, done()); }
    else if (name == "requests")
    {
        navigate(page::requests);
        data.requests.clear();
        data.next_requests.reset();
        requests();
    }
    else if (name == "accept" || name == "reject")
    {
        if (data.view != page::requests || data.selected < 0 || static_cast<std::size_t>(data.selected) >= data.requests.size())
        { data.status = "请先选择入群申请"; return; }
        auto const user = data.requests[data.selected].applicant.id;
        client_->respond_group_join_request(id, user, name == "accept", callback([this, id, view](auto value) {
            if (data.active != id || view_ != view || data.view != page::requests) { return; }
            if (!value) { error(value.error()); return; }
            notify("入群申请已处理");
            requests();
            changed(id, false);
        }));
    }
    else { data.status = "未知群聊命令"; }
}
}
