#ifndef CHAT_CLIENT_INCLUDE_CHAT_ERROR_TEXT_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_ERROR_TEXT_HPP

#include <string>
#include <string_view>
#include <unordered_map>

#include "chat/error.hpp"

namespace chat
{

// The text shown to people for an error. Server messages are protocol strings in English;
// both clients show the same Chinese wording for them.
inline std::string error_text(error const& value)
{
    static std::unordered_map<std::string_view, std::string_view> const messages{
        {"Server error", "服务器出错，请稍后重试"},
        {"Authentication required", "登录状态已失效，请重新登录"},
        {"Already authenticated", "当前连接已登录"},
        {"User already online", "该账号已在其他地方登录"},
        {"Username already exists", "用户名已被占用"},
        {"User not found", "用户不存在"},
        {"User unavailable", "用户不存在"},
        {"Conversation unavailable", "会话不可用"},
        {"Message unavailable", "消息不存在或已被删除"},
        {"Communication not allowed", "你们还不是好友，无法发送"},
        {"Contact required", "需要先成为好友"},
        {"Already friends", "你们已经是好友"},
        {"Incoming friend request not found", "好友申请已失效"},
        {"Conflicting friend requests", "好友申请状态已变化，请刷新后重试"},
        {"Group unavailable", "群聊不可用"},
        {"Group permission denied", "没有执行此操作的群权限"},
        {"Member unavailable", "该成员已不在群中"},
        {"Invite unavailable", "邀请码无效或已失效"},
        {"Too many invite attempts", "邀请码尝试次数过多，请 10 分钟后再试"},
        {"Invitees must be your contacts", "只能邀请自己的好友"},
        {"At most three administrators are allowed", "最多只能设置三名管理员"},
        {"The owner cannot be an administrator", "群主不能设为管理员"},
        {"The owner cannot leave without transferring ownership", "群主需先转让群主身份才能退出"},
        {"Avatar unavailable", "头像不可用"},
        {"Avatar upload in progress", "头像正在上传，请稍候"},
        {"Avatar upload unavailable", "头像上传失败，请重试"},
        {"Avatar upload unavailable or incomplete", "头像上传失败，请重试"},
        {"Attachment unavailable", "文件不存在或已被删除"},
        {"Attachment upload in progress", "文件正在上传，请稍候"},
        {"Attachment upload unavailable", "文件上传失败，请重试"},
        {"Attachment upload incomplete", "文件上传失败，请重试"},
        {"Connection closed", "连接已断开"},
        {"Connection already open", "连接已建立"},
        {"Parse error", "请求无效，请升级客户端"},
        {"Invalid Request", "请求无效，请升级客户端"},
        {"Method not found", "服务器不支持此操作，请升级客户端"},
        {"Invalid params", "请求参数无效"},
    };
    if (auto const found = messages.find(value.message); found != messages.end()) { return std::string(found->second); }
    if (value.kind == error_kind::transport) { return "无法连接服务器（" + value.message + "）"; }
    if (value.kind == error_kind::protocol) { return "服务器响应异常，请稍后重试"; }
    return value.message;
}

}    // namespace chat

#endif
