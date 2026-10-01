# 当前用户头像

本阶段从重新 fetch 后的 `origin/main = 6ff2ac65f50bc6c25f4636b7d51de7dd09c5ae03` 开始。头像属于用户，独立于消息附件，不保存发送时的头像历史。

## 数据与版本

SQL 015 给 `users` 增加 `avatar_revision`，默认 0；`user_avatars` 按 `user_id` 保存唯一当前头像的 media type、size、BYTEA 和更新时间。是否有头像由当前数据行是否存在决定，不重复保存 bool。

更新和清除都在一个事务内锁定用户行、递增 revision，再写入或删除数据。清除保留递增后的 revision，重新上传继续递增：例如 `1/存在 → 2/不存在 → 3/存在`。因此缓存键不会出现 ABA。账号、联系人、用户搜索、单聊对端、消息作者、群成员对象返回 `avatar_revision` 和 `has_avatar`，不返回图片内容。历史消息查询当前用户状态，不向 messages 表固化版本。

## RPC 与通知

所有请求使用现有 JSON-RPC / WebSocket，并要求认证。

| RPC | 参数 | 结果 |
|---|---|---|
| `begin_avatar_upload` | `size` | `upload` |
| `upload_avatar_chunk` | `upload, offset, data`（base64） | 下一 `offset` |
| `finish_avatar_upload` | `upload` | `avatar_revision, has_avatar` |
| `cancel_avatar_upload` | `upload` | `cancelled` |
| `get_avatar` | `user, revision, offset` | 当前 metadata、user、offset、size、media_type、data、has_more |
| `clear_avatar` | 空对象 | `avatar_revision, has_avatar` |

SDK 对外提供 `set_avatar/get_avatar/clear_avatar`，内部负责分块和错误时取消。上传只修改认证用户，不接受目标 user 参数。读取与现有用户搜索可见性一致，任何认证用户可读取现存用户头像。

上传文件最多 1 MiB，chunk 为现有 32 KiB；base64 chunk 约 44 KiB，满足服务器 WebSocket 入站 64 KiB 上限。上传状态独立存于 session，不覆盖消息附件上传，不持久化半成品。随机正 upload ID 使用现有 OpenSSL；偏移、实际大小、base64、完整内容和 magic 都独立检查。中断连接销毁上传，重连不能继续旧 upload ID。

服务端只接受 PNG/JPEG，完整解码并限制最多 16 × 1024 × 1024 像素。PNG 检查 chunk 边界和最终 IEND；JPEG 检查完整解码、警告和未消费尾随内容。使用已安装 libpng/libjpeg，不引入转码、缩放或通用媒体框架。libjpeg 的 setjmp 错误处理位于普通函数，避免进入协程挂起上下文（[C++ 标准](https://eel.is/c++draft/csetjmp.syn)）。

get 请求 revision 过期时返回当前 metadata、空内容、size/offset 0、has_more false；SDK 返回权威新状态，Qt 据此重新获取。每块读取在同一 SQL 快照中取得版本和数据，更新不会把新版本配上旧内容。

提交后发 `avatar {user, avatar_revision, has_avatar}`。收件人包括自己、双向联系人、已有单聊对端（包括尚无消息的单聊）、当前共享群的成员；SQL UNION 去重，不全局广播。头像是公开用户信息，群退出或移除不使该用户头像变为私有。没有头像事件回放，离线变化由登录、联系人、会话和已加载历史等权威快照恢复。

## Qt 缓存和展示

一个账号窗口共用一份最小内存缓存；每用户只保存当前 revision、present、最多 128 × 128 的已解码绘制图片以及是否尝试下载。同一版本在联系人、会话、历史、搜索和成员列表重复出现时只下载一次。0 或更高 revision 的无头像状态直接 fallback。成功缓存重连保留，失败或中断的当前头像重连可重试。

版本变化清除该用户旧图，旧 metadata 和旧异步结果不能覆盖新 revision。bridge 的头像 generation 在新连接、close 和实际断线时推进，丢弃旧连接的上传/下载结果；logout 清理缓存。缓存下载失败继续首字符和原有背景色，不在 paint 中请求网络或解码图片。

账号、联系人、单聊会话、消息作者、资料窗口、单聊 header 和群成员共用圆形绘制，保持尺寸和布局，使用平滑缩放。自己的资料窗口提供“更换头像”“移除头像”，上传前检查文件和真实解码，显示轻量进度/错误，不弹成功提示；关闭窗口不会让回调访问已销毁控件。

只保存一个当前头像。没有裁剪器、动画、多尺寸服务版本、自定义群头像、CDN、对象存储或头像历史。

## 回归范围

覆盖旧用户迁移默认值和级联清理；未认证、参数、超限、格式、偏移、取消、断线旧 upload、附件状态隔离；替换/清除/重新上传的 revision、原子并发读取、相关通知去重及非全局路由；SDK 多 chunk、异常响应和通知、析构 pending 回调；Qt fallback、圆形绘制、同版本去重、单用户失效、失败重试、过期结果；三个真实窗口完成设置、联系人/单聊/群展示、更换、清除、重连和重新登录。

完整构建和 sanitizer 的实际结果见 [开发状态](development-status.md)。
