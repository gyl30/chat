#include "app.hpp"
#include "file_io.hpp"

#include <chat/avatar.hpp>

#include <algorithm>
#include <string_view>

namespace chat::tui
{
namespace
{
bool image_signature(std::string_view bytes)
{
    constexpr std::string_view png("\x89PNG\r\n\x1a\n", 8);
    return bytes.starts_with(png) || (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xd8 &&
        static_cast<unsigned char>(bytes[2]) == 0xff);
}
}

void app::profile_command(std::string const& name, std::string argument)
{
    assert_ui();
    if (name == "profile" || name == "account")
    {
        data.profile = data.self;
        if (name == "profile")
        {
            if (data.view == page::contacts || data.view == page::users)
            {
                if (auto const* selected = data.selected_user()) { data.profile = *selected; }
            }
            else if (data.view == page::friend_requests || data.view == page::friend_sent)
            {
                auto const& requests = data.view == page::friend_requests ? data.friends.incoming : data.friends.outgoing;
                if (data.selected >= 0 && static_cast<std::size_t>(data.selected) < requests.size())
                { data.profile = requests[data.selected].user; }
            }
            else if (data.view == page::members)
            {
                if (data.selected >= 0 && static_cast<std::size_t>(data.selected) < data.members.size())
                {
                    auto const& member = data.members[data.selected];
                    data.profile = {member.id, member.username, member.avatar};
                }
            }
            else if (auto const* current = data.active_conversation(); current && current->kind == conversation_kind::direct)
            { data.profile = {current->user, current->username, current->avatar}; }
        }
        navigate(page::profile);
        return;
    }
    if (name == "user" || name == "contact")
    {
        auto const* selected = data.selected_user();
        if (!selected)
        { data.status = "请先选择用户"; return; }
        data.profile = *selected;
        navigate(page::profile);
        return;
    }
    if (name == "copy-user")
    {
        data.copy_text = data.view == page::profile ? data.profile.username : data.self.username;
        if (data.view == page::contacts || data.view == page::users)
        {
            if (auto const* selected = data.selected_user()) { data.copy_text = selected->username; }
        }
        else if (data.view == page::members)
        {
            if (data.selected >= 0 && static_cast<std::size_t>(data.selected) < data.members.size())
            { data.copy_text = data.members[data.selected].username; }
        }
        else if (data.view == page::conversation)
        {
            if (auto const* current = data.active_conversation(); current && current->kind == conversation_kind::direct)
            { data.copy_text = current->username; }
        }
        navigate(page::copy);
        return;
    }
    if (name == "avatar-clear" || (name == "avatar" && argument == "clear"))
    {
        if (!online()) { return; }
        auto const view = view_;
        client_->clear_avatar(callback([this, view](auto value) {
            if (view != view_) { return; }
            if (!value) { error(value.error()); return; }
            data.self.avatar = *value;
            if (data.profile.id == data.self.id) { data.profile.avatar = *value; }
            data.status = "头像已清除";
        }));
        return;
    }
    if (name == "avatar")
    {
        if (!online()) { return; }
        if (argument.starts_with("set ")) { argument.erase(0, 4); }
        auto const view = view_;
        auto action = [this, view](std::string path) {
            if (view != view_ || !online()) { return; }
            auto bytes = read_local_file(path, chat::max_avatar_size);
            if (!bytes) { data.status = bytes.error(); return; }
            if (!image_signature(*bytes)) { data.status = "头像必须是 PNG 或 JPEG 图片"; return; }
            data.status = "正在上传头像…";
            client_->set_avatar(std::move(*bytes), callback([this, view](auto value) {
                if (view != view_) { return; }
                if (!value) { error(value.error()); return; }
                data.self.avatar = *value;
                if (data.profile.id == data.self.id) { data.profile.avatar = *value; }
                data.status = "头像已更新";
            }));
        };
        if (argument.empty() || argument == "set") { ask("头像 PNG/JPEG 文件路径（最多 1 MiB）", {}, std::move(action)); }
        else { action(std::move(argument)); }
        return;
    }
    chat::user target = data.profile;
    if (data.view == page::conversation)
    {
        if (auto const* current = data.active_conversation(); current && current->kind == conversation_kind::direct)
        { target = {current->user, current->username, current->avatar}; }
        else { target = {}; }
    }
    if (data.view == page::contacts || data.view == page::users)
    {
        auto const* selected = data.selected_user();
        if (!selected)
        { data.status = "请先选择用户"; return; }
        target = *selected;
    }
    if (data.view == page::friend_requests || data.view == page::friend_sent)
    {
        auto const& requests = data.view == page::friend_requests ? data.friends.incoming : data.friends.outgoing;
        if (data.selected < 0 || static_cast<std::size_t>(data.selected) >= requests.size())
        { data.status = "请先选择好友申请"; return; }
        target = requests[data.selected].user;
    }
    if (name == "add" || name == "add-contact" || name == "accept-friend" || name == "reject-friend" || name == "cancel-friend")
    {
        if (!online()) { return; }
        if (target.id <= 0 || target.id == data.self.id) { data.status = "请先选择其他用户"; return; }
        auto const relationship = data.friendship(target.id);
        if ((name == "add" || name == "add-contact") && relationship != friendship_state::none)
        { data.status = relationship == friendship_state::accepted ? "已经是好友" : "请先处理当前好友申请"; return; }
        if ((name == "accept-friend" || name == "reject-friend") && relationship != friendship_state::incoming_pending)
        { data.status = "请选择收到的好友申请"; return; }
        if (name == "cancel-friend" && relationship != friendship_state::outgoing_pending)
        { data.status = "请选择已发送的好友申请"; return; }
        auto const view = view_;
        auto done = callback([this, view](auto value) {
            if (!value) { if (view == view_) { error(value.error()); } return; }
            contacts(); friend_requests(); conversations();
            if (view == view_) { data.status = "好友申请已更新"; }
        });
        if (name == "accept-friend" || name == "reject-friend")
        { client_->respond_friend_request(target.id, name == "accept-friend", std::move(done)); }
        else if (name == "cancel-friend") { client_->cancel_friend_request(target.id, std::move(done)); }
        else { client_->send_friend_request(target.id, std::move(done)); }
        return;
    }
    if (name == "remove-contact")
    {
        if (!online()) { return; }
        if (!data.is_contact(target.id)) { data.status = "请选择你的联系人"; return; }
        auto const view = view_;
        confirm("删除好友？双方聊天权限将关闭，历史记录保留", [this, view, target] {
            if (view != view_ || !online()) { return; }
            client_->remove_contact(target.id, callback([this, view](auto value) {
                if (!value) { if (view == view_) { error(value.error()); } return; }
                contacts(); friend_requests();
                conversations();
                if (view == view_) { data.status = "联系人已删除，历史记录保留"; }
            }));
        });
        return;
    }
    if (name == "message")
    {
        if (!online()) { return; }
        if (!data.is_contact(target.id))
        { data.status = data.friendship_hint(target.id); return; }
        auto const view = view_;
        client_->open_direct_conversation(target.id, callback([this, view](auto value) {
            if (view != view_) { return; }
            if (!value) { error(value.error()); return; }
            pending_open_ = value->conversation;
            conversations();
        }));
        return;
    }
    data.status = "未知个人资料命令";
}
}
