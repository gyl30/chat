# Telegram 群聊设计调研与本项目裁剪方案

调研日期：2026-10-01。仓库基线：`f73b8f48590999b6ffa4444889d7b37c780d2ed8`。本文记录可验证事实和本项目采用的设计；实现结果见文末阶段记录。

以 Telegram 官方协议、TDLib 和 Telegram Desktop 为主要参考。Telegram 服务端没有公开源码，不能从客户端类型推断它的数据库表、事务锁或部署架构。[官方说明](https://telegram.org/faq#q-can-i-get-telegram-39s-server-side-code)

## 角色和权限

TDLib 将群主与管理员定义为不同成员状态；管理员拥有权限集合。普通群管理员具有适用的完整权限，超级群才允许逐项授权。因此固定角色是有依据的最小选择，当前不需要构建权限配置系统。[官方权限协议](https://core.telegram.org/api/rights)；[TDLib 固定源码：成员状态](https://github.com/tdlib/td/blob/42e6a5259551178d1dab54a22ad96d14bd906e20/td/generate/scheme/td_api.tl#L2488-L2500)

Telegram 超级群可以向管理员授予邀请、修改资料、任命权限更少的管理员等能力。本项目采用用户已确定的约束：

| 操作 | 群主 | 管理员 | 普通成员 |
|---|---|---|---|
| 任免管理员 | 可以，最多三名，群主不计入 | 不可以 | 不可以 |
| 邀请自己的联系人 | 可以 | 可以 | 不可以 |
| 修改群名 | 可以 | 可以 | 不可以 |
| 自行退出 | 先转让群主，再退出 | 可以 | 可以 |
| 转让群主 | 仅转给当前管理员 | 不可以 | 不可以 |
| 移除成员 | 可以移除管理员/成员，不能移除自己 | 仅普通成员，不能移除自己 | 不可以 |
| 收发消息、读取可见历史 | 可以 | 可以 | 可以 |

三名上限、仅群主任免和联系人邀请是本项目规则，不能标为 Telegram 的统一限制。权限仍由服务端当前成员关系判断，Qt 控件禁用仅用于交互。[Telegram 管理员权限字段](https://core.telegram.org/constructor/chatAdminRights)

保持现有 `conversations` 和 `conversation_members`。SQL 013 已增加 `owner_id` 和 `is_admin`；对外返回明确的 `owner/admin/member`。不增加单独群表，不通过用户 ID 或用户名模拟群，不将群主同时计为管理员。

## 群主退出的时间差异

不能简单说“Telegram 群主永远不能退出”。2026-02-09 官方更新支持退出时选择新群主，也描述了退出后一周自动转交管理员。旧 RPC 文档仍列有创建者退出错误；实际能力应结合群类型和新版本区分。[2026 年更新](https://telegram.org/blog/crafting-android-design-and-more#leaving-groups-to-a-new-admin)；[RPC 错误说明](https://core.telegram.org/method/channels.leaveChannel)

SQL 013/014 所在阶段未做转让，因此当时禁止群主直接退出。2026-10-01 后续 Goal 明确采用手动转让：仅当前群主转给当前管理员，目标成为 owner 且 `is_admin=false`，原 owner 成为 admin，管理员总数不增加；原 owner 随后使用既有 `leave_group` 退出。不引入自动选举、计时任务或 ownership history。

本轮重新核查官方文档：Telegram 有专门的所有权转让 RPC，并且要求 2FA；移除普通群成员与超级群 ban/restrict 也有不同接口。本项目只借鉴“转让职责”和“移除成员”的区分，继续使用上述固定角色和一次移除语义，不照搬密码时效、细分权限、封禁或历史撤回模式。[转让 RPC](https://core.telegram.org/method/channels.editCreator)；[普通群移除 RPC](https://core.telegram.org/method/messages.deleteChatUser)；[超级群成员权限 RPC](https://core.telegram.org/method/channels.editBanned)

## 历史可见性、未读和已读回执

Telegram 提供新成员历史可见性的独立设置，不等于它的未读或阅读位置。[历史可见性 RPC](https://core.telegram.org/method/channels.togglePreHistoryHidden)；[TDLib 历史设置接口](https://github.com/tdlib/td/blob/42e6a5259551178d1dab54a22ad96d14bd906e20/td/generate/scheme/td_api.tl#L15191-L15192)

TDLib 的会话对象分别提供未读数量、最后已读的收件消息和发件消息；查看消息的成员列表又有单独接口。[会话对象](https://core.telegram.org/tdlib/docs/classtd_1_1td__api_1_1chat.html)；[已读成员查询](https://core.telegram.org/method/messages.getMessageReadParticipants)

本项目已选择：当前成员可看完整历史，退出后不能继续通过服务器访问群；重新加入仍可看完整历史，加入之前的消息不计未读。这是选定的本地产品语义，不宣称完全复刻 Telegram 所有群类型或重新加入行为。

必须区分：

- 历史访问权限：哪些消息能拉取。
- 加入水位：未读计算从哪里开始。
- 阅读水位：用户实际执行 `mark_read` 到哪条消息。

先前“把新成员的 read position 直接设为最新消息”可以消除历史未读，但现有 Qt 也使用它判断别人是否已读，因而会把入群当作实际阅读。若保留真实已读含义，应在成员关系中存独立加入水位，并让未读从 `max(加入水位, 阅读水位)` 之后计算；加入水位不限制历史查询，也不产生阅读回执。该字段是满足现有交互语义所需的状态，不是为未知功能预留。

加入水位必须与消息写入串行化。现有发送路径锁定会话，邀请事务也锁定会话，并在取得锁后用新的查询读取最新消息 ID，避免等待锁期间新增消息被遗漏。

Telegram 官方说明群消息至少被一位其他成员看到就显示双勾；小群还能查询谁读过。调研基线中的 `qt/src/message_model.cpp` 当时要求所有其他成员都达到该消息水位；SQL 014 所在阶段已改为用户确认的“至少一位当前其他成员真实读过”语义，成员自己的阅读水位保持独立。[群已读产品说明](https://telegram.org/blog/chat-themes-interactive-emoji-read-receipts#read-receipts-in-small-groups)

## realtime 和断线恢复

Telegram 用 `seq/pts/qts` 区分已处理更新与缺口，并通过差量接口恢复。消息 ID 和更新序号用途不同，不能把消息 ID 的空洞当成漏事件。[更新协议](https://core.telegram.org/api/updates)

本项目可以先实现同样的恢复目标，而复用现有简单机制：数据库是权威状态；群变更提交后发送带会话 ID 的通知；客户端重新获取会话、成员、角色、阅读位置；重连重新获取这些快照，再按真实消息 ID 补消息。退出者收到一次会话变更并关闭活动群，后续消息不再路由给它。

这能恢复当前需要的成员、权限和消息状态。本阶段已核实 `chat_widget::recovery_cursor()` 从已加载消息的第一条 ID 之前恢复，bridge 自动向前分页，因此已加载历史的编辑与删除也会重新获取；不能仅凭 `after=最新消息ID` 宣称补齐所有变更。当前没有必须回放每一次成员事件的需求，因此不新增事件表、通用事件总线或完整 Telegram 更新序号体系。

## Qt 源码参考

本次固定读取 Telegram Desktop 提交 `8c0d1e5691a28637ee99084662f49c4ca0254e24`，TDLib 提交 `42e6a5259551178d1dab54a22ad96d14bd906e20`。后续比较以固定源码链接定位，重新实施仍以本项目实际 HEAD 为准。

- [EditAdminBox](https://github.com/telegramdesktop/tdesktop/blob/8c0d1e5691a28637ee99084662f49c4ca0254e24/Telegram/SourceFiles/boxes/peers/edit_participant_box.cpp#L395)：管理员编辑独立于普通成员列表，区分自己能否编辑目标权限。
- [历史可见性窗口](https://github.com/telegramdesktop/tdesktop/blob/8c0d1e5691a28637ee99084662f49c4ca0254e24/Telegram/SourceFiles/boxes/peers/edit_peer_history_visibility_box.cpp#L56)：独立设置，并通过保存回调提交选择。
- [成员数据处理](https://github.com/telegramdesktop/tdesktop/blob/8c0d1e5691a28637ee99084662f49c4ca0254e24/Telegram/SourceFiles/api/api_chat_participants.cpp#L284)：明确解析群主和管理员，不从显示文本推断角色。

本项目群成员窗口展示角色、人数和操作结果即可；不照搬整个窗口框架、匿名管理员或细分权限菜单。消息编辑仍只允许作者，删除仍保持现有全员占位语义。Telegram 的消息删除不留占位，属于产品差异，不在本次“重点参考”中自动改写。[删除说明](https://telegram.org/faq#q-can-i-delete-my-messages)

## 其他项目作为旁证

QQ 官方确认只有群主可设置管理员；腾讯云 IM 将群主、管理员、成员和成员阅读序号分别公开。这能佐证固定角色与成员水位的拆分，但腾讯云 SDK 不是 QQ 或微信内部服务端源码。[QQ 官方说明](https://kf.qq.com/faq/120511jiYzIJ140828eimiAR.html)；[腾讯云群组数据结构](https://cloud.tencent.com/document/product/269/1502)

OpenIM 的群主退出会被拒绝，群成员持有角色，并另有消息序号模型；Tinode 则通过订阅权限和 `ReadSeqId/RecvSeqId` 分开管理权限、阅读与接收。只借鉴职责划分，不采用它们的基础设施或假定其加入语义相同。[OpenIM 退出逻辑](https://github.com/OpenIMSDK/Open-IM-Server/blob/175a7bb0673eca18e9d1b10bff4f728da6b1b513/internal/rpc/group/group.go#L1009-L1037)；[Tinode 订阅模型](https://github.com/tinode/chat/blob/eb90ef83acae6542f52719377ee5a76d267a5618/server/store/types/types.go#L893-L915)

## 实施边界

先完成群主与三名管理员任免的 server/client/Qt/tests 闭环，再独立完成邀请、改名、退出、加入水位与快照恢复。每阶段完整 build/CTest、检查 diff、独立 commit/push。没有实施、验证完的草稿不得视为权威基线。调研与实施分开记录，未验证的实现不作为权威基线。

## 阶段记录

- 群角色阶段：SQL 013 增加群主与管理员；服务端、client library 和 Qt 实现成员角色与管理员任免。完整 Qt 构建及 CTest 13/13 通过，包含角色权限、三名上限、重连和群主成员约束验证。

- 群成员阶段：SQL 014 独立保存加入水位；服务端、client library 和 Qt 实现邀请联系人、改群名、自行退出，以及成员/角色/阅读位置快照恢复。群已读按至少一位其他成员的真实阅读水位判断。完整 Qt 构建及 CTest 13/13 通过，包含等待会话锁的邀请、退出后权限与通知隔离、重新加入的历史/未读/角色、真实 Qt 管理操作验证。

- 群管理生命周期：实现群主向当前管理员手动转让、群主移除管理员或成员、管理员仅移除普通成员。复用 SQL 013/014，不增加 schema；被移除者收到明确通知，Qt 关闭该群与相关窗口，重新邀请恢复普通成员和新的加入/阅读状态。实际验证结果见 [开发状态记录](development-status.md)。
