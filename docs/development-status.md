# 开发状态

本记录对应截至 2026-10-04 的仓库实际历史。阶段提交和 push 结果以 Git 历史为准，Telegram 调研与裁剪依据见 [调研记录](telegram-group-design-research.md)。

上一轮长期 Goal 从 `bac606681cac9acff3f75c9c0588831b77646214` 开始；头像在该基线已完成，未重复开发。阶段 0–12 的产品能力和最终综合审查均已完成。本轮从 `49bfa2941dd23aff3c50332d7a90b8d9cae9d592` 开始，仅收口产品语义、授权关系和状态转换，不增加产品功能。下文各阶段的“下一阶段”是当时的开发记录，好友关系已在后续百人验证 Goal 中明确变更，当前关系规则以下面的“好友申请与确认”为准；历史单向授权描述仅记录旧版本。

## Qt / TUI 一致性收口与冻结

本轮实际 `BASE_HEAD` 为 `9b2c09de393969347e00a13303b81a82cae3df7d`。重新 fetch 后 HEAD 与 origin/main 一致，工作区和 submodules 干净。只修复客户端展示、动态列表选择和二级导航，并按用户追加要求实现 TUI 消息左右布局。

| 问题 | 修复 | 回归边界 |
|---|---|---|
| TUI incoming/outgoing 刷新只 clamp 索引，前项删除或重排会改变操作目标 | 应用快照前临时保存选中 user ID，刷新后按 ID 恢复，消失则 clamp，空列表归零 | 前项/当前项删除、重排、空列表；真实 y/n/x 的 SDK 事实与选中人一致 |
| TUI 群选人刷新按原索引定位，过滤后也可能漂移 | 按过滤后的候选 user ID 恢复选择；清除失去 accepted 关系的已选项，隐藏的已选好友仍可取消 | 有/无过滤、删除、重排、空列表及已选项取消 |
| Qt 添加好友的 Back 一律返回 Contacts | 只记录 Chats、Contacts、New friends 三种来源，Back 恢复来源及一级高亮；New friends 再返回 Contacts | widget 实际按钮点击及真实 X11 的三条返回路径 |
| 本地已确认好友搜索在 Contacts 与选人窗口语义不同 | 统一字面 substring；Qt 使用 Unicode 大小写不敏感，TUI 仅折叠 ASCII 大小写，其他 Unicode 按字面匹配 | ASCII、中文、空格、点号、Qt Unicode 大小写与 TUI 边界；TUI /、清空、j/k/Enter；New friends 固定入口保留 |
| TUI 操作错误提示与只读会话横幅不一致 | 从当前关系推导同一提示：未好友、已发申请、收到申请、正在刷新聊天权限 | 状态、渲染及真实 SDK 集成；不保留旧统一文案 |

远端 Add Friend 的 `search_users` 仍使用服务端 prefix 查询，没有把待确认申请或陌生人放入 accepted Contacts。没有增加长期 selected ID 或权限 shadow state。TUI 资料、复制和好友动作统一读取当前可见用户，修复 `:contact` 忽略 New friends 首行产生的索引偏移。入群审批响应不再重复本地删行，由现有权威刷新保持申请人 ID；真实异步回归验证请求处理期间移动选择不会换人。

TUI 消息自己的靠右、他人的靠左，短消息块按内容收缩，长消息最多占聊天区 75% 宽度并换行。发送者、时间、引用、附件、回应、已读和消息滚动保留；历史和搜索使用同一绘制路径。渲染回归覆盖 60/80/120/160 列及长中文消息的头、中、尾可达，真实 tmux 在 80/160 列核对两方消息位置。

P3 取舍：Qt 文件选择后的二次发送资格检查使用“当前会话暂时无法发送文件。”，断线仍保留原提示。Qt 已打开 Profile 的 presence 展示保持原行为，本轮不增加资料窗口的 presence 变化订阅或失效信号；头像与好友关系的既有刷新保留。

本轮没有修改 server、client library、RPC、WebSocket、SQL、数据库模型或 third/submodule；migration 仍连续为 001–026。X11 测试驱动等待窗口实际映射后再聚焦，并修正实际按钮坐标；断言和 timeout 保持。失败日志与成功截图保留，只清理本轮创建的专用测试数据库，不修改已有用户身份。

各局部回归先确认 RED 再修复为 GREEN；Qt models/delegate/UI 定向 3/3、TUI state/render/integration 定向 3/3 均通过。最终代码实际执行 `tests/verify.sh`，Qt/TUI 均 ON，没有 suppression、排除测试、忽略失败或放宽 timeout：

| 构建 | 完整 build | 完整 CTest | CTest 耗时 |
|---|---|---|---|
| normal Debug | PASS | 20/20 PASS | 93.29 s |
| ASan | PASS | 20/20 PASS | 127.28 s |
| UBSan | PASS | 20/20 PASS | 116.67 s |

真实 tmux 导航/动作回归 9/9 PASS，真实 Qt X11 导航回归 4/4 PASS。证据分别位于 `/tmp/chat-ui-freeze-tmux-aligned`、`/tmp/chat-ui-freeze-x11-final`，最终统一验证日志为 `/tmp/chat-ui-freeze-verify-final.log`。`git diff --check` PASS，submodules 干净，没有新增临时 migration、调试代码或兼容入口。

本轮完成后冻结现有 Chats、Contacts、New friends、New、Account、二级返回、本地好友搜索和只读会话信息架构。后续仅在真实复现支持下做必要的小修复，不自行展开新产品功能或全仓重构。

## 长期 Goal 恢复核验与最终收口

本次恢复执行的实际 `BASE_HEAD` 为 `387cf76fc60455d54747d71cc0158e844893f0af`。重新 fetch 后 `HEAD == origin/main`，工作区和 submodules 干净，没有未完成的头像修改。根据当前实现、SQL、回归和 Git 历史逐项核验阶段 0–12；下方路线表中的十五个产品阶段及修复提交（`bac6066` 至 `49bfa29`）均已存在于 `origin/main`，没有重复开发或合并阶段。保留后来已经发布的好友确认模型和 Qt/TUI 客户端。

完成需求与规范两轴审查，复核 server/client/Qt 的连接与 QObject 生命周期、现有 generation 和请求状态、上传清理、头像及图片缓存、事务失败后的连接恢复、锁后成员与角色权限、实时收件范围，以及服务端 cursor 排序。群阅读仍基于当前其他成员的真实读位；mute 仅抑制通知，包括 mention；群公告与消息、个人置顶与群消息置顶保持独立。链接撤销不删除已有 pending，审批接受与加入水位在同一会话锁事务中完成。没有确认新的代码缺陷，没有因文件大小拆分模块或增加状态、抽象和产品功能。

SQL 编号连续为 001–026，当前 migration 回归覆盖 fresh 升级及已有身份、消息和读位保持。本次没有新增或修改 migration；长期产品阶段使用 SQL 015–023，024–026 为随后已经发布的身份及好友关系演进。完整验证仍采用已经实际运行的本地 `tests/verify.sh`；CI 未接入缺少固定工具链获取方案的 hosted workflow，限制见 [验证说明](verification.md)。

重新运行已有 `chat_tui_scale_fixture` 的 `configure → seed → measure_sizes → close`。使用新建专用数据库并依序应用 001–026，账号、已确认好友、成员和消息全部通过 SDK 建立，没有用 SQL 注入业务状态。每种规模各三个真实成员查询、历史查询及发送样本，成员数与 read positions 数均等于群规模：

| 人数 | get_members 均值 | history 均值 | send 均值 | 结果 |
|---:|---:|---:|---:|---|
| 3 | 53.806 ms | 4.022 ms | 17.890 ms | PASS |
| 10 | 2.053 ms | 56.167 ms | 68.399 ms | PASS |
| 50 | 43.116 ms | 146.359 ms | 18.285 ms | PASS |
| 200 | 98.542 ms | 44.831 ms | 18.487 ms | PASS |

均值包含网络、数据库和回调等待，是本次观测而非性能阈值。部分成员在线，不代表 200 个在线连接的 fanout 吞吐；成员快照、读位和通知遍历仍随人数增长，没有设定新人数上限。SDK 与 server 正常退出，成功删除本轮自建数据库，未修改既有用户身份。证据位于 `/tmp/chat-long-goal-capacity-q_26fdex`。

审查和规模测试之后，实际完整执行 `tests/verify.sh`，Qt/TUI 均 ON，三套构建及全部 20 个 CTest 通过：

| 构建 | 完整 build | 完整 CTest | CTest 耗时 |
|---|---|---|---|
| normal Debug | PASS | 20/20 PASS | 92.22 s |
| ASan | PASS | 20/20 PASS | 125.83 s |
| UBSan | PASS | 20/20 PASS | 117.05 s |

包含 migration、server/client、friendship、PostgreSQL、Qt model/delegate/真实多窗口 UI，以及 TUI 的状态、渲染、文件、跨线程投递和真实集成测试。既有连接重连、并发消息、成员竞争、edit/delete/reaction/read/typing、附件头像、移除转让、邀请重入、mute/通知、mention、群 pin、公告、链接及审批回归均保留。没有 suppression、排除测试、忽略失败或放宽 timeout，`git diff --check` PASS；完整日志为 `/tmp/chat-long-goal-final-verify.log`。

本次收口仅更新实际审查与验证记录。产品路线和最终审查已完成，边界仍见下方“保持的边界与后续可选路线”，不继续多设备、E2EE、音视频、超大群或新的基础设施。

## 好友申请与确认

正式关系由陌生人、outgoing pending、incoming pending、accepted friend 构成。申请必须由对方明确接受，接受后在同一事务创建双向 contacts；pending 不进入联系人列表，不获得 direct communication 或 presence 权限。反向申请只呈现已有 incoming，不暗中接受；同方向重复申请幂等。好友删除原子清除双向 contacts 和该 pair pending。现有单聊与历史保留，双方变为只读：仍可查询历史/搜索、下载旧附件、mark read、个人静音/置顶、删除自己旧消息；不能 send/reply/edit/reaction/typing 或新附件。

SQL 026 新增仅保存当前 pending 的 `friend_requests`，用户外键、自申请 CHECK、无序 pair 唯一索引；不保存决定历史。SQL 001–025 不改。迁移保留已有双向联系人，将旧单向关系转为原方向 pending 并删除该单向 contacts，绝不自动授予反向好友权限；保留身份、历史和原创建时间。开发库实际 preflight 为 41 条 contacts：3 对互为联系人、35 条单向，无 self/orphan 行。停服后再次扫描并应用 026，结果保留 6 条 contacts、生成 35 条 pending；逐行核对关系方向和创建时间，全部用户名/用户 ID 保持不变，重启后 health PASS。

SDK 与 RPC 使用 `send_friend_request`、`get_friend_requests`、`respond_friend_request`、`cancel_friend_request` 和双向 `remove_contact`；旧 `add_contact` 不再提供。pair mutation 按用户 ID 顺序取得 `FOR NO KEY UPDATE`，再锁已有 direct conversation；既有 direct mutation 继续在同一 conversation 锁后复核权限。双方 `friendship` 通知只表示状态失效，由客户端重新获取 contacts、friend requests、presence 与 conversations；重连取持久化权威快照，不建立 decision history。

Qt 联系人顶部提供“新的朋友”与未处理数量；Qt/TUI 独立区分 incoming/outgoing，资料与搜索按关系提供添加、等待/取消、接受/拒绝、消息/删除。群成员身份不构成好友关系。手工建群/邀请仅选择已确认好友，选人支持搜索、多选、取消与计数，再输入群名确认；群资料以概要和全部成员入口呈现，角色与管理动作分层。邀请链接与群加入审批仍不要求好友。

Qt 左下角自身头像打开账号资料，替代独立退出导航按钮；资料提供头像操作和确认退出登录。TUI `:account` 提供等价入口，不启用鼠标或终端图片。百人群、百人在线、稳态与最终 sanitizer 的实际结果另记于[百人验证记录](tui_100_member_verification.md)。

好友实现阶段统一验证已实际通过 normal、ASan、UBSan，各 20/20 CTest；新增 `friendship` 复用现有 server fixture，覆盖关系状态机、离线恢复及真实数据库锁同步竞争。百人真实矩阵、100 在线 99/99 无重复 fanout、独立 1800.399873 秒稳态及最终统一验证已完成；最终 normal/ASan/UBSan 均 20/20 PASS，分别 93.65/125.41/112.12 秒，证据与限制见专门报告。

## Qt / TUI 信息架构与导航

本轮基线 `4a861f23cabed157f89f8ec97340d72c7048fef2`，fetch 后 HEAD 与 origin/main 一致，工作区和 submodules 干净。只修改两端展示、导航及测试；不新增 SQL/RPC，不改变好友确认、presence 或群成员/角色模型。

Qt 一级只保留 Chats（聊天）、Contacts（联系人），底部自己的头像打开 Account。聊天列表 header 的 `+` 统一提供添加好友、发起群聊、加入群聊；复用现有动作，建群仍先搜索/多选/取消已确认好友，再填写群名，成功后直接进入新群。联系人 header 保留添加好友快捷入口。Account 显示用户名、头像设置/清除与需要确认的退出登录，不是退出程序。

TUI 一级为 Chats、Contacts、Account，常驻键位 `h/c/u`；`N`（Shift+n）打开 New 菜单，提供同样三个动作，`:new` 与原有命令仍可用。小写 `n` 保留申请页的拒绝动作，避免全局 New 与破坏性动作冲突。Contacts header 不再把建群当成主要内容，顶部 New friends(N) 只是进入 incoming/outgoing 的入口；Tab 在两个申请分页之间切换，Esc 返回 Contacts。TUI 不启用鼠标或内联图片。

两端 Contacts 正文始终来自 `get_contacts()` 的 accepted friends，pending 只在新的朋友/关系资料中出现。没有增加客户端过滤来掩盖服务端或 state 污染。陌生人、outgoing、incoming 的 profile 分别提供添加、等待/取消、接受/拒绝，只有 accepted 才显示 Message；无历史 pending 不创建空 direct。已有历史即使删除或重新申请仍保留在 Chats，按权威 `can_send=false` 只读，聊天区区分未好友、等待确认和收到申请；原历史/搜索/旧附件下载/已读/个人静音置顶/删除自己旧消息不受导航改动影响。

本次真实基线测试中，最简单窄屏 Enter→Esc 能返回；可复现的失败是从只读历史切换顶层 `:conversations` 后 Esc 又返回旧会话。另有 Tab 反复压入返回栈及宽屏列表/消息双高亮。先留 RED 后修复：顶级目的地清理旧返回栈，同级 tab 不压栈，只有实际键盘区域反色；历史、草稿和当前会话保留。联系人刷新后按用户身份保持选择，已选好友被删或列表为空时选择合法项，不会无高亮且 Enter 无响应。

定向验证：Qt models/delegate/UI 3/3 PASS；TUI state/render/integration 3/3 PASS。真实 tmux TUI 导航回归 5/5 PASS（只读历史 Enter/Esc、Tab 返回根页面、联系人与新朋友分离、New 三动作、Account 退出）；真实 Qt X11 导航回归 3/3 PASS（accepted-only 联系人与申请隔离、聊天页三动作与两步建群、底部账号取消与确认退出）。同一代码在本次长期 Goal 恢复核验中完整通过 normal/ASan/UBSan，各 20/20 CTest，实际结果见上方收口记录。

## 已完成路线

| 提交 | 阶段 | 数据库演进 |
|---|---|---|
| `4163b94` | 单聊、群聊统一会话；建群、群历史、实时消息、独立已读位置、Qt 群界面及重连 | SQL 008 |
| `132cf46` | 引用真实消息；同会话验证；历史、实时和 Qt 引用展示 | SQL 009 |
| `cb308da` | 仅作者编辑；编辑时间；历史、实时、Qt 和离线恢复 | SQL 010 |
| `83e2bfe` | 仅作者执行全员删除；保留删除占位及引用占位 | SQL 011 |
| `70a37a9` | 单向移除联系人，保留会话与历史 | 无 |
| `2801af9` | 会话内文字搜索；字面匹配、分页、权限和 Qt 搜索窗口 | 无 |
| `4789af6` | 文件、PNG/JPEG 图片；分块传输、持久化、历史、引用、实时、Qt 下载/预览 | SQL 012 |
| `78a28c3` | 单聊/群聊正在输入；节流、结束、过期、切换及断线清理 | 无 |
| `dc4481c` | 创建者群主；群主任免最多三名管理员；角色持久化及 Qt 管理 | SQL 013 |
| `27b2a88` | 管理员/群主邀请联系人、改名；非群主退出；重新加入；快照恢复及群已读语义 | SQL 014 |
| `6b6a193` | 稳态审计、libpq 测试环境、过期回调及生命周期/并发验证 | 无 |
| `6ff2ac6` | 群主转让、成员移除、权限与实时隔离、再邀请及 Qt 生命周期 | 无 |
| `bac6066` | 真实用户头像上传、获取、更换、清除、实时更新与 Qt 缓存展示 | SQL 015 |
| `09e9e25` | 可重复 normal/ASan/UBSan 验证入口与工具链、测试库前提说明 | 无 |
| `c4ae62f` | 群消息已读人数与当前成员详情，复用真实阅读位置 | 无 |
| `aa8a459` | 搜索消息未加载成员身份时保持双勾，避免误显示已读人数 | 无 |
| `09a511e` | 单聊和群聊有限表情回应、持久化、实时与重连 | SQL 016 |
| `a6b1a31` | 图片气泡、完整附件传输、缓存及点击预览 | 无 |
| `3001ed5` | 桌面消息通知、实际阅读判定及窗口恢复 | 无 |
| `323c9f9` | 个人会话免打扰、服务端持久化、通知抑制及重连 | SQL 017 |
| `638b775` | 个人会话置顶及跨置顶/普通层的完整 cursor 分页 | SQL 018 |
| `a653634` | 群成员提及、真实目标持久化、文字高亮与重连 | SQL 019 |
| `31d6cc4` | 群内单条消息置顶、实时摘要、编辑与删除一致性、历史搜索 | SQL 020 |
| `d6c0952` | 当前群公告、纯文本编辑、实时查看及草稿与权限生命周期 | SQL 021 |
| `fbdb06b` | 单个高熵群邀请链接、撤销、非联系人加入及完整生命周期 | SQL 022 |
| `93d8dc3` | 链接加入审批、私有申请通知、分页管理与原子成员创建 | SQL 023 |
| `49bfa29` | 异步回调、图片下载和连接生命周期收口 | 无 |
| `3f1acef` | 单向联系人通讯授权、历史只读、权威 can_send、Profile 和 presence 隐私 | 无 |

另外完成历史大响应接收、编辑消息布局和消息操作按钮对比度修复，分别见 `32f3f3b`、`a545c9a`、`f73b8f4`。

## 数据模型

- `users` 保存账号、最后在线时间和单调递增的 `avatar_revision`；`contacts(owner_id,contact_id)` 保存已确认好友的双向记录；`friend_requests` 只保存当前待确认申请。`user_avatars` 保存一个当前头像，是否存在由数据行决定；不与消息附件共表。
- `conversations.kind` 显式区分 `direct/group`；单聊使用真实的两端 user ID，群使用会话 ID、群名和 `owner_id`。
- `conversation_members` 表示当前成员，持有 `is_admin`、`last_read_message_id`、`joined_message_id` 和个人 `muted/pinned`。群主必须是群成员，由延迟外键保证；管理员上限在同一会话锁事务内检查。
- `messages` 指向会话和真实作者，包含回复 ID、编辑时间、删除占位；`message_attachments` 保存附件元数据和内容。删除附件消息会清除文件内容。SQL 016 增加独立 `message_reactions` 和消息的单调 `reaction_revision`，每用户每消息一条回应，删除消息时清除回应。
- SQL 019 的 `message_mentions(message_id,user_id)` 保存群文字提及的真实目标；编辑替换、删除清空，退出成员不删除历史目标。旧消息不回填。
- SQL 020 的 `conversations.pinned_message_id` 保存每群一个当前置顶消息；与个人 membership 的 pinned 独立。初始为空，软删除同事务清空，物理删除通过外键 SET NULL 清空。
- SQL 021 的 `conversations.announcement` 保存每群一个当前纯文本公告，默认空字符串，空字符串表示未设置；数据库约束限定 group 和最多 4096 个 UTF-8 字节。没有公告历史或消息伪装。
- SQL 022 的 `conversations.invite_token` 保存每群一个可空、唯一的当前邀请 secret；仅 group 可保存 64 位小写十六进制 token。撤销设为 NULL，重新创建使用新的 OpenSSL 随机值，不维护过期、次数或链接历史。
- SQL 023 的 `conversations.join_approval` 控制邀请链接是否产生申请，默认 false；`group_join_requests(conversation_id,user_id,created_at)` 仅保存唯一的待处理关系，接受/拒绝后删除。申请不等于 membership，没有审批历史。
- SQL 024 增加用户名和群标题 CHECK；不修改身份、不改变大小写敏感唯一性，不回改 SQL 001–023。
- SQL 008 保留旧单聊、自聊、消息、联系人和阅读位置；SQL 009–014 渐进增加上述能力。SQL 013 应用前已确认本次数据库没有既存群，不猜测旧群创建者，也不删除旧消息。
- 新建或重新加入的成员能读取完整历史。邀请取得会话锁后读取最新消息 ID 作为加入水位；实际阅读仍从 0 开始，仅由 `mark_read` 推进。未读统计使用 `id > greatest(last_read_message_id, joined_message_id)`，排除删除消息及群成员自己的消息。

## JSON-RPC 与 realtime

现有 JSON-RPC 2.0、WebSocket 和 cursor pagination 保持统一。废弃的成员 `users` 响应已改为明确的 `members`，不保留双格式兼容。

| RPC | 主要参数或结果 |
|---|---|
| `open_direct_conversation` / `create_group` | 真实用户 / 群名及自己的联系人 ID 列表；open 要求当前联系人，返回 `conversation,can_send:true`；create 返回会话 ID |
| `get_conversations` | `(pinned, activity, id)` cursor；全部当前 membership，包括已创建的空单聊；置顶优先，各层内活动时间和 ID 降序；会话资料、未读、个人 muted/pinned 和权威 `can_send` |
| `set_conversation_muted` | `conversation, muted`；只修改当前成员自己的偏好，返回当前 muted |
| `set_conversation_pinned` | `conversation, pinned`；只修改本人列表排序偏好，返回当前 pinned |
| `get_messages` | 会话及互斥的 `before/after` 消息 ID；消息、当前成员实际阅读位置、`has_more` |
| `mark_read` | 会话和真实消息 ID；阅读位置只增不减 |
| `get_members` | `members: [{id, username, avatar_revision, has_avatar, role}]`，角色为 `owner/admin/member` |
| `set_group_admin` | `conversation, user, admin`；仅群主；返回 `changed` |
| `transfer_group_owner` | `conversation, user`；仅群主转给当前管理员；返回 `changed` |
| `remove_group_member` | `conversation, user`；群主移除 admin/member，管理员仅移除 member；返回 `changed` |
| `rename_group` | `conversation, title`；群主/管理员；返回 `changed` |
| `invite_group_members` | `conversation, members`；群主/管理员邀请自己的联系人；重复邀请不重置成员状态 |
| `leave_group` | `conversation`；普通成员/管理员自退，群主先转让再退出；返回 `changed` |
| `pin_group_message` / `unpin_group_message` | 会话及真实消息 ID / 会话；仅群主、管理员；返回 `changed` |
| `set_group_announcement` | `conversation, text`；群主/管理员设置当前纯文本，空字符串清除；返回 `changed` |
| `get_group_invite` / `create_group_invite` / `revoke_group_invite` | `conversation`；仅当前群主/管理员；返回 `{token: null|string}`；创建稳定，撤销幂等 |
| `join_group` | `token`；已认证用户使用当前链接；返回权威 `conversation/title/member_count/state`，state 为 `joined/member/pending` |
| `set_group_join_approval` | `conversation, required`；当前群主/管理员设置链接加入模式，返回 `changed` |
| `get_group_join_requests` | `conversation, before?`；当前群主/管理员获取申请人 metadata、创建时间及 `next`，每页最多 50 条，按 user ID 降序 |
| `respond_group_join_request` | `conversation, user, accept`；当前群主/管理员接受或拒绝，原子结束 pending 关系，返回 `changed` |
| `send_message` / `edit_message` / `delete_message` | 会话、真实消息或回复 ID；编辑/删除仅作者且仍为当前成员 |
| `search_messages` | 会话、字面查询和 `before` cursor |
| `set_message_reaction` | `conversation, message, emoji`；六种表情之一，显式空字符串清除；返回 reaction snapshot |
| 附件 RPC | begin/upload/finish/cancel/get；32 KiB 分块，单文件最多 10 MiB |
| `set_typing` | 会话、开始/结束；不写消息或阅读位置 |

消息、编辑删除、已读和输入提示分别使用 `message/message_updated/read/typing` notification。群变化使用 `conversation` notification，包含真实会话 ID；事务提交后通知当前成员，退出者或被移除者额外收到一次 `removed: true` 的会话变化通知。之后的群访问被成员权限检查拒绝，后续群消息不再发送给该用户。client library 的 conversation handler 接收 `(conversation, removed)`，不保留旧签名兼容层。

消息、发送确认、历史、搜索和会话最新消息携带 `mentions:[{user,username}]`。目标由服务器在会话锁事务内解析当前群成员，不能由客户端指定；没有独立提及 notification。静音抑制包括提及在内的全部桌面通知。

Qt 收到群变化后刷新会话、成员及实际阅读位置；关闭已退出的活动群，清除历史、回复、草稿、输入提示并禁用发送。重连刷新权威快照，并从已加载历史的起点向前分页，恢复新增消息以及已加载消息的编辑/删除。群消息在至少一位其他成员实际阅读到该消息时显示已读，入群水位不产生已读回执。

当前单连接按顺序处理 RPC 和 notification，不引入事件日志或通用版本体系。没有多设备、群在线人数统计或分布式 presence。

## 验证

SQL 014 所在阶段运行：

```sh
cmake -S . -B build -DCHAT_BUILD_QT_CLIENT=ON
cmake --build build -j12
PGPASSWORD=chat ctest --test-dir build --output-on-failure
git diff --check
```

完整 build PASS；CTest 全部 13/13 PASS。包含 migration、client、PostgreSQL 连接、超时、HTTP、WebSocket、server、JSON 反射、在线用户、密码、Qt model、Qt delegate 和真实 Qt UI。

新增覆盖三名管理员上限、任免权限、群主成员约束、改名、自己的联系人邀请、退出后历史/搜索/附件/编辑删除/输入/阅读权限、退出后通知隔离、邀请等待会话锁、新加入实际读位为 0、旧历史不计未读、重新加入角色及重连。Qt 测试使用三个真实窗口完成管理、退出、再邀请和历史恢复，截图已检查；另验证新打开的空单聊不会因会话列表刷新而关闭。

## 保持的边界与后续可选路线

邀请链接默认直接加入，可由群主/管理员启用审批；显式联系人邀请继续直接加入。群公告、@mention、个人会话置顶和群内单条置顶消息已接入，个人会话置顶不等于群内置顶消息。群主必须先手动转让再退出；群主/管理员没有编辑、删除他人消息的权限。退出或被移除者本地活动历史清空；服务端仍保留群消息，重新加入可重新获取。移除不等于永久封禁，重新邀请或使用有效链接均可重新加入或提交审批，恢复普通成员，旧管理员身份和真实读位不继承。

2026-10-02 启动的长期路线已按阶段独立完成：验证基线、群已读详情、reaction、图片气泡预览、桌面通知、会话 mute/pin、群 mention、群置顶消息、公告、邀请链接和审批。最终审查只修复实际生命周期问题，没有增加产品功能。范围仍不扩大到多设备、微服务、Redis、Kafka、event sourcing 或 CQRS。

## 稳态与工程收口

以重新 fetch 后的 `27b2a88` 为基线完成 `16fd4be..HEAD` 审计，详见 [稳态审计记录](steady-state-audit.md)。本阶段无 migration，无新产品功能。测试去掉 PostgreSQL 主机、数据库、用户硬编码，统一继承 libpq 环境；修复新连接未失效旧搜索/附件回调，以及 GCC 静态反射在 sanitizer/Debug 构建中的同名局部类型冲突。

新增或增强事务失败后连接池恢复、真实 send/invite 锁竞争、上传中断/重连/退出清理、过期 Qt 回调和小群规模测试；现有生命周期回归继续覆盖 leave/rejoin、typing/disconnect、edit/delete/reconnect。没有删除仍有职责的状态字段，也没有拆分巨型文件或增加配置框架。

验证使用调用进程提供的 `PGHOSTADDR/PGPORT/PGDATABASE/PGUSER/PGPASSWORD`，所有构建启用 Qt：

| 构建 | 完整 build | 完整 CTest |
|---|---|---|
| `build`，Debug，默认 `-g` | PASS，`-j12` | 13/13 PASS |
| `build/asan`，Debug，`-g1 -fsanitize=address -fno-omit-frame-pointer` | PASS，`-j12` | 13/13 PASS，无 suppression 或测试排除 |
| `build/ubsan`，Debug，`-g1 -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer` | PASS，`-j12` | 13/13 PASS，无 suppression 或测试排除 |

ASan/UBSan 的 C、C++ 编译器及 executable linker 均启用对应 sanitizer。`git diff --check` PASS。测量每种群规模各三个样本，Debug 的平均端到端延迟如下；除了发送者，测量成员均离线：

| 成员数 | get_members | history（含全部读位） | send（含发布查询/遍历） |
|---|---|---|---|
| 3 | 1.67 ms | 2.26 ms | 7.83 ms |
| 10 | 1.35 ms | 2.42 ms | 7.23 ms |
| 50 | 2.01 ms | 2.19 ms | 7.05 ms |
| 200 | 43.95 ms | 43.56 ms | 10.24 ms |

阶段 1 已验证 3–200 人范围的完整成员、读位和发布路径；这是测量范围，不是人数上限或在线吞吐承诺。未引入大群基础设施。群主转让和成员移除的实现与验证单独记录。

## 群管理生命周期

以重新 fetch 后的 `6b6a193` 为基线实现 `transfer_group_owner` 和 `remove_group_member`，server、client library、Qt 界面及测试形成闭环。转让只允许当前 owner 选择当前 admin；同一事务中目标成为 owner、`is_admin=false`，原 owner 降为 admin，管理员数量保持不增加。原 owner 随后可正常退出。

owner 可以移除 admin 或 member，不能移除自己；admin 只能移除普通 member，不能移除 owner、其他 admin 或自己。移除删除当前 membership，不引入封禁列表。重新邀请使用默认普通成员身份和实际读位 0，加入水位为取得会话锁后的最新消息 ID；旧历史可见但不计未读。

所有群管理与发送继续使用同一 conversation 行锁；实时发布也在取得该锁后的新查询中读取收件人，并在锁释放前同步入队，避免成员移除期间使用旧快照。typing 发布另外复核发送者仍为成员，抑制检查后被移除者的提示。没有增加 event log、event bus 或 version framework。

被移除的在线 session 清理该群上传并收到一次 `conversation {conversation, removed: true}`；后续消息、编辑、删除、阅读和输入通知不再路由给它，历史/搜索/成员/附件/发送/修改/阅读/输入请求被现有成员校验拒绝。离线用户通过重连权威快照发现已无该群权限，不保存待回放事件。开始上传也在会话锁内建立 session 状态；finish 在任何 await 前接管内容，避免其他 session 清理上传时破坏挂起请求。

Qt 立即复用 `close_conversation` 清历史、回复、草稿、typing 和附件发送状态，同时关闭该群的成员、搜索、附件及嵌套保存窗口；迟到结果按会话 ID 过滤，已关闭 QObject 不再接收回调，不新增跨会话 shadow state。群成员窗口按实时角色启用转让/移除按钮，确认窗口使用稳定目标 ID，重连重新获取成员与角色。

数据库没有新增字段、表、索引或 migration：SQL 013/014 已足够表达本次规则，SQL 001–014 保持原样；真实头像在后续独立阶段使用 SQL 015，见下文。旧单聊、消息占位、每成员实际读位与三名管理员上限保持。

测试新增完整权限矩阵、原群主退出、移除后的所有访问拒绝及通知隔离、被移除管理员重新邀请后的角色重置、非零真实读位重置、加入水位/未读和重连快照。并发测试实际等待同一会话锁，覆盖 transfer/leave、remove/send、remove/invite、remove/typing，以及被移除的半途上传和已挂起 finish。Qt 三个真实窗口验证转让、重连角色、移除时成员/搜索/附件/保存窗口关闭及聊天状态清理；转让截图已检查。

本阶段最终验证如下，三个构建均启用 Qt、使用 Debug 和 `-j12`，数据库参数继承 libpq 环境：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| 正常 `build` | PASS | 13/13 PASS | 43.24 s |
| ASan `build/asan` | PASS | 13/13 PASS | 55.54 s |
| UBSan `build/ubsan` | PASS | 13/13 PASS | 51.94 s |

Sanitizer 编译与链接选项同阶段 1，未使用 suppression 或测试排除。ASan 发现 raw WebSocket 协程测试接收字符串的生命周期泄漏；接收结果改为具名对象并复用循环结果后，原测试与完整 CTest 均通过。扩展后的服务器集成测试在 ASan 下单次实测 31.23 s，因此 CTest 时限由 30 s 调整为 60 s；所有行为断言和泄漏检查保持开启。`git diff --check` PASS，没有新增临时 migration、调试打印或 TODO。


## 真实用户头像

从重新 fetch 后的 `origin/main = 6ff2ac6` 开始完成头像闭环，方案与协议详见 [头像设计](avatar-design.md)。开始时 HEAD 与远端一致、工作树 clean；未修改 SQL 001–014。新增 SQL 015 给既有用户无损增加 revision 默认值，并建立独立 `user_avatars`；测试数据库已应用该 migration。

revision 在上传和清除时都递增，与当前数据一起事务提交；清除保留 revision，后续上传继续递增，避免 `1 → 0 → 1` 的 ABA。业务对象使用 `avatar_revision + has_avatar`，联系人、会话、历史、用户搜索、成员及认证不携带头像二进制。历史消息显示发送者当前头像。

独立 RPC 为 `begin_avatar_upload/upload_avatar_chunk/finish_avatar_upload/cancel_avatar_upload/get_avatar/clear_avatar`；client library 提供上传、获取、清除及 avatar notification handler。任何认证用户可读取与用户搜索同样可见的头像，修改仅限自己。PNG/JPEG 按实际 magic 和完整解码验证；最多 1 MiB、32 KiB chunk、16 × 1024 × 1024 像素，满足现有服务器 64 KiB WebSocket 入站限制。上传随机 ID 和内容只属于 session；取消、断线、重新连接均不能继续旧上传，消息附件状态保持独立。

`avatar` 通知包含 user、revision 和 has_avatar，发送给本人、双向联系人、已有单聊对端、当前共享群成员，去重且不全局广播。过期 get 请求返回权威 metadata 和空内容；客户端按新状态重取。真实用户资料是公开信息，移出群不使头像成为私有。

Qt 在现有 avatar 绘制文件中保存最小当前头像缓存：一用户一当前版本，重复观察只下载一次，绘制使用已解码 QPixmap；版本变化只清该用户图，过期结果不能覆盖新状态。下载失败继续 fallback，重连保留匹配的成功缓存并重试失败/中断请求。新连接、close、实际断线使头像回调失效，logout 清理缓存；SDK 析构抑制 pending 回调，正常 close 仍报告传输错误。

账号头像、联系人/用户搜索、单聊会话、单聊/群消息、资料窗口、单聊 header、群成员和消息搜索均接入同一圆形绘制。未设置时保留首字符与本地背景色；自己的资料窗口提供更换/移除和轻量状态，没有成功弹窗。关闭资料窗口后上传完成不会访问已销毁控件。没有头像裁剪、动画、历史、自定义群头像、多尺寸服务版本或存储基础设施扩展。

回归覆盖 migration 默认值和级联；未认证/非法参数/超限/无效图片/偏移/base64/取消/断线旧 ID/附件隔离；替换、清除、再上传及原子并发读取；当前用户 metadata、相关通知去重、空单聊关系及无关用户隔离；SDK 多 chunk、异常响应/通知和析构；Qt 缓存去重、版本变化、失败 fallback、重试、单用户刷新与旧结果；三个真实 Qt 窗口完成多 chunk PNG、JPEG 更换、清除、联系人、单聊、群、成员、资料、重连、关闭窗口和重新登录。真实截图已检查。

Qt 全链路测试在本阶段一次 ASan 下实测 29.27 s，接近原 30 s 总时限，因此总时限改为 60 s；单步 5 s 超时和行为断言保持。发现群退出测试在异步列表刷新前进行否定断言，现先等待退出快照到达，再验证后续群消息不会恢复该会话；没有去掉隔离检查。

最终验证如下，所有构建启用 Qt、Debug、`-j12`，使用 libpq 环境变量提供数据库参数：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| 正常 `build` | PASS | 14/14 PASS | 46.75 s |
| ASan `build/asan` | PASS | 14/14 PASS | 60.59 s |
| UBSan `build/ubsan` | PASS | 14/14 PASS | 54.48 s |

ASan/UBSan 的 C、C++ 和 executable linker 选项沿用前一阶段；未使用 suppression 或排除失败测试。新增独立 `avatar_image` 测试，其余头像行为扩展既有 server/client/Qt 测试。最终 ASan Qt 全链路实测 29.99 s。`git diff --check` PASS，无临时 migration、调试打印、废弃 API 兼容层或明显 TODO；本阶段仅交付头像，完成后停止，不继续新产品路线。

## 长期路线：可重复验证基线

新的长期 Goal 从重新 fetch 后的 `HEAD = origin/main = bac6066`、干净工作树开始。代码、SQL 015、测试和前述验证记录确认头像阶段已经完成，没有重复实现。本阶段没有新增产品能力或 migration。

新增 `tests/verify.sh`，从任意工作目录顺序运行 normal/ASan/UBSan 完整配置、`-j12` 构建、Qt ON、全部 CTest，最后检查 diff；CTest 显式单进程，避免共享测试 schema 和 Qt 固定端口竞争。继承 libpq 环境，不自动迁移或保存凭据。工具链、空测试库初始化、测试隔离前提及 CI 评估见 [验证说明](verification.md)。

GitHub Actions 尚未接入：当前完整验证的是特定 GCC 16 trunk、Boost 1.92 和固定 third gitlink，标准 hosted runner 没有对应编译器，仓库尚无固定工具链获取方案。本阶段交付可靠本地入口，没有提交未验证的 workflow。PostgreSQL service 和 Qt offscreen 本身不构成接入障碍。

实际执行新入口一次，全部构建通过，未排除服务器/Qt 测试或使用 sanitizer suppression：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| 正常 `build` | PASS | 14/14 PASS | 42.72 s |
| ASan `build/asan` | PASS | 14/14 PASS | 61.16 s |
| UBSan `build/ubsan` | PASS | 14/14 PASS | 57.98 s |

`bash -n tests/verify.sh` 与 `git diff --check` PASS。下一阶段为群聊已读详情；其余长期路线仍属规划。

## 长期路线：群聊已读详情

以重新 fetch 后的 `09e9e25` 为基线，复用 `get_messages.read_positions`、`get_members` 和现有 `read/conversation` 通知，没有新增 RPC、receipt 表或 migration。

群消息显示“已读 N 人”，点击文字或右键“已读详情”打开当前成员的姓名和头像列表。人数、列表与群双勾使用同一条件：当前其他成员的真实 `last_read_message_id >= message.id`；排除当前查看用户，不把 `joined_message_id` 或发送行为当作阅读。退出成员不计入，入群前历史可见但加入水位不产生已读。单聊保持原单/双勾。

选择群时获取成员身份快照；阅读、成员、头像变化实时刷新详情。会话关闭、退出、移除、logout 或目标消息删除时关闭窗口，QObject context 管理回调。群变化和断线先清活动群的旧读位，再由权威历史恢复；沿用已有 history generation 丢弃过期请求，防止离线或快速重入时把旧高读位 `max` 合并进新 membership。没有新增 generation、成员版本或通用状态框架。

新增模型断言覆盖零/一/多读、自己排除、成员快照已退出但旧读位仍高、重入读位 0、分页和快照与较新阅读事件合并；delegate 验证可点击文字及单聊无该入口。服务器集成测试确认分页与重连携带真实读位。三个真实 Qt 窗口验证详情姓名/ID、实时读人数、重连、退出后人数以及移除时关闭详情；截图已检查。

实际执行 `tests/verify.sh`，完整验证如下：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| 正常 `build` | PASS | 14/14 PASS | 45.54 s |
| ASan `build/asan` | PASS | 14/14 PASS | 58.90 s |
| UBSan `build/ubsan` | PASS | 14/14 PASS | 58.50 s |

无 sanitizer suppression、测试排除、新临时状态或调试代码，`git diff --check` PASS。下一阶段为有限 emoji reaction，尚未实现。

后续准备时核查到搜索窗口复用 message delegate，但没有成员姓名快照。独立修复未加载成员身份时误显示“已读 0 人”和原双勾失效：此时按权威 read positions 保持原双勾，只在成员快照已存在时显示人数和详情入口。没有新增 RPC 或兼容协议；新增模型回归覆盖这个窗口边界。重新执行完整入口：normal 14/14（45.10 s）、ASan 14/14（58.74 s）、UBSan 14/14（55.86 s），所有 build、diff 检查通过，无测试排除或 suppression。

## 长期路线：消息表情回应

从重新 fetch 后的 `aa8a459` 开始，单聊和群聊使用同一回应模型。固定支持 👍、❤️、😂、😮、😢、🎉；每用户每消息最多一个，选择不同表情替换，选择自己当前表情取消。回应不推进会话活动时间、未读或真实阅读位置。

SQL 016 无损增加 `messages.reaction_revision`（已有消息默认 0）和独立 `message_reactions(message_id,user_id,emoji)`，主键保证每用户一条，数据库约束限定表情。实际修改与 revision 递增在同一事务；幂等设置和重复清除不递增。清空后仍保留 revision，避免迟到的非空快照覆盖空状态。编辑、删除、回应和成员管理使用同一 conversation 行锁，锁后检查当前成员与消息归属。

`set_message_reaction {conversation,message,emoji}` 使用明确设置语义，空字符串表示清除，缺失、null、未知字段或非法表情拒绝。返回及 `reaction` 通知均为 `{conversation,message,reaction_revision,reactions:[{emoji,users:[id]}]}`；数量和当前用户选择从用户列表计算。通知仅给当前会话成员，普通发送者由 RPC 获得确认；历史、搜索、会话最新消息、编辑及新消息统一携带轻量回应快照。离群者的既有回应作为历史内容保留，但不能继续修改。删除消息显式清空回应并递增 revision，保留原消息占位。

SDK 验证 revision、会话/消息 ID、表情白名单、非空且不重复的用户和表情；Qt 使用相同模型展示数量和自己的选择，右键选择及点击已有标签均可操作。paint 不做网络 IO。模型独立合并 reaction revision 与 edited_at，旧历史、编辑或通知不能覆盖新回应；deleted 不恢复交互。活动会话遇到尚未加载的新消息回应时复用历史恢复，未加载的旧消息等待正常分页。搜索窗口也接收回应与删除变化。

Qt 将原头像连接计数器按实际共同职责改名为 connection generation，回应 notification 和 RPC 回调在 QObject 线程复核后发信号；没有新增生命周期计数器。菜单使用 persistent index，触发时再次检查连接、会话和删除状态，避免菜单打开后模型重置或删除时发送到错误消息。

新增回归覆盖 migration 默认值、唯一约束及级联；未认证、非法参数、跨会话、非成员及删除消息拒绝；设置、幂等、替换、聚合、清除及重新添加；历史/搜索/会话摘要和分页重连；回应与移除/删除真实锁竞争；SDK 异常协议；Qt stale revision、编辑与回应交错、删除、菜单、气泡命中和旧连接回调。三个真实 Qt 窗口验证实时聚合、更换、取消、离线变化与重连，截图已检查。竞争测试发现统计视图事务快照会停留在第一次查询，轮询前使用 `pg_stat_clear_snapshot()`，仍要求两个请求实际等待会话锁，未放宽断言。

最终代码实际执行 `tests/verify.sh`，所有构建启用 Qt、Debug、`-j12`，继承 libpq 环境：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| 正常 `build` | PASS | 14/14 PASS | 49.96 s |
| ASan `build/asan` | PASS | 14/14 PASS | 60.43 s |
| UBSan `build/ubsan` | PASS | 14/14 PASS | 60.61 s |

未使用 suppression 或测试排除，`git diff --check` PASS。没有新增通用事件框架、回应历史或自定义 emoji。下一阶段为图片气泡预览和缓存。

## 长期路线：图片消息展示

从重新 fetch 后的 `09a511e` 开始，沿用 SQL 012 的 10 MiB 附件、32 KiB 分块和成员权限，不新增 migration、RPC、服务端缩略图或图片存储。单聊和群聊中的 PNG/JPEG 在气泡内展示按比例缩放的预览；下载中显示占位，损坏图片或下载错误保留文件信息和原文件保存入口。点击气泡打开现有附件窗口，可保存完整原始字节；普通文件行为保持。

`message_images` 集中管理当前账号的图片任务与内存缓存，使用现有 Qt `QCache`，按原始字节和解码预览计费，预算 64 MiB。消息 ID 是数据库全局主键，附件内容不可编辑，因此直接用消息 ID 标识缓存；最多三个下载任务，其余排队，同一消息不会重复排队。只观察可见消息，delegate paint 读取已解码 pixmap，不做网络 IO 或图片解码。正常会话切换丢弃旧队列，已发出的任务继续计入并发上限；成功缓存可在切回及重连后复用。删除、失去成员权限或 logout 清理对应数据，断线清理 pending 和失败状态。bridge 在 Qt 线程校验连接 generation，旧连接结果不能填充新账号缓存。

预览沿用现有附件窗口的 640×480 等比缩放、16 百万像素及 64 MiB 解码分配限制。头像缓存与附件图片缓存职责分开，没有通用资源下载框架。缓存命中时查看窗口复用 pixmap 和原始字节，无重复下载或解码；大图片的首次下载仍需完整附件传输。

新增恰好 10 MiB PNG 上传及同一客户端两个并发完整下载测试，校验每个结果的全部字节。初次实测上传 14.11 s、两次下载共 3.10 s；逐块上传的延迟定位到 TCP 小块写入，客户端使用已有 Corosio `TCP_NODELAY` 接口后上传降至 1.39 s，两次下载共 4.77 s。仅这个由数百 RPC 组成的边界操作使用 30 s 等待，普通 RPC 仍为 5 s；没有扩大附件或 WebSocket 上限。

Qt 回归覆盖排队上限、去重、失败和损坏图片 fallback、迟到结果、切换队列、缓存预算淘汰、删除和纯 paint；三个真实 Qt 窗口覆盖单聊/群聊气泡、实际点击、缓存预览与原文件保存、断线重连保持缓存、删除及 logout 清理。单聊和群聊截图已检查。

实际执行 `tests/verify.sh`，所有构建启用 Qt、Debug、`-j12` 并继承 libpq 环境：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| 正常 `build` | PASS | 14/14 PASS | 52.78 s |
| ASan `build/asan` | PASS | 14/14 PASS | 69.30 s |
| UBSan `build/ubsan` | PASS | 14/14 PASS | 61.91 s |

无 suppression 或测试排除，`git diff --check` PASS。下一阶段为桌面消息通知，尚未实现。

## 长期路线：桌面消息通知

从重新 fetch 后的 `a6b1a31` 开始，复用 Qt Widgets 的 `QSystemTrayIcon`，没有新增 migration、RPC、依赖或通知 daemon。仅实时新消息触发通知，自己的消息、编辑、删除、history 和 reconnect 补拉不会产生普通消息通知。标题使用权威单聊发送者或群名与发送者，正文压缩空白并截取 120 字符；文件和图片使用文件名摘要。首次遇到未知会话先等现有会话刷新，断线、退出、logout、资料刷新失败或消息删除时清理待通知内容。

窗口前台、目标会话已加载且视图在底部才视为实际阅读；最小化、后台、其他会话、历史滚动和模态窗口均不被当作阅读。新消息仅在视图原本位于底部时自动滚动。真实阅读请求由视图触发，服务端确认的读位回到已有 message model；已确认阅读后的反复滚动不重复写读位。没有新增本地 shadow read position。相关消息、编辑和已读确认回调在 Qt 线程检查现有 connection generation，避免旧账号结果或额外信号排队破坏顺序。

通知点击只恢复窗口；Qt 公共 `messageClicked()` 不携带通知身份，不能可靠地将任意旧通知映射到对应会话，因此不伪造最近会话跳转。平台没有托盘或系统禁用通知时，持久化消息及未读继续正常。官方接口与源码核查见 [通知取舍](desktop-notification-research.md)。

三个真实 Qt 窗口的回归覆盖群名与发送者、未知单聊、自己的消息和前台阅读抑制、其他会话、最小化与窗口恢复、图片/文件摘要、历史滚动位置、实际读位、重复滚动、编辑/删除、重连恢复和旧连接回调。另在独立 Xvfb、X11 托盘和 D-Bus session 下运行完整 Qt UI 测试，整条回归 PASS，实际托盘气泡截图已检查。原生验证发现测试将 GTK 文件对话框当作 Qt 控件访问的空指针，测试现明确使用 Qt 文件对话框，应用仍使用原生对话框；没有跳过文件上传、保存或嵌套窗口测试。

最终代码实际执行 `tests/verify.sh`，所有构建启用 Qt、Debug、`-j12`，继承 libpq 环境：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| 正常 `build` | PASS | 14/14 PASS | 51.93 s |
| ASan `build/asan` | PASS | 14/14 PASS | 70.79 s |
| UBSan `build/ubsan` | PASS | 14/14 PASS | 62.78 s |

未使用 suppression 或测试排除，`git diff --check` PASS。下一阶段为服务端持久化的个人会话 mute；本次没有提前加入通知偏好或其他产品功能。

## 长期路线：个人会话免打扰

从重新 fetch 后的 `3001ed5`、干净工作树开始。SQL 017 给 `conversation_members` 无损增加 `muted BOOLEAN NOT NULL DEFAULT false`，既有单聊和群成员默认未静音。偏好属于本人当前 membership；退出或被移除后失效，重新加入从 false 开始。不新增偏好表、计时、通知等级或多设备同步。

`set_conversation_muted {conversation,muted}` 明确设置 true/false，返回 `{muted}`；重复设置幂等，缺失、null、错误类型、未知字段、未认证或非成员请求拒绝。同一 conversation 行锁后重新更新本人 membership，与 send/remove/invite 等现有路径串行化；事务错误关闭连接，权限失败回滚，不遗留不可复用事务。个人设置通过本人 RPC 确认，不向其他成员广播；重新登录和重连从 `get_conversations.muted` 恢复。

静音只抑制桌面通知；服务器实时消息、typing、实际阅读、历史、搜索、未读与活动时间均保持现有行为。SDK 严格解析 bool，Qt 会话列表右键提供“静音/取消静音”，标题旁显示轻量“静音”文字。确认结果更新现有模型并刷新权威列表，同时使较早的列表请求失效；不添加本地 pending mute、影子偏好或额外 generation。通知直接检查同一模型，未知会话仍先获取权威资料。

独立审查没有发现权限或事务问题，补充单聊断线重新登录验证。真实 UI 回归另外复现了菜单打开后实时消息重置会话列表，旧 persistent index 使操作丢失；菜单现保存稳定会话 ID 和明确目标 bool，触发时复核当前会话仍存在。该回归修复前失败，修复后与完整验证一起执行，不模拟列表数据。

回归覆盖 SQL 016→017 及已有成员默认值；SDK metadata、错误响应和显式设置；服务端未认证、参数、权限、幂等、个人隔离、未读保持、单聊重新登录、群重连、remove/reinvite 默认值及真实会话锁竞争；Qt model/delegate、三个真实窗口中的单聊/群聊通知抑制、实时消息和未读、取消静音恢复、菜单刷新竞争及旧连接回调。没有新增测试框架或跳过原有行为断言。

最终执行 `tests/verify.sh`，所有构建启用 Qt、Debug、`-j12`，继承 libpq 环境：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| 正常 `build` | PASS | 14/14 PASS | 53.02 s |
| ASan `build/asan` | PASS | 14/14 PASS | 69.83 s |
| UBSan `build/ubsan` | PASS | 14/14 PASS | 67.21 s |

未使用 suppression 或测试排除；`git diff --check` PASS。静音标识及正常未读截图已检查。历史滚动回归先等待前一条消息的异步滚动和服务端阅读确认，再模拟查看历史，保持通知、位置和真实读位断言。无调试打印、临时 schema 或未使用状态。下一阶段为服务端个人会话 pin 和正确的 cursor 排序。


## 会话置顶

从重新 fetch 后的 `323c9f9`、干净工作树开始。SQL 018 无损增加 `conversation_members.pinned BOOLEAN NOT NULL DEFAULT false`。置顶属于当前用户 membership，只影响本人列表；不改变消息活动时间、已读或未读，不向其他成员广播。退出或被移除删除 membership，再邀请从未置顶开始。

`set_conversation_pinned {conversation,pinned}` 明确设置或取消，返回 `{pinned}`，重复设置幂等。mute/pin 保持两个明确 RPC，共用现有偏好处理文件中的事务和 conversation 行锁；锁后检查本人当前 membership。未认证、非成员及非法参数拒绝，事务错误关闭连接，权限失败回滚。

`get_conversations` 的筛选、page/visible 排序、JSON 聚合和 next cursor 统一采用 `(pinned,activity,id)`，三个字段都进入 cursor。每页仍为 50，置顶层在前，各层内按 activity/id 降序，最后一条 visible 决定 next。协议严格要求 bool pinned，不保留旧二元 cursor 的兼容路径。继续使用现有实时列表分页，不引入跨请求快照：遍历期间有活动变化时，后续权威刷新恢复；本人 pin 确认会使旧列表遍历失效并重新从第一页获取。

Qt 保留 bridge 全页聚合和服务端顺序，右键提供“置顶/取消置顶”，标题旁显示轻量标识；不在本地重排第一页。菜单捕获稳定会话 ID 和明确目标 bool，列表在菜单打开期间重置后仍能正确执行。确认结果和重连快照复用现有模型及 generation，不增加 pending pin 或额外缓存状态。

回归覆盖默认迁移、严格 RPC/SDK 元数据与 cursor 校验、个人单聊/群聊偏好、重复设置、登录/重连、移除/再邀请及实际会话锁竞争。123 个测试会话覆盖多页置顶、置顶到普通层的边界、相同 activity 的 ID 排序、全部分页不重不漏及取消置顶。真实三个 Qt 窗口覆盖菜单期间真实消息导致 modelReset、其他成员隔离、重连、新普通消息不挤掉置顶以及取消后恢复活动排序；截图已检查。

最终沿用 `tests/verify.sh` 完整验证：

| 构建 | 完整 build | 完整 CTest |
|---|---|---|
| normal Debug | PASS，`-j12` | 14/14 PASS，60.28 s |
| ASan | PASS，`-j12` | 14/14 PASS，72.10 s |
| UBSan | PASS，`-j12` | 14/14 PASS，69.19 s |

没有 suppression、测试排除或调试代码，`git diff --check` PASS。两项独立只读审查未发现确定性缺陷。下一阶段为持久化群 @mention；本阶段没有加入群内置顶消息或其他产品能力。

## 群成员提及

从重新 fetch 后的 `638b775` 开始，SQL 019 增加独立目标关系，SQL 001–018 保持原样。方案见 [提及设计](mentions-design.md)。当前用户名允许 Unicode、空格和标点，解析使用当前成员完整用户名的字面、区分大小写匹配，最长姓名优先；不把邮箱、词内 substring、`@@` 或已退出成员作为新目标。只解析群文字，单聊和附件文件名不产生提及。

发送、编辑、删除与成员变化继续共享 conversation 行锁。正文和目标同事务提交；重复提及去重，编辑替换，删除清空。退出不抹掉历史目标，再邀请恢复普通 membership 和最新加入水位。提及 metadata 导致完整消息超过既有 64 KiB 上限时，正文、目标和会话活动更新全部回滚。

SDK 严格校验 metadata，Qt 共用消息模型和 delegate，正文高亮并对目标本人显示“提及你”。历史、搜索、自己的发送确认、实时更新和重连均使用服务器保存的目标；不依赖客户端重新解析正文作为事实。发送、编辑和删除的旧连接回调现在在 Qt 线程复核现有 connection generation，没有新增状态计数器。静音仍抑制包括提及在内的全部桌面通知，实际阅读和普通未读保持。

回归覆盖迁移默认值与级联、不能伪造目标、重复/非成员/大小写/词边界、前缀姓名、Unicode、空格和正则符号、编辑删除、扩张 payload 回滚、退出再加入、分页及重连。现有 send/invite 真实锁竞争同时验证目标与加入水位一致。SDK 增加 malformed metadata 测试；Qt 模型覆盖 stale history 和删除，多行 delegate 检查高亮像素，三个真实窗口验证本人提示、静音、搜索和重连；截图已检查。普通编辑 SQL 漏投影新字段的问题已修复，并恢复新群测试所需的联系人前提，没有放宽原有断言。

最终实际运行 `tests/verify.sh`，三套均为 Qt ON、Debug、`-j12`，继承 libpq 环境：

| 构建 | 完整 build | 完整 CTest |
|---|---|---|
| normal | PASS | 14/14 PASS，56.13 s |
| ASan | PASS | 14/14 PASS，71.53 s |
| UBSan | PASS | 14/14 PASS，71.18 s |

无 suppression 或测试排除，`git diff --check` PASS。两项独立只读审查之后，以 PostgreSQL 17 的实际匹配结果及前缀姓名回归核实最长匹配语义，不依赖候选聚合顺序。没有新增通用事件框架、shadow state、临时 migration 或调试代码。下一阶段为群内单条置顶消息。

## 群内置顶消息

从重新 fetch 后的 `a653634`、干净工作树开始。SQL 020 增加可空消息引用，限定只有 group 可持有；旧会话默认没有置顶，不修改 SQL 001–019。每群只保留一个当前目标，不保存历史。群主和管理员可置顶任何属于该群的未删除消息，普通成员只能查看。目标替换和重复设置采用明确幂等语义，不改变活动时间、普通未读、实际阅读或个人会话排序。

`pin_group_message {conversation,message}` 和 `unpin_group_message {conversation}` 返回 `{changed}`；参数、认证、当前成员、管理权限和目标归属均由服务器检查。复用群管理的 conversation 行锁和事务，锁后查询角色，与移除、转让、管理员变更、发送和删除串行化。成功变化通过现有 conversation notification 刷新，操作者由 RPC 确认后刷新。删除在原消息更新 SQL 的同一事务中清除置顶，提交后发送会话变化；数据库物理删除通过外键 SET NULL 清理。

`get_conversations.pinned_message` 为可空引用摘要，包含真实消息 ID、作者、当前文字前 160 字及编辑状态；不混入 latest message 或 message 的持久化字段。历史和重连从同一会话快照恢复。现有 `message_updated` 也刷新会话快照，置顶摘要跟随编辑。SDK 拒绝缺失、错误类型、非法 ID、deleted 或 direct conversation 的非空置顶摘要。

Qt 消息菜单按当前成员角色显示“置顶消息/取消置顶消息”，使用稳定消息、会话 ID，触发时重新检查当前角色和连接。header 显示摘要；点击已加载目标滚动定位，较早的目标复用现有搜索窗口。置顶栏直接读取已有 conversation model，没有另存 active pinned ID、pending 状态或新增 generation。RPC 回调在 Qt 线程复核现有连接 generation，重连和账号切换的旧结果不会写入新状态。

回归覆盖 SQL 019→020 的空默认值、群类型约束和物理删除清理；未认证、非法参数、非成员、普通成员、跨群及删除消息拒绝；owner/admin 设置、替换、幂等和取消；摘要与 latest/unread 独立、编辑、登录重连，以及 pin/delete 真实锁竞争。SDK 验证摘要和动作的异常协议，Qt 模型验证当前角色和摘要数据，三个真实窗口验证菜单权限、实时置顶栏、取消、编辑刷新、转让后的管理角色、重连、删除自动解除和 stale callback；截图已检查。

实际持久化超过一页的历史验证了未加载目标走搜索。补充的空白首行用例在修复前失败，预填现改用首个非空行（最多 80 个 Qt 字符，满足搜索参数字节边界）；如果正文全为空白，仍提供搜索窗口手动输入。没有新增随机访问 RPC、置顶历史或索引框架。

最终代码运行 `tests/verify.sh`，三套启用 Qt、Debug、`-j12` 并继承 libpq 环境：

| 构建 | 完整 build | 完整 CTest |
|---|---|---|
| normal | PASS | 14/14 PASS，59.24 s |
| ASan | PASS | 14/14 PASS，82.69 s |
| UBSan | PASS | 14/14 PASS，74.50 s |

无 suppression 或测试排除，`git diff --check` PASS。独立审查建议已通过实际代码和回归复核，真实空白首行问题已修复。没有新缓存、事件框架、临时 schema 或调试代码。下一阶段为群公告。

## 群公告

从重新 fetch 后的 `31d6cc4`、干净工作树开始。SQL 021 为会话无损增加一个当前 `announcement TEXT NOT NULL DEFAULT ''`；原有单聊、群、消息、成员和阅读位置保持。只允许 group 保存非空公告，数据库与服务端按 UTF-8 字节限制为 4096；Qt 保存按钮使用同一字节边界。纯文本保留换行和字面符号，空字符串清除，不保存历史、作者或修改时间，不生成消息、改变活动时间或累计未读。

`set_group_announcement {conversation,text}` 返回 `{changed}`；群主和管理员可修改、清除，普通成员仅查看。缺失、null、错误类型、未知字段、NUL 和超限内容拒绝。事务取得现有 conversation 行锁后重新检查 membership 和角色，与任免、移除、转让及消息写入串行化。重复保存和重复清除幂等且不通知；退出分支改为显式 `leaving`，新增动作不会落入成员删除路径。提交后沿用现有 `conversation` notification，移除者不再收到群变化。

`get_conversations.announcement` 是必需字符串，SDK 校验格式、大小和 direct 必须为空；没有新消息字段或公告 RPC 查询框架。Qt 会话模型保存同一权威资料，群资料窗口提供纯文本查看、保存和清空。复用已有 pending 状态和 connection/conversation generation，响应在 Qt 线程检查连接，旧连接结果不能刷新新账号。原有群资料窗口关闭及成员移除清理路径保持。

公告编辑框是未保存草稿，`announcement_` 是当前权威比较基线，不新增 dirty 字段。另一位管理员更新公告时保留草稿并更新基线；失去管理权限后回到权威公告。主动清空同时清理未保存草稿。独立审查发现的草稿覆盖用例在修复前失败，修复后通过；真实窗口测试另外修正了主窗口与对话框排队快照的观察时序，等待对话框也确认保存，保留保存和清空断言。

ASan 前置 UI 回归暴露 typing 段的观察时序：先前只等待一个查看窗口的旧提示过期。现等待两个查看窗口均完成上一段状态，再检查新开始、idle 停止及草稿保留，未扩大该段超时或移除断言。定向跟踪确认停止请求约在新输入后三秒发送，临时跟踪已删除。一次验证被 SIGTERM 中断，未记为通过，后续完整重跑。

回归覆盖 SQL 020→021 的已有单聊/群默认值、direct 约束及大小限制；认证、参数、成员及管理权限；owner/admin 保存、替换、重复和清空；message/unread/membership 不变；在线变化、离线更新及重新登录恢复；被移除者权限和通知隔离；公告写入与管理员降权的真实锁竞争。SDK 覆盖 malformed metadata 和动作结果；Qt 覆盖模型资料、旧连接回调、草稿及权限转换，三个真实窗口验证成员只读、已经打开的窗口实时更新、UTF-8 边界、转让后管理员编辑、清空、重连和没有额外桌面通知。实际群资料截图已检查。

最终代码沿用 `tests/verify.sh` 的配置和命令，三套均为 Qt ON、Debug、`-j12`，完整 CTest 顺序运行并继承 libpq 环境：

| 构建 | 完整 build | 完整 CTest |
|---|---|---|
| normal | PASS | 14/14 PASS，60.31 s |
| ASan | PASS | 14/14 PASS，74.44 s |
| UBSan | PASS | 14/14 PASS，70.15 s |

无 suppression 或测试排除，`git diff --check` PASS。没有修改 SQL 001–020，没有新增 shadow state、通用公告管理框架、调试输出或临时 migration。下一阶段为单个高熵、可撤销的群邀请链接。

## 群邀请链接

从重新 fetch 后的 `d6c0952` 开始。SQL 022 无损增加一个当前可空 invite token，不修改 SQL 001–021。服务端用现有 OpenSSL `RAND_bytes` 生成 32 个随机字节，编码为 64 位小写十六进制；唯一约束支持查找，群类型和格式受数据库约束。token 作为 opaque secret 保存，只有当前群主、管理员能查询，普通会话、历史、成员资料及 notification 不携带它。

`get_group_invite/create_group_invite/revoke_group_invite {conversation}` 返回 `{token:null|string}`。创建已有链接时返回同一 secret，撤销重复执行仍返回 null；撤销后重新创建产生新 secret。`join_group {token}` 要求认证，允许非联系人使用有效链接，返回权威会话 ID、群名、成员数量及 `joined/member` 状态。已有成员幂等保留角色、真实阅读位置、加入水位及个人 mute/pin。新加入或重新加入使用当前群消息水位、普通成员身份、实际读位 0 和默认偏好；旧历史可见且不计未读。

创建、查看、撤销和加入均复用 conversation 行锁。加入先通过唯一 token 定位，再在取得群锁后复核当前 token，不能凭已撤销的旧查询结果插入成员。管理操作锁后重新检查当前 membership 和角色，与降权、移除、转让、退出及发送串行化。提交后用现有 conversation 变化通知刷新当前成员；没有全局广播、订阅或事件框架。仅真实变化通知，不改变 message、activity 或阅读位置。

Qt 群资料按当前角色显示链接、创建、复制和撤销，成员列表保留实际空间。侧栏“加入群”接收完整 `chat://join/<token>`，只解析这一格式；复制和粘贴是普通文本入口，没有操作系统 URL 注册或 deep-link 框架。成功加入使用服务器返回的群名和成员数打开历史，再刷新权威列表；无占位群名、pending join ID 或链接缓存状态。移除不是永久封禁，持有仍有效链接的用户可以重新加入；不再允许该链接加入时应由管理员撤销。

新增旧连接成员回调回归在修复前失败：`get_members` 原来直接 emit，排队结果可能在断线后重新启用群管理。现于 Qt 线程复核已有 connection generation，再交给成员模型和资料窗口；链接与加入使用相同防过期方式。没有增加 generation 字段。AutoConnection 按实际发射线程与接收线程判断，跨线程会排队，同线程避免再次排队，依据见 [Qt 线程与信号](https://doc.qt.io/qt-6/threads-qobject.html)。同一 session 的 RPC 逐个 await 并按顺序响应，链接查询和修改不会反序覆盖，因此不增加冗余链接 loading 状态。

回归覆盖 SQL 021→022 的既有单聊/群默认值、group-only、token 格式和唯一性；未认证、非法参数、普通成员、非成员和单聊拒绝；owner/admin 查看、稳定创建、撤销与重建；非联系人加入、旧历史、实时新消息、真实读位及幂等角色/偏好保留；移除后的普通身份重新加入和登录恢复；管理员降权/链接、撤销/加入、加入/发送的真实会话锁竞争。SDK 检查四个 RPC 的结果和异常协议。三个真实 Qt 窗口完成创建、复制、管理员查看与撤销、移除后粘贴加入、旧链接拒绝、重新创建及转让/重连恢复；资料截图已检查。测试收尾的断线次数随新增重连用例更新，原生命周期断言保持。

最终代码沿用 `tests/verify.sh` 的配置和命令，三套均启用 Qt、Debug、`-j12`，完整 CTest 继承 libpq 环境顺序运行：

| 构建 | 完整 build | 完整 CTest |
|---|---|---|
| normal | PASS | 14/14 PASS，66.01 s |
| ASan | PASS | 14/14 PASS，80.12 s |
| UBSan | PASS | 14/14 PASS，77.08 s |

无 suppression 或测试排除，`git diff --check` PASS。没有修改 SQL 001–021，没有临时 migration、调试输出、链接历史或无需求抽象。下一阶段为邀请链接上的最小入群审批。

## 群聊入群审批

从重新 fetch 后的 `fbdb06b` 开始。SQL 023 无损增加 group-only 的 `join_approval`，已有群默认 false，保持链接直接加入；群主、管理员可切换为审批模式。独立 `group_join_requests` 只保存 `(conversation_id,user_id,created_at)` 的唯一待处理关系，外键级联清理，不保存审批历史、留言或结束状态。

启用时，非成员使用当前有效链接得到 `pending`，重复申请保持同一创建时间；不会创建 membership、增加成员数或获得历史、搜索、成员、消息、附件及阅读权限。已有成员仍返回 `member` 并保留角色和偏好。显式联系人邀请继续直接加入，并在同一事务清除对应 pending；撤销链接不删除已经提交的申请。关闭审批不批量批准，申请人随后使用当前直接链接加入时原子清除自己的 pending。

`set_group_join_approval/get_group_join_requests/respond_group_join_request` 仅允许当前群主、管理员。申请列表使用 user ID 降序 cursor，每页 50 条，返回申请人当前头像 metadata 和创建时间；无 offset 或任意申请数量上限。接受原子删除申请并创建普通 membership，加入水位取锁后最新消息 ID，真实读位 0、非管理员、个人 mute/pin 默认 false；旧历史可见且不计未读。拒绝只删除 pending，重复决定返回 `changed:false`。申请、审批、配置、权限变化及发送继续共用 conversation 行锁。

`join_request {conversation,user,state:pending|accepted|rejected}` 只通知当前群主、管理员和申请人，收件人去重；普通成员不接收私有申请。它是当前关系的变化提示：发布时复核 pending/membership，抑制已被后续变化取代的旧提示，不引入事件日志或版本框架。接受及联系人邀请还通过权威 `conversation` 快照恢复成员资格。离线恢复不回放审批事件或保存决定历史；管理员重新加载 pending，申请人从会话列表恢复已接受的群，或再次使用有效链接取得当前加入结果。

Qt 群资料包含审批开关和独立“入群申请”页，显示姓名、统一头像、通过/拒绝和加载更多。首页刷新立即失效旧游标，阻止刷新期间从旧 cursor 加页；同一 session 按序处理并回传 RPC，复用现有 pending 和连接 generation，没有新增分页代次框架。角色丢失清空私有申请并拒绝迟到结果。`pending` 回复只给轻量提示，绝不打开会话或注入虚构列表项；新 RPC 和 notification 在 Qt 线程校验连接 generation，关闭窗口由 QObject context 清理回调。

新增回归覆盖 SQL 022→023 的既有用户/单聊/群默认值、group-only 开关、pending 唯一性及级联；未认证、非法参数、普通成员/非成员权限；重复申请、隔离、私有通知、接受/拒绝、真实读位/未读、待审批重连、撤销/切换模式和显式联系人邀请；58 个申请人的完整 cursor 遍历；审批与降权/发送、重复接受和接受/拒绝的真实锁竞争。SDK 验证三个 RPC、metadata、pending 结果和 notification 的异常协议。三个真实 Qt 窗口完成配置、重复申请、管理员拒绝、待审批状态服务器重启、群主接受和历史恢复；申请页截图已检查。

Qt 重启测试先确认申请已在服务器持久化，再重启，避免把尚未完成的 RPC 当作成功提交；瞬时状态文字可能被其他快照刷新覆盖，不用它代替数据库事实。新的 modal 测试有单步超时。已删除临时诊断输出，未改 SQL 001–022。完成本阶段后进入最终综合审查，不增加其他产品功能。

最终实际执行 `tests/verify.sh`，三套均启用 Qt、Debug、`-j12`，完整 CTest 顺序继承 libpq 环境：

| 构建 | 完整 build | 完整 CTest |
|---|---|---|
| normal | PASS | 14/14 PASS，66.04 s |
| ASan | PASS | 14/14 PASS，84.30 s |
| UBSan | PASS | 14/14 PASS，77.46 s |

首次完整入口受到 SIGTERM 中断，没有计作成功；重新完整运行后通过。未使用 suppression、测试排除或扩大现有测试超时，`git diff --check` PASS。

## 最终综合审查

产品阶段推送后重新 fetch，审查基线为 `HEAD = origin/main = 93d8dc3bad218fb8556e40448b95c828359159fa`，工作树干净。审查覆盖整个开发历史、SQL 001–023、数据库模型、server/client/Qt 生命周期、权限与事务锁、协议和通知、cursor、缓存及测试。SQL 编号连续；本 Goal 新增 SQL 016–023，原有 SQL 001–015 未修改，最终收口没有新 migration。

修复了以下实际问题：

- 部分旧 RPC 以及 read/typing/conversation/presence 通知原来直接发射 Qt 信号，排队结果可能进入新连接。现在复用既有 connection generation，在 Qt 线程交付前检查；主窗口使用 AutoConnection，已在 Qt 线程校验的结果不再次排队。跨线程生命周期信号仍按 Qt 规则排队，没有新增 generation 或业务状态。旧连接回归在修复前失败。
- 群移除或消息删除原来立即移除图片 active 项，但 SDK 下载仍在途，既可能突破三个任务的限制，也可能把旧权限错误当成重新加入后的新下载结果。现在保留在途名额并标记结果不再需要，完成后丢弃内容再启动同消息的后续下载。三个在途任务和单个同 ID 重试两种回归均通过，没有下载版本框架。
- SDK 在自己的 connected、RPC 或 message 回调中被析构时，原关闭流程会 join 当前网络线程并抛出 `Resource deadlock avoided`。这个失败已复现；现在立即抑制后续回调，将内部状态交给清理线程，在当前回调返回后执行原有 shutdown/join。通常由调用线程析构的同步行为保持，三个公共回调回归确认内部所有权最终释放。
- PostgreSQL 连接池取得连接时，如异步连接失败，显式关闭该连接，避免保留 libpq 已连接但等待句柄未就绪的状态，下一次取得可以重新连接。现有连接、事务失败恢复和连接池复用测试完整通过；未对操作系统句柄分配失败做故障注入。
- Qt UI 测试的 RPC helper 改为按值捕获共享 promise，超时返回后迟到 callback 不再引用已经销毁的栈对象。没有新增测试配置框架或扩大超时。

其他审查结论：

- PostgreSQL 仍是事实来源。群权限及 membership 关键变化在 conversation 行锁之后校验，发送、邀请、转让、移除、审批与对应读写串行化；发布阶段重新查询当前收件人。头像和附件分别存储，未持久化 incomplete upload。
- `joined_message_id` 仅是未读基线，真实阅读只来自 `last_read_message_id`。群已读人数和详情计算当前其他成员的读位，退出者不计入；不维护永久逐消息 receipt 表。
- 个人 pinned 的服务端 `(pinned,activity,id)` cursor 保持完整分页，群内 pinned message 与个人排序独立。reaction revision、头像 revision 和现有连接/查询 generation 各自有真实职责；没有发现需要机械删除的 shadow state，也未按文件行数拆分模块。
- 新会话资料通过权威快照恢复；实时通知包括 `message/message_updated/read/typing/presence/conversation/avatar/reaction/join_request`。mute 只抑制桌面通知（包括 mention），不抑制消息、未读或实时快照。审批通知只到当前管理者和申请人；离线没有审批决定历史或事件回放。
- Qt 图片和头像在异步数据到达时解码并缓存，delegate paint 不下载、不重复解码。图片缓存 64 MiB、最多三个在途下载；头像最多 1 MiB，附件最多 10 MiB，两者使用 32 KiB chunk。图片预览限制 16 × 1024 × 1024 像素，普通文件下载路径保持。

最终实际运行 `tests/verify.sh`，三套均 Qt ON、Debug、`-j12`，继承 libpq 环境且完整 CTest 顺序执行：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| normal | PASS | 14/14 PASS | 68.13 s |
| ASan | PASS | 14/14 PASS | 92.74 s |
| UBSan | PASS | 14/14 PASS | 80.87 s |

没有 suppression、排除测试或放宽断言。最终 `git diff --check` PASS。14 个测试入口为 migration、client、pg_connection、corosio_timeout、http_server、websocket、server、avatar_image、simdjson_reflection、online_users、bcrypt、qt_models、qt_delegate 和 qt_ui；功能回归扩展这些既有入口，未为覆盖率制造重复测试。完整集成回归涵盖 connect/disconnect/reconnect、并发发送和成员变更、编辑删除、回应、阅读、输入、附件头像、移除与转让、邀请重入、通知与静音、提及、群置顶、公告、链接及审批；三个真实 Qt 窗口和截图验证继续执行。

最终 normal 的小群测试每种规模各取三个样本，平均端到端耗时如下。只有发送者在线，其余成员离线，因此它验证成员资料、完整读位和收件人查询规模，不代表 200 个在线连接的吞吐或同时通知能力：

| 成员数 | get_members | history（全部读位） | send（含发布查询/遍历） |
|---|---|---|---|
| 3 | 1.98 ms | 3.22 ms | 13.27 ms |
| 10 | 2.02 ms | 2.82 ms | 13.83 ms |
| 50 | 42.84 ms | 3.40 ms | 15.43 ms |
| 200 | 44.68 ms | 44.34 ms | 25.31 ms |

当前成员快照、读位返回和群通知 fanout 随成员数线性增长；已测范围 3–200 人，没有凭此新增人数上限或大群基础设施。GitHub Actions 仍未接入，当前 GCC 16 反射及 Boost 1.92 缺少固定、已验证的 hosted 工具链获取基线，可靠本地入口和限制见 [验证说明](verification.md)。桌面通知点击恢复窗口，Qt 公共接口未提供稳定的通知会话身份，未声称可精确跳到任意旧通知对应的会话。

本 Goal 至此停止。未实现且不属于本轮范围的能力包括多设备同步、E2EE、音视频、超大群、channel/broadcast、bot、分布式 presence、对象存储/CDN、Redis/Kafka、微服务、event sourcing 和 CQRS；需要另行确定真实需求。没有自动启动这些路线。

## 历史产品语义与授权审查：联系人和单聊（已由好友确认模型替代）

本轮重新 fetch 后的起点为 `HEAD = origin/main = 49bfa2941dd23aff3c50332d7a90b8d9cae9d592`，开始时工作树干净。修改前完整运行统一验证入口，normal、ASan、UBSan 均 14/14 PASS，分别 67.03、85.61、80.21 秒；新增非联系人 open 回归在修复前失败。本部分没有 schema migration，SQL 001–023 未修改。

`contacts(owner_id,contact_id)` 是操作者自己的单向主动 direct 通讯授权。A 添加 B 只授予 A→B，接收消息、对方添加自己、历史会话和共享群都不授予反向发送资格。get_contacts 只返回自己的关系，search_users 排除自己和自己的已有联系人。direct conversation 是历史容器，删除联系人不删除会话、成员、消息、读位、搜索或附件。

| direct 操作 | 自己仍持有 peer contact | 自己已无 peer contact |
|---|---|---|
| open direct、发送文字/新回复、附件 begin/finish、typing 开始/结束、reaction 添加/替换/清除、编辑自己的消息 | 允许，仍检查当前 membership/作者/目标 | 拒绝 |
| 历史及 cursor、历史搜索、附件/图片查看、公开头像和资料、mark_read、个人 mute/pin | 允许 | 允许，仍检查历史 membership |
| 删除自己过去发布的消息 | 允许 | 允许，仍检查作者和历史 membership |

direct mutations 在 conversation 行锁之后查询当前联系人的数据库事实。remove_contact 使用同一个 conversation 行锁后删除自己的关系；不存在已提交 pair 的创建竞争由 contact 行 `FOR KEY SHARE` 与 DELETE 串行化，拒绝的创建完整回滚。没有 server permission cache、关系 generation、全局 mutex 或 ACL 框架。附件 begin 和 finish 都检查，移除联系人清理自己的未完成上传；服务端 finish 也独立拒绝数据库关系已经撤销的在途上传并释放状态。

get_conversations 返回权威 `can_send`，包括已创建但无消息的真实 direct 容器；群当前成员为 true，direct 为自己是否仍持有 peer contact。open_direct 成功返回实际会话 ID 和 can_send:true，非联系人返回明确 domain error。SDK 使用独立结果 DTO 和严格解析，不保留旧 int 结果兼容接口。Qt 只用该快照决定 compose 和消息交互，历史及删除自己的旧内容保持；只读输入提示“对方不是你的联系人，添加联系人后可发送消息”。添加/删除联系人刷新联系人、presence 和会话；重连取得会话快照前不恢复缓存的发送资格。

Profile 的自己页面保留头像和复制用户名；联系人有消息及移除入口；非联系人有添加及复制入口。添加按钮等待 RPC 成功和权威联系人刷新后才变为消息，没有乐观赋权。联系人/搜索头像、消息作者、历史 direct peer 和群成员复用同一资料窗口。

presence/last_seen 只对观察者自己主动添加的联系人可见。get_presence 不包含历史 direct 或共享群；实时发布仅查 `contacts(contact_id=变化用户)` 对应的 owner，并在联系人行锁释放前入队，移除完成后的新发布不再包括该观察者。last_seen 先持久化，通知后查询不会读到提交前旧值。Qt 清理移除、断线和失败刷新的旧 presence。username/头像仍是公开资料，头像通知保留联系人关系、历史 direct 和共享群的相关范围。

回归覆盖四种 A→B/B→A 组合中双方的全部主动操作与历史权限、实际删除/重新添加和重连；三个用户的 presence 单向隐私与公开头像区分；数据库锁同步的发送/typing/reaction/edit 与关系撤销、新 pair 创建竞争；附件上传期间关系撤销及清理；已有多页置顶排序、群生命周期、三窗口 Profile 实际添加、非联系人只读和历史附件访问。Qt 在断线期间真实删除关系，再持有数据库锁延迟 reconnect snapshot，确认旧发送资格不会复活；随后在只读单聊通过实际菜单删除自己的旧附件。

本阶段实际完整运行 `tests/verify.sh`：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| normal | PASS | 14/14 PASS | 74.08 s |
| ASan | PASS | 14/14 PASS | 95.55 s |
| UBSan | PASS | 14/14 PASS | 88.85 s |

首次 ASan 复验揭示旧测试固定要求“群移除与 typing 竞争时 typing 必须成功”，该断言与移除先取得锁后的权限失效冲突。已改为检查实际锁顺序对应的成功或权限拒绝以及通知隔离，然后重新完整执行三套验证。没有 suppression、测试排除或增加超时，`git diff --check` PASS。

## 产品语义与授权审查：消息、身份及群状态

阶段起点重新 fetch 确认 `HEAD = origin/main = 3f1acef633855726a69b7e6844b7f1e73231c029`，工作树干净。本轮没有新增产品功能，以下规则由数据库、server、SDK、Qt 和回归共同表达。

- 新回复的目标必须属于当前 conversation 且 `NOT deleted`。目标查询移到发送事务的 conversation 行锁之后，文字和附件 finish 共用这条路径；删除和回复按取得锁的顺序执行。已经存在的 reply 保留真实 ID，原文删除后历史、实时恢复和 Qt 显示“消息已删除”。拒绝新回复时回滚整个发送，不留消息、附件或 activity 变化。
- 用户名为 1–64 UTF-8 字节，保留中文及合法 Unicode、内部 whitespace、`.`、`-`、`_`、其他正常标点及大小写敏感身份。首尾不能是 Unicode White_Space。拒绝非法 UTF-8、纯 Unicode White_Space、`@`、NUL/C0/C1、U+2028/U+2029 和显式 Bidi_Control；不 trim、归一化或自动改名。server 与 Qt 共用 `chat/text.hpp`，复用现有 Boost header-only UTF 解码，SQL 024 保留已有 CHECK，SQL 025 按同一 Unicode 空白集合增加首尾约束。具体规则及 Unicode/正则依据见 [提及设计](mentions-design.md)。
- SQL 025 前实际扫描现有 46 个用户：首空白、尾空白及总冲突均为 0；应用 024→025 后核对全部既有 user ID/username 快照不变，没有改名、trim、归一化或删除历史身份。SQL 001–024 保持不变，新增 CHECK 复用 SQL 024 的 25 个 Unicode White_Space 字符；实测 `btrim(text, characters)` 去除集合内首尾字符并保留内部空白。
- SQL 024 前实际扫描测试库：47 个既有用户中，空白、`@`、控制/方向字符冲突为 0；唯一超长项精确匹配旧测试的 2000 字符尾缀、fixture 前缀和固定测试 hash。仅清理这个已经核实的测试遗留项，没有改名、截断或删除真实身份。原测试改为合法的 64 字节最长 username，并继续验证提及扩展后完整消息超过 64 KiB 时原子回滚。群标题扫描 138 条，无纯空白或超长冲突；测试库 SQL 023→024 成功，隔离 schema 的 migration 回归还验证既有中文/空格身份和历史不被改变。
- group title 统一为不含 NUL、不超过 256 UTF-8 字节、不能全是 Unicode 空白。create/rename 和 Qt 使用相同规则，有意义的前后、内嵌空格原样保存。公告空字符串或纯 Unicode 空白表示 clear；有意义的内容不 trim，仍为 4096 字节上限和纯文本。
- clear_avatar 在用户行锁之后读取当前状态：已经无头像则返回当前 revision，不递增、不发通知；实际清除递增，重新上传继续递增。头像仍为公开资料，和 presence 的隐私边界分离，缓存没有 ABA；见 [头像设计](avatar-design.md)。

全局审查的前置、事实变化、依赖和恢复语义如下。持久化业务以 PostgreSQL 为准，mutation RPC 独立检查；Qt 快照和 generation 只控制显示与异步交付。

| 能力 | 前置与事实变化 | 后置、撤销和重连；并发边界 |
|---|---|---|
| 群内 pinned message | owner/admin；同群未删除消息；每群一个当前 ID | conversation 刷新摘要，编辑同步；删除自动清空；同一 conversation 行锁 |
| 个人 conversation pin | 当前历史 membership；仅自己的 pinned | 不改 message activity；服务端 `(pinned,activity,id)` cursor，分页和重连保持；conversation 行锁 |
| mute | 当前历史 membership；仅自己的 muted | 只抑制桌面通知，包括 mention；消息、未读、实时不变，重新登录恢复；conversation 行锁 |
| 桌面通知 | 新的他人实时消息，非实际活动阅读、未 mute | 文字/图片/文件摘要；编辑、删除和 reconnect history 不作为新消息通知；离线不回放一串通知 |
| reaction | 群当前 member，direct 自己 contact；未删除消息，每人一条 | 聚合/revision 持久化及实时恢复；删消息清空，删 contact 保留历史但不能再改；conversation 行锁 |
| mention | 群当前成员完整字面 username；由 server 解析真实 user ID | 正文和目标原子保存，编辑替换、删除清空；退出者不能成为新目标，旧事实保留；conversation 行锁 |
| attachment / image | begin/finish 都检查当前发送资格；session 未完成上传 | 10 MiB/32 KiB；历史 membership 可下载，direct 删 contact 不删文件；group 移除失去访问；finish 与 membership/contact 变化共享锁 |
| avatar | 认证用户仅改自己；认证用户可读公开头像 | 独立表、1 MiB/32 KiB、单调 revision；相关用户更新，旧 callback 不覆盖；用户行锁与原子数据事务 |
| typing | 群当前 member，direct 自己 contact；true/false 都检查 | 不持久化；接收过期、切换和断线清理；发送范围在 conversation 锁内复核 |
| read/unread / 已读详情 | 当前历史 membership；同会话真实 message ID | member 行 GREATEST 单调阅读；joined 仅未读基线，deleted 不计未读；详情只计当前其他成员真实读位，重连取权威快照 |
| reply | 当前发送资格；同会话未删除目标 | 新回复和删除共用 conversation 行锁；旧 reply 在删除后保持 ID 和占位 |
| edit/delete | 作者及当前 membership；direct 编辑另需自己 contact，历史删除不需 contact | 编辑时间单调；软删保持 ID，清正文/reaction/mention/附件/群 pin；不增加 unread；conversation 行锁 |
| search/history | 当前历史 membership；direct 不需 contact | cursor 分页，当前删除/引用状态；被移出群后拒绝，重新邀请后旧历史可见但不计初始未读 |
| group roles / transfer | owner 任免最多三 admin；转让目标必须是当前 admin | 新 owner 的 is_admin=false，旧 owner 降 admin，admin 数不增加；转让后旧 owner 可退出；权限在 conversation 行锁后读取 |
| invite/remove/leave/rejoin | 手工 owner/admin invite 要求操作者自己的 contacts；owner 可移 admin/member，admin 只移 member；owner 不能直接退出/移自己 | 移除者收到一次会话变化、Qt 清当前会话及交互状态；后续全部群访问/实时隔离；再邀请是普通 member、read=0、最新 joined 水位；conversation 行锁 |
| announcement | 当前 member 可读，仅 owner/admin set/clear | 独立群字段，不是 message；不改 unread/activity，不进入 search/reply/reaction；conversation 权威刷新及锁后权限 |
| invite link | owner/admin 单个 256-bit 随机 opaque token；authenticated 加入，无 contact 要求 | 锁后复核 token；revoke 旧链接不能发起新 join/request，已提交 pending 保留；remove 不等于 ban，不自动产生联系人 |
| join approval | link 按当前模式 direct/pending；手工 invite 始终直接加入 | pending 唯一，非 member 无群访问；accept 锁内消费 request、当前水位新 membership，reject 仅消费；申请和结果只到管理者/申请人，重连取实际关系 |

新增回归覆盖已删除目标的新文字/附件回复、真实数据库锁排队的 delete-first/reply-first RPC 两种顺序、旧引用占位、初始与重复无头像 clear 无广播及重新上传不复用缓存键；正式 username 的 server/Qt/DB 规则、合法中文空格标点/64 字节边界、非法控制/NUL/方向字符/字节超限；群名 create/rename 空白和内容保持、公告空白 clear；撤销链接后旧 pending 独立保留。原有 direct 四组合、群权限/加入/移除/转让、pin、mute/通知、提及、阅读、缓存和生命周期回归保留。

延伸检查还复现了 Qt 引用合并缺陷：已加载原文的删除更新先到、带旧 quote 的新回复或分页结果后到时，旧正文会重新显示。回归在修复前失败；现在新插入和批量合并都用已有原文的 deleted/edited_at 单调合并 quote，晚到引用不恢复已删除内容或旧编辑。不保存额外 tombstone/cache/generation，不在 paint 查询网络；原文尚未加载时继续使用服务端 quote，历史及重连从权威查询恢复。

原文在撰写回复期间被删除时，Qt 立即取消这个已经失效的待发送引用，保留用户尚未发送的文字草稿；已发布的旧回复仍保留删除占位。真实三窗口测试先选中待回复消息，再通过作者菜单删除，验证引用栏关闭、草稿不丢失以及之后发送成功。

全部源码收口后再次实际运行 `tests/verify.sh`，三套完整构建和全部既有 CTest 均通过：

| 构建 | 完整 build | 完整 CTest | 总耗时 |
|---|---|---|---|
| normal | PASS | 14/14 PASS | 72.79 s |
| ASan | PASS | 14/14 PASS | 92.32 s |
| UBSan | PASS | 14/14 PASS | 88.37 s |

没有 suppression、排除测试或放宽超时。SQL 001–023 未改，编号连续至 024；新增 helper 均有多个实际调用点，没有新权限状态、通用框架、兼容路径或调试代码，`git diff --check` PASS。本轮仅修复已经确定的不一致；完成独立提交和 push、确认远端与工作区后停止，不进入新产品开发。

## 用户名首尾空白收口

本轮基线 `ae59692bab1dfd9b1cec586a8d66e190ff5486ac`，开始时工作树干净且 `HEAD == origin/main`。先扩展现有 helper、server 注册和 Qt 注册测试，三个入口均确认 RED，再修改共享校验；首尾 Unicode whitespace 被拒绝，内部 ASCII/NBSP/U+3000 及中文继续合法。注册拒绝返回参数错误且不落库，Qt 显示首尾空白规则。搜索继续使用原有 trimmed query 和 prefix match，真实注册/搜索及现有 mention 的最长匹配、大小写、邮箱/`@@` 边界、Unicode 和字面特殊字符回归通过。

SQL 025 的迁移及 insert/update 回归验证合法旧身份原样保留，并保留 SQL 024 的 `@`、control、纯空白和字节上限约束。定向测试 4/4 PASS；随后实际完整运行 `tests/verify.sh`，normal、ASan、UBSan 均完成构建和全部 14/14 CTest，无 suppression、排除测试或放宽 timeout。`git diff --check` PASS。

## 终端 TUI 客户端

以 `545f1dbd0ff3e5fa7ed1eee4e5a955f7d5668865` 为本轮基线，新增 C++26 `chat_tui`，通过 `CHAT_BUILD_TUI_CLIENT`（默认 OFF）构建。FTXUI 以 `third/ftxui` submodule 固定到 v7.0.3 / `f921fad208912747c17d129a8ef75ec7624b6eec`，不使用浮动分支或配置时联网下载。TUI 直接调用 `chat::client`，不依赖 Qt，不新增协议实现。

```sh
git submodule update --init --recursive
cmake -S . -B build -DCHAT_BUILD_QT_CLIENT=ON -DCHAT_BUILD_TUI_CLIENT=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j12
./build/chat_tui ws://127.0.0.1:18080/ws
```

不传 URL 时使用上述默认值，也可在登录页修改。用户名与群标题复用现有校验；密码隐藏输入、只保留在进程内存，退出登录清除。`--help`、`--version` 不进入全屏。

支持会话 cursor 分页、置顶/静音/未读、用户搜索、好友申请/接受/拒绝/取消/移除、已确认好友双向 direct 权限、好友 presence、消息历史/收发/回复/编辑/确认删除/reaction/已读/typing/搜索及服务端 mention；附件路径上传与显式路径保存；自己/联系人/非联系人资料和头像上传清除；群创建/邀请/成员角色/群主转让/移除/退出/重命名、公告、群置顶、邀请链接、加入审批及申请分页。权限呈现使用权威 `can_send` 和当前成员角色，server 仍是最终授权边界。

100 列及以上使用双栏，40–99 列使用列表与会话页面切换；小于 40 列或 12 行显示尺寸提示。`h/c/u` 打开 Chats/Contacts/Account，`N` 打开 New；`j/k`、方向键、Enter、Esc、Tab 切换和选择，`i` 输入，`:` 打开命令输入，`?` 显示真实键位及命令，Ctrl+C 安全退出。完整键位以程序内帮助为准。中文、emoji 和长单行使用 FTXUI 的字符宽度；长消息可逐行浏览。向上浏览历史或长消息、进入其他页面、终端过小时不自动标记最新消息已读。

SDK callback 只捕获值并投递线程安全 inbox，再用 FTXUI `PostEvent(Custom)` 唤醒 UI；所有快照、页面和组件变更在 UI 线程执行。一个阻塞 deadline worker 负责 typing 和 1/2/4/8/15 秒重连退避，空闲不由应用轮询。连接/session 与页面上下文拒绝过期回调；分页保留服务端顺序和真实 cursor。关闭先停止接收队列，再取消并在 UI 线程析构 SDK、join 网络与定时线程，最后退出 FTXUI loop。

重连重新认证，刷新联系人/presence、已加载会话列表范围、当前最新一页消息和群元数据；保留当前会话及草稿，清除 typing、reply/edit、临时页面/弹窗与上传状态。断线期间已被移出群时回到会话列表。附件上传不会重放；发送或保存失败显示 status，不擅自清除草稿。只在当前会话最新区域标记已读，群已读人数只统计当前成员。

终端限制：附件最多 10 MiB，只按文件路径选择；保存使用排他创建，拒绝覆盖已有文件。图片仅显示文件信息，无 inline graphics 或外部 opener。头像只显示 Unicode 首字符 fallback 和 set/default 状态，不下载位图或建立头像缓存。复制采用可滚动的文本页和终端自身选择，无剪贴板重依赖、系统通知、默认响铃或磁盘凭据配置。

Qt bridge 的字符串转换、signal、页面焦点和 reconnect 呈现仍是 Qt glue。TUI 初始接入使用 SDK 原有 DTO，未为了少量快照合并规则增加跨 DTO 适配框架；后续好友确认需求增加 SDK 好友 API，当前联系人/presence 语义以本文顶部的好友确认模型为准。

TUI 测试入口包括 `tui_state`（权限、导航、单调消息合并与分页 cursor）、`tui_inbox`（跨线程投递、取消与关闭）、`tui_file`（路径、类型、大小和不覆盖保存）、`tui_render`（登录、群聊、只读 direct、Unicode、安全文本、宽窄屏和输入可见性），以及复用现有真实 server fixture 的 `tui_integration`。集成测试通过 SDK 驱动 TUI app，覆盖好友申请状态与删除后双向只读、消息交互、附件、头像、群角色/链接/审批、超过 50 条的真实分页、重连和异步退出；原有 Qt/server/client 测试继续运行。

TUI 专项审查修复了迟到会话快照误关闭刚重新加入的群、成员重新加入后旧已读位置覆盖新 membership、权限降级后残留审批页、搜索结果删除后丢失分页边界，以及多行 header 遮挡输入框。恢复使用请求上下文和最小 generation；普通消息流量不会因为每次 dirty 都重启分页而饿死列表。

实际运行双用户 `chat_tui` 并在 tmux 中完成注册登录、联系人与单向只读、direct 双向收发、回复、reaction、搜索/可复制文本、附件上传/保存且 SHA-256 一致，以及群创建/typing/公告/置顶。覆盖 80×24、100×30、120×40、60×20、35×10 再恢复；停止并重启独立测试 server 后自动重连、恢复当前群及历史、再次发送成功，最后两端 logout/Ctrl+C 正常退出。空闲 3 秒实测进程 CPU 增量为 0 ticks。仅按创建时记录的精确 ID 清理本次测试账号与会话，未修改历史身份。终端 smoke 另修复长用户名列表水平居中裁掉前缀的问题，增加合法 64 字节用户名渲染回归。

最终 ASan 的“认证中关闭 TUI”集成回归暴露服务端会话 lifetime 缺口：bcrypt 转到 system thread pool 时没有维持 I/O outstanding work，测试服务退出可能早于认证 continuation 返回。`connection_worker::run_session()` 现在为整个会话持有 work guard，让关闭正常等待会话结束；未修改第三方源码、协议或授权规则，保留原失败场景作为回归，不用延时等待或 suppression 绕过。

全部修复后重新实际执行 `tests/verify.sh`，Qt/TUI 在三套构建中均为 ON；完整构建及全部 19 个 CTest 通过：

| 构建 | 完整 build | 完整 CTest | CTest 耗时 |
|---|---|---|---|
| normal | PASS | 19/19 PASS | 81.77 s |
| ASan | PASS | 19/19 PASS | 105.87 s |
| UBSan | PASS | 19/19 PASS | 97.26 s |

包含原有 `qt_models`、`qt_delegate`、`qt_ui` 和全部 server/client/migration 测试；没有 suppression、测试排除、跳过 TUI 或放宽 timeout。`git diff --check` PASS。依赖除新增 FTXUI 外未升级，SQL 001–025 未修改。

## 终端多行粘贴修复

以 `a000f0901d393a4653055139604b370777970f00` 为基线，TUI 启用终端 bracketed paste，并通过现有 FTXUI `Special` 事件识别开始/结束标记。粘贴中的换行进入消息草稿，不触发发送；手动 Enter 仍发送一条完整消息。单行字段将粘贴换行/tab 作为空格输入，不触发登录、命令或确认操作。粘贴只进入开始时的输入目标；取消或会话/输入上下文变化后丢弃剩余内容。没有增加完整粘贴缓存、定时器、配置项或第三方修改。未提供粘贴边界的终端输入仍无法可靠区分粘贴换行与手动 Enter。

现有 `tui_render` 回归先确认 RED，再转为 GREEN，覆盖多行正文、取消、会话变化、重连上下文及密码/命令输入不自动提交。真实 tmux smoke 11/11 PASS：两个 TUI 收发中文、emoji、空行和末尾换行，三次均确认粘贴及 Esc 保留草稿时没有消息，Enter 后只持久化并送达一条完整正文；粘贴途中停止/重启 server 后，残余输入不执行导航/退出，恢复后保留草稿并等待手动发送。另以实际 shell job control 验证 Ctrl+Z 挂起关闭粘贴模式、`fg` 恢复重新启用，以及 Ctrl+C 退出关闭模式。

实际完整执行 `tests/verify.sh`，Qt/TUI 均为 ON；normal、ASan、UBSan 完整 build 及全部 20/20 CTest PASS，CTest 耗时分别为 94.46 s、126.62 s、122.40 s。没有 suppression、排除测试或放宽 timeout。SQL、server、client library 和 Qt 未修改。

## 真实使用与发送后切页回归

本轮实际基线为 `e55d877945c96856f58096ac0fdecd25dd9c8a9d`，开始时工作树干净且与 `origin/main` 一致。审查最近提交、当前产品规则、Qt/TUI 导航与发送回调、已有验证及真实交互入口；没有按功能清单继续增加产品能力。Qt/TUI 均启用的 Debug clean build 完成，未发现编译警告。

真实 TUI 复现了发送后立即切页的问题：消息已经提交，但成功回调先因页面上下文变化返回，尚未清除的已发送正文仍作为草稿，返回会话后容易重复发送。用隔离数据库的 conversation 行锁阻塞真实发送 RPC，确认其等待锁后切页再释放；修复前连续三次均仅持久化一条消息但遗留草稿。

`tui/src/messages.cpp` 现在先处理成功发送所属会话的草稿，再检查当前页面。仅清除仍与这次发送相同的草稿；切到其他会话时使用已有草稿映射，保留后来输入的正文。成功后会话列表仍从服务端刷新，迟到回调不切换页面、不改变新页面的发送状态；失败继续保留草稿。没有增加 generation、缓存、字段或协议，也没有修改 server、SDK、Qt 生产代码或 SQL。

现有 `tui_integration` 新增回归覆盖切到 Contacts、切到另一会话并保留该会话草稿、保留后来输入的草稿及服务端拒绝时不清除。测试通过 SDK 回调投递与 UI inbox 的实际边界确定执行顺序，不依赖 sleep；修复前确认 RED，修复后 `tui_integration`、`tui_state`、`tui_render` 共 3/3 PASS。真实 tmux 相同锁同步场景再跑三次，均只持久化一次且原草稿清空。

真实 Qt 规模测试发现的另一问题属于驱动：两步建群和退出确认的坐标点击了相反按钮。`239a69d`（修正真实 Qt 测试的对话框操作）仅修正 `tests/qt_x11_smoke.py` 的点击位置，保留原断言；两个真实 Qt/X11 进程的四个场景全部通过，包括 100 人群、陌生人资料申请和明确接受、双向消息、两步建群搜索多选取消，以及退出取消/确认。

真实 TUI 导航 smoke 11/11 PASS，覆盖好友申请与历史只读、宽窄屏导航、动态选择、双向消息布局、中文多行粘贴、断线期间粘贴尾部丢弃、建群/加入和退出确认。另用两个真实终端完成六个日常场景：窄屏长文件名附件复制/保存且 SHA-256 相同、原文删除后已发布引用的占位、双会话草稿在实时重排后保持、55 条消息的历史/搜索分页、搜索期间断线后恢复会话和未发送草稿、两端 Ctrl+C 退出。相同文件名在气泡中截断但复制页及保存可访问完整内容，没有证据要求改变现有布局；临时驱动等待了不可见文本或错误语言的状态也没有被当作产品缺陷。

最终实际完整运行 `tests/verify.sh`，Qt/TUI 都为 ON：

| 构建 | 完整 build | 完整 CTest | CTest 耗时 |
|---|---|---|---|
| normal | PASS | 20/20 PASS | 91.69 s |
| ASan | PASS | 20/20 PASS | 128.73 s |
| UBSan | PASS | 20/20 PASS | 122.72 s |

没有 suppression、跳过、排除测试或放宽 timeout；构建日志没有编译警告，`git diff --check` PASS。原有 migration、依赖和 submodule 未改，临时脚本与证据仅保留于 `/tmp`。主要证据为 `/tmp/chat-maintainer-final-verify-20261004.log`、`/tmp/chat-maintainer-qt-final-20261004`、`/tmp/chat-maintainer-tui-nav-20261004`、`/tmp/chat-maintainer-send-switch-20261004`、`/tmp/chat-maintainer-send-switch-green-20261004` 和 `/tmp/chat-maintainer-tui-use-confirmed-20261004`。

资源观察期间两端 TUI 都保持 4 个线程、7 个 FD；恢复后空闲三秒 RSS 不变，CPU 增量分别为 0.01 s、0 s。上述是短时真实使用与状态转换证据，不能证明数天运行无泄漏，也没有据此修改 allocator、缓存或轮询策略。

## 体验品质审查：弹出菜单

体验提升以 `75fb9a0292a3891fcec84982579e0fe1c346d999` 为基线。
真实 Qt/X11 截图发现全局透明 QWidget 样式使消息右键菜单出现黑底深色字。
现在为 QMenu、选中项和子菜单设置明确的浅色表面与绿色选中态，保持原有动作和权限。
回归先确认透明背景 RED，再验证菜单、键盘选中项和 reaction 子菜单 GREEN。
真实 Qt 导航 4/4、补充 X11 页面及 HiDPI 采集通过；真实 TUI 基线导航 11/11 通过。
完整 `tests/verify.sh`：normal 20/20（97.91 s）、ASan 20/20（131.87 s）、UBSan 20/20（123.57 s），
无 suppression、跳过或 timeout 放宽，`git diff --check` PASS。
参考边界、before/after 原图、初始问题清单和评分见 [体验品质审查](experience-quality.md)。
登录层级、多行输入、跨页一致性和连续两轮最终自审仍在本轮 Goal 内，尚未宣称完成。

## 体验品质审查：桌面登录与注册

Qt 登录默认聚焦用户名，服务器地址默认收起并可渐进展开；保持原有连接和身份语义。
登录与注册统一表单宽度、内边距和间距，明确主次操作、字段 accessible name 与键盘焦点。
回归确认旧初始焦点 RED，新布局在五档窗口和设置展开/收起后 GREEN。
真实 X11 注册拒绝非法身份、成功创建真实隔离账号并返回登录，
125% / 150% / 200% 登录与注册采集、后续导航 4/4 均通过。
完整 `tests/verify.sh`：normal 20/20（96.68 s）、ASan 20/20（129.13 s）、UBSan 20/20（123.90 s），
无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
截图、审查取舍和未完成项继续维护在 [体验品质审查](experience-quality.md)；整体品质循环仍未完成。

## 体验品质审查：终端登录层级

TUI 默认聚焦 Username，服务器设置渐进展开，保留原有 URL 与认证语义。
采用无按钮独立边框的终端原生层级、明确反色焦点和紧凑低高度布局。
组件回归先 RED 后 GREEN，覆盖 Unicode、密码遮蔽、设置展开编辑及收起后值保留。
真实 tmux 八档尺寸、原生注册和日常导航共 13/13 通过，保存原始 ANSI 与格宽预览。
完整 `tests/verify.sh`：normal 20/20（95.70 s）、ASan 20/20（129.81 s）、UBSan 20/20（123.16 s），
无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
尚未完成多行 Qt composer、跨页一致性与最终两轮品质自审，不将本阶段等同于整个 Goal 完成。


## 体验品质审查：桌面多行消息输入

Qt 输入区从单行改为有界增长的纯文本编辑，支持多行粘贴、Enter 发送与 Shift+Enter 换行。
空草稿禁用发送，离线保留文字，长内容在框内滚动，输入法未确认文字不会提前发送。
回归覆盖多行、软换行 resize、滚动、离线和输入法确认；原有回复、typing、
删除联系人、成员移除与重连用例继续通过，没有改业务规则或 migration。
真实 Qt/X11 双客户端与五档尺寸、三档缩放、导航共 7/7 通过；TUI/tmux 回归 11/11 通过。
完整 `tests/verify.sh`：normal 20/20（100.30 s）、ASan 20/20（127.75 s）、UBSan 20/20（119.27 s），
无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
截图和阶段评分继续维护在 [体验品质审查](experience-quality.md)，整体 Goal 仍在进行。

## 体验品质审查：桌面草稿与发送确认

Qt 草稿按会话保存于内存，切换恢复对应正文，关闭/成员移除/账号切换清理。
发送等待期间保持输入可编辑，阻止同一会话重复提交；成功后仅清除仍匹配的草稿。
迟到成功不覆盖后来文字或其他会话；参数拒绝和断线保留未确认正文，重连不自动重发。
中文 fallback 字体的实际行高曾使短多行正文被遮住，现按 QTextLine 排版结果有界增长到六行。
保持原有 server/RPC/SQL、回复切换行为和 client bridge 的连接 generation。

既有 Qt 测试新增会话归属、成功/失败、后台确认、新草稿、重连和真实 server 拒绝回归，
三处缺陷均先确认 RED 后修复。
真实 Qt 双客户端草稿六项、输入/五档窗口/三档缩放/导航七项，TUI/tmux 回归 11/11 均通过；
发送竞争以真实数据库行锁同步，不靠 sleep 猜顺序。
最终完整 `tests/verify.sh`：normal 20/20（98.19 s）、ASan 20/20（129.67 s）、
UBSan 20/20（124.03 s），无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
真实截图、边界和未完成项见 [体验品质审查](experience-quality.md)；整体品质循环仍未完成。

## 体验品质审查：桌面好友申请

收到/发出的好友申请复用联系人行的头像、姓名/状态层级、选中态和焦点轮廓。
短列表按内容收口，空区隐藏，长列表分别滚动；Enter 打开原有资料与申请操作。
实时头像变化刷新对应行，权威列表刷新保持选择和正在浏览的位置，缩窗保留键盘选择可见。
没有新增列表模型、权限状态、协议或 migration，好友语义不变。

既有 Qt UI 回归覆盖行节奏、真实图片缓存、80 条申请、刷新/错误/空态、键盘与 resize；
真实双 Qt 流程 8/8、25 条真实申请的键盘/缩窗流程、100 人群流程 4/4、TUI/tmux 11/11 通过。
完整 `tests/verify.sh`：normal 20/20（97.06 s）、ASan 20/20（130.31 s）、UBSan 20/20（123.44 s）。
随后 Qt/TUI/server/client 全量 clean build 和诊断 fixture PASS，client 严格警告检查 PASS，
重建后的完整 CTest 20/20（101.00 s），无编译警告，`git diff --check` PASS。
没有 suppression、跳过或 timeout 放宽；截图与待处理项见 [体验品质审查](experience-quality.md)。
群资料、终端页面层级、HiDPI 图标及最终连续两轮品质自审仍未完成。


## 体验品质审查：桌面群资料与管理

群概览、成员、入群申请和管理采用统一的信息层级、用户行、焦点和主次操作。
概览显示最多三位成员并保留全部成员入口，不限制群人数。
概览和管理在小窗口中原生滚动；公告全文支持键盘焦点、选择和读取到末尾。
实时头像刷新所有成员/申请列表，申请刷新按用户 ID 保留正在审阅的身份与浏览位置；
百人成员列表缩窗后保持有焦点的当前行可见，没有新增加载或权限 shadow state。
保持原有角色、管理员上限、审批、公告、邀请、退出、移除和群主转让语义，无 migration。

现有 Qt UI 测试补齐头像、空态、刷新选择、Enter、表单滚动、末尾成员 resize 和公告键盘回归，
真实双 Qt 七项流程、百人群原生流程 4/4、TUI/tmux 回归 11/11 通过。
实际查看 420×480、520×680、720×820、连续 resize 与 125%/150%/200% 完成加载后的原图；
早期驱动假设和加载中截图已纠正，未计为完整验收。
最终完整 `tests/verify.sh`：normal 20/20（94.53 s）、ASan 20/20（135.70 s）、UBSan 20/20（118.17 s）。
无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
server、client library、SQL 和依赖未修改；证据及其余品质任务见 [体验品质审查](experience-quality.md)。
终端空态/页眉、其他资料与弹窗、HiDPI 图标和最终连续两轮 fresh review 仍未完成。


## 体验品质审查：终端空态与页眉

五处已复现的空会话、历史、入群申请、用户搜索和消息搜索分别说明内容，
空会话和消息搜索提示已有的下一步入口。无入群申请时不再提示通过/拒绝。
标题、次级键盘提示和正文分层，长搜索词不再剪掉操作提示；成员管理提示固定在页眉。
没有新增状态、公共 API 或测试 executable；未改 Qt、server、client library、SQL 和依赖。

既有 render 测试先 RED 后 GREEN；TUI 定向 CTest 5/5，真实双 TUI 七项空态/搜索/复制/新建流程、
原有 tmux 导航 11/11、Qt/X11 导航 4/4 通过。实际查看 60、70、80、100、120、160 列，
样式原文无损保留，PNG 仅为格宽预览，不替代完整终端字体和浅色背景验收。
完整 `tests/verify.sh`：normal 20/20（97.76 s）、ASan 20/20（132.49 s）、UBSan 20/20（122.70 s）。
无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
证据、评分及仍未完成的品质任务见 [体验品质审查](experience-quality.md)。


## 体验品质审查：头像绘制与资料交互

Qt 头像与 SVG 各使用一个 DPR=2 资源，保留逻辑尺寸，覆盖 100/125/150/200% 缩放。
真实头像缓存保留原有中心圆形显示语义，解码一次后缩至 208×208，满足最大 104 逻辑像素头像。
不引入多尺寸缓存或 icon engine；revision、传输、权限与生命周期规则不变。

移除群概览、好友申请和搜索头像的重复资料绑定。
联系人与会话由现有 delegate 区分头像/正文点击，关闭头像资料不会继续打开原行聊天；
正文仍单击进入聊天。没有新增 opened/busy/generation 状态。
既有公共绘制/缓存和实际 viewport 鼠标事件回归先 RED 后 GREEN，集成测试保留原业务断言。

真实双 Qt 四档缩放、加载后的群/用户/自己资料和一次点击关闭验证通过；
原有 Qt/X11 导航 4/4、TUI/tmux 导航 11/11 通过。
完整 `tests/verify.sh`：normal 20/20（93.23 s）、ASan 20/20（131.17 s）、UBSan 20/20（123.32 s）。
无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
本机原生日志保留缺失 at-spi bus/GTK 模块提示，无障碍未据此记为完整验收。
server、client library、SQL 和依赖不变，无 migration。
截图、自评及仍未完成的资料整体层级和品质任务见 [体验品质审查](experience-quality.md)。


## 体验品质审查：用户与账号资料层级

用户资料不再重复显示身份，保留标题选择与复制能力。
宽度与群资料统一到 520、内边距 24、间距 12；头像仍为 104，主要/次级动作采用 160×40 图文按钮。
消息/好友确认与更换头像为主要操作，复制为次级，退出及关系移除/取消/拒绝降低视觉权重。
关闭图标有 accessible name 和 tooltip；身份下显示已有账号、关系或联系人 presence，
现有 model/好友事件实时刷新，关系移除后停止显示 private presence。不增加请求或 shadow state。
移除不用的重复身份控件和样式，未改 server、client library、TUI、SQL 或依赖，无 migration。

现有 Qt UI 入口补 identity、64 字节中文/emoji、520×600 内有界布局、操作权重、关闭名称和实时关系/presence 回归，
逐项 RED 后 GREEN，保留原好友/历史/群权限及生命周期断言。
真实双 Qt 四档缩放、资料开关和系统 clipboard 复制通过；Qt/X11 导航 4/4、TUI/tmux 导航 11/11 通过。
原生脚本更新可见操作定位，避免继续依赖旧资料绝对坐标；失败 fixture 已清理。
完整 `tests/verify.sh`：normal 20/20（98.19 s）、ASan 20/20（131.03 s）、UBSan 20/20（121.52 s）。
无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
参考补证据记录了 FWA Ceramic Beats 真实 CLEAR 前后界面；Every Neuron 未完成加载，不记为交互验收。
截图、自评和剩余品质工作见 [体验品质审查](experience-quality.md)。


## 体验品质审查：终端长身份与摘要

TUI 单行摘要复用现有 FTXUI 格宽接口保留省略号，避免宽字吃掉边框；
群人数、成员角色与连接状态独立保留，侧栏为滚动指示留一格。
用户/账号资料显示完整身份并正常换行，正文、复制和身份数据不变。
既有公共 render 回归逐项 RED 后 GREEN，覆盖六档宽度、64 字节身份及角色。
没有新增状态或解析框架，Qt、server、client library、SQL 和依赖不变，无 migration。

真实 xterm 浅色/深色和 60/70/80/100/120/160 列采集 31 张原图；
两真实 TUI 双向聊天，另一个通过实际注册/邀请链接加入，并核对权威身份与成员关系。
原有 TUI/tmux 导航 11/11、Qt/X11 导航 4/4 通过。
完整 `tests/verify.sh`：normal 20/20（95.66 s）、ASan 20/20（138.23 s）、UBSan 20/20（121.19 s）。
无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
额外 DSR 与依赖源码确认 FTXUI 7.0.3 对复合 emoji/宽字组合符存在格宽或输出问题，
尚未修复，不将本轮记作全部 Unicode 验收。
原图、参考边界、评分和后续工作见 [体验品质审查](experience-quality.md)。


## 体验品质审查：消息搜索与附件弹窗

搜索采用 Qt 主默认搜索按钮，Enter 在输入框只搜索一次，聚焦分页/关闭仍执行相应动作。
取消重复 returnPressed 路径，不增加键盘状态或 handler 框架。
图片预览按可用区域平滑等比例缩放，不再让源图尺寸限制窗口；
保留解码源以便缩小后重新放大，不反复解码，保存继续使用原始 bytes。
两类弹窗复用 24/12 布局尺度，底部操作紧凑排列，明确搜索/保存与关闭的权重。
既有 Qt UI 公共事件、cursor、geometry、显示像素回归逐项 RED 后 GREEN，旧断言保留。

真实双 Qt 完成邀请链接入群、群聊、连续 Enter 搜索、图片 resize 和系统文件选择器保存。
100/125/150/200% 完成加载后采集 50 张实际窗口；不是全部页面的尺寸排列验收。
原有 Qt/X11 导航 4/4、TUI/tmux 导航 11/11 通过。
完整 `tests/verify.sh`：normal 20/20（97.29 s）、ASan 20/20（133.72 s）、UBSan 20/20（120.43 s）。
无编译警告、suppression、跳过或 timeout 放宽，`git diff --check` PASS。
server、client library、TUI、SQL 和依赖不变，无 migration。
创建/加入群、确认框、复合 emoji、剩余矩阵与最终两轮 fresh review 尚未完成；
原图、失败驱动的核验与阶段评分见 [体验品质审查](experience-quality.md)。

## 体验品质审查：建群与操作确认

选人、命名、加入群共用 24/12 布局尺度与有限宽度；命名页不再沿用高列表画布，
有界成员列表可用 Tab/End 浏览，真实 20 人尾部和焦点已复核。
标准按钮完成命名后刷新样式，主操作与取消/危险动作分层，取消平台图标。
六种确认使用具体中文动作与纯文本正文，初始 Enter/Esc/关闭取消；显式确认才执行。
邀请联系人要求实际选择，注册成功/错误通知和关闭条使用中文动作。
业务权限、邀请审批、群创建与联系人规则不变，无 server/client/SQL/third 修改或 migration。

真实双 Qt、四档缩放完成 44 张原图；Qt/X11 导航 4/4、TUI/tmux 导航 11/11。
完整 `tests/verify.sh`：normal 20/20（102.42 s）、ASan 20/20（129.69 s）、UBSan 20/20（122.08 s）。
无 sanitizer 报告、编译警告、跳过或 timeout 放宽，`git diff --check` PASS。
Help 横向截断在独立真实终端复现；与复合 Unicode 和剩余矩阵继续处理。
Qt 暂评 87、TUI 77，尚未进入最终连续两轮 fresh review。
原图、驱动失败根因及覆盖边界见 [体验品质审查](experience-quality.md)。

## 体验品质审查：终端 Help 完整阅读

Help 静态说明按完整单词换行，同一内容生成同时用于渲染和实际滚动范围；
Help/Copy 使用纵向视口，不再让未约束的命令段落横向裁掉。
Copy 的原文本/Unicode 换行不变，不把此项算作复合字符问题的解决。
六档宽度乘两档高度的公共键盘事件回归验证完整 33 命令、j/k 到底回顶和 Esc 返回 Contacts。
真实 xterm/tmux 八组尺寸完成 32 组 PNG/ANSI，验证返回 Chats；
主代理和独立代理亲看原图，专项 T05 收口，不代表最终全面 fresh review。

Qt/X11 导航 4/4、TUI/tmux 导航 11/11；完整验证 normal、ASan、UBSan 各 20/20。
最终 normal 重跑为 99.09 秒，ASan 132.50 秒、UBSan 124.60 秒，Qt/TUI 均启用。
无 sanitizer 报告、编译警告、跳过或 timeout 放宽，自有成功隔离库与进程已清理。
`git diff --check` PASS，Qt/server/client/SQL/third 不变，无 migration。
Qt 暂评 87、TUI 78；复合 Unicode 与剩余页面、键盘、终端、参考矩阵继续处理。
原图、覆盖边界和后续工作见 [体验品质审查](experience-quality.md)。

## 体验品质审查：消息编辑与目标归属

消息编辑继续使用真实 QInputDialog，宽度 520、内容边距 24、间距 12，
中文保存/取消与原有按钮语言一致；正文按宽度换行，Tab 聚焦保存而不替换全选内容。
菜单打开前捕获消息 ID、会话与正文，历史插入不会把编辑目标或初始正文带到另一条消息；
保存仍检查原会话及可发送权限。空串不提交，非空空白原样保存，业务规则不变。

七种公共事件场景、两类目标漂移与输入边框像素均先复现 RED，再验证 GREEN。
首轮原图发现编辑区样式未应用，完成命名后刷新样式，再完整采集四档 DPI 与双 Qt。
最终 49 张原图证据包含长 Unicode 正文、取消/保存、Tab、另一客户端实时更新，
主代理与独立代理实际复核边框和长文首尾；不是全页面或无障碍完整验收。
最终完整 `tests/verify.sh`：normal 20/20（97.32 s）、ASan 20/20（135.61 s）、UBSan 20/20（122.28 s）。
Qt/X11 导航 4/4、TUI/tmux 导航 11/11，Qt/TUI 均启用；无编译警告、sanitizer 报告、
suppression、跳过或 timeout 放宽。自有成功隔离库与进程已清理，`git diff --check` PASS。
Qt 暂评仍为 87、TUI 78；其他菜单目标、复合 Unicode、参考与剩余矩阵继续处理。
未改 server、client library、TUI、SQL 或 third，无 migration。
原图、失败驱动的区分及专项覆盖边界见 [体验品质审查](experience-quality.md)。

## 体验品质审查：消息菜单操作归属

真实嵌套菜单事件循环中的旧历史插入，先复现删除、回复、附件和读者详情目标漂移。
操作捕获原会话与消息 ID，回复也保留原发送者/正文；菜单和删除确认后重查连接与会话。
已读详情按原消息 ID 刷新，失去资格时关闭，不增加 token、状态或后端快照。
递归检查又复现搜索分页期间复制错误正文，菜单前捕获 QString 后永久回归 GREEN。
十个公共事件场景覆盖历史插入、会话切换、取消和确认时切换，原断言全部保留。

真实双 Qt 完成特定读者、原 ID 回复、取消/显式删除、130-byte 附件原字节保存与系统剪贴板复制。
统一最终二进制 SHA、同期源码与 fixture 元数据完整，26 原图和 20 裁图不是独立测试数。
完整 `tests/verify.sh`：normal 20/20（98.08 s）、ASan 20/20（134.78 s）、UBSan 20/20（122.24 s）。
Qt/X11 导航 4/4、TUI/tmux 导航 11/11；无警告、sanitizer 报告、跳过或 timeout 放宽。
隔离库已删除、自有进程与端口释放，长期服务未动，`git diff --check` PASS。
实际原图新增 Q18：回复取消按钮低对比、已读列表默认选中样式；下一阶段继续收口，
不能以操作正确代替视觉与可访问性验收。Qt 暂评 87、TUI 78，复合 Unicode、全矩阵和
最终连续两轮 fresh review 尚未完成。未改 server、client、TUI、SQL 或 third，无 migration。
证据和覆盖边界见 [体验品质审查](experience-quality.md)。

## 体验品质审查：回复取消与读者列表

取消回复复用 SVG、36×36 点击区域、中文可访问名称和 tooltip，键盘轮廓与悬停同品牌。
Space 或鼠标取消后回输入并保留多行草稿，下一条发送不携带旧引用。
已读成员复用现有用户 delegate，以真实“已读”状态、头像、姓名省略和完整 tooltip 呈现；
刷新仅局部捕获读者 ID 和滚动，恢复原身份，成员消失不移交选择，不增加长期状态。

公开控件回归先 RED 后 GREEN；永久断言覆盖键盘、鼠标、草稿、23 人滚动、
头像/回执更新与读者移除。独立四档延迟事件循环探针补核焦点、身份与滚动。
真实双 Qt 的 100/125/150/200% 使用完成 76 组原图/裁图，实际多行正文与无 quote 发送精确一致，
读者 51→52 后选择保持，64-byte Unicode 姓名合法注册，长提示完整可读。
主代理与独立代理亲看最终状态原图，没有 Q18 新 material 缺陷，专项收口。
两次原生驱动失败分别为未真实阅读、夹具预留账号未连接，证据不算 PASS，未为此改产品。

完整 `tests/verify.sh`：normal 20/20（96.74 s）、ASan 20/20（137.27 s）、UBSan 20/20（123.14 s）。
Qt/X11 导航 4/4、TUI/tmux 导航 11/11；无 sanitizer 报告、警告、跳过或 timeout 放宽。
最终隔离库已删除，自有进程和端口释放，长期服务未动，`git diff --check` PASS。
未改 server、client、TUI、SQL 或 third，无 migration；证据见 [体验品质审查](experience-quality.md)。
Qt 暂评 87、TUI 78，复合 Unicode 严格探针仍 RED，剩余矩阵和两轮完整 fresh review 未完成。

## 体验品质审查：长消息与提及排版

长姓名提及的旧原生图出现气泡外溢；公开回归又复现长 token 测量高度不足。
统一正文 QTextLayout 测量与绘制，无词边界时自然换行；提及、引用和原文字节不变。
永久 256 组合覆盖宽度、方向、正文种类、坐标原点和 DPR，先 RED 后 GREEN，
核对完整高度与字形像素；短消息和原有反应/已读/附件断言保留。
独立实际绘制区域点击补核 36 次反应与六次已读目标，无空白误触。

最终 `tests/verify.sh`：normal 20/20（103.42 s）、ASan 20/20（141.22 s）、
UBSan 20/20（134.91 s），Qt/TUI 均 ON；无 sanitizer 报告、编译警告、跳过或 timeout 放宽。
Qt/X11 导航 4/4、TUI/tmux 导航 11/11；隔离验证库已删除。
双 Qt 原生操作与最终视觉复审记录见 [体验品质审查](experience-quality.md)。
最终驱动 exit 0，主端四档缩放、另一端固定 100%，44 组原图/裁图覆盖四种窗口和连续 resize；
实际发送正文与提及、SDK 引用和真实读位一致，最终自有数据库、客户端和端口已清理。
仅 Qt delegate、既有回归和证据文档变化，server/client/TUI/SQL/third 不变，无 migration。
临时 UAX29 候选独立重编验证官方实际 853 cases 与单次线性扫描，
但 leading mark 渲染、Screen span、裁剪和原生列策略仍未解决，不作为 T04 完成。
Qt 暂评 87、TUI 78，完整品质矩阵与最终两次 fresh review 继续推进。

## 体验品质审查：终端字素编辑

以 `a04c91e1fc765ac9e0010ff8159b9bd3ef58432d` 为基线，FTXUI Input 改用维护库
utf8proc 2.12.0 / Unicode 18 的 UAX29 边界，移动、删除、覆盖不拆字素，
CRLF 保留原始 offset，回调重入不读取旧边界。宽字组合符附着到真实字格而非保留空格。
依赖通过 TUI 专属 CMake 固定官方 archive/hash；构建目录精确修补原 target，
子模块和 gitlink 不变，无 `/tmp` fork 交付或新应用层 Unicode framework。

永久官方 853 条 oracle 与 11 类输入/合并/覆盖/密码/上下/鼠标/CRLF/重入通过，
原 pin 对相同测试 actual RED，生产 actual GREEN。
最终 `tests/verify.sh` normal/ASan/UBSan 均 21/21（104.86/136.98/132.55 s），
Qt/TUI 均 ON；无编译告警、sanitizer 报告、跳过或 timeout 放宽。
全新 Debug `-Werror` build/21 CTest 通过；额外全量 RelWithDebInfo `-Werror`
遇未修改 Capy/PG coroutine 的 GCC 告警，仅 Unicode target 在该配置通过，未压告警。
首次并行 CTest 固定端口冲突失败保留，最终已串行全量重跑。

双真实 TUI 在 60/80/120 列完成 132 条字素编辑发送流程，SDK 精确正文和对端显示
同时核验，66 份带样式 capture 保留；既有 TUI 导航 actual 11/11。
隔离数据库、自有客户端/服务器/端口已清理，长期服务未动。
终端 width/span、leading mark、原子裁剪、选择复制和外层 shaping 尚未解决，
真实 ZWJ 行仍有残留，不将本阶段 GREEN 视为 T04 完成。
详见 [体验品质审查](experience-quality.md) 和 [依赖维护说明](../tui/cmake/README.md)。
Qt 暂评 87、TUI 78，全产品品质目标及最终两轮 fresh review 继续推进。

## 体验品质审查：终端渲染隔离验证

严格107条span断言证明生产渲染仍61项FAIL。隔离原型完成整字组写入、原子裁剪，
新增viewport传播并迁移Border/Separator/Gauge/Graph/Canvas实际writer；最终仍3项FAIL，
未接入Chat或修改third。独立嵌套Frame/半格选择27/0、import14/0；sibling writer原型
旧83条FAIL、新0，数量不是独立场景数。原生独占XTerm的3个CJK对照证实旧越界、
边框缺失和覆盖字符丢失，候选对应修正，不能外推emoji或全产品。

fresh隔离ASan/UBSan均build成功，82翻译单元全部核插桩；两套viewport/import/Unicode
以及sibling writer检查均通过，report0。严格span仍exit1，未冒称正式21CTest重跑。
两项leading/zero-only显示与raw空续格擦除契约、实际terminal列/outer shaping仍待处理。
原型目录与冻结证据见[体验品质审查](experience-quality.md)；生产暂评Qt87/TUI78不提高，
T04和完整体验目标保持未完成。本阶段无应用/server/client/SQL/third改动，无push。

## 体验品质审查：终端完整显示与原文选择

从 `42687959b138aa8de567d8e00533a328a25df3d9` 继续，把完整字素 span、Frame 选择视口、
typed writer 与显示载体接入正式 TUI。21 文件 SHA guard/零 fuzz 维护补丁不修改 FTXUI 子模块；
patched public headers 传递给 screen/dom/component 消费者，避免 Cell 布局混用。
Text/VText 保存单份原文与字素 offsets，Input/换行/省略共享显示列模型；复制、编辑与发送不写入载体。
孤立 Mc 的 dotted-circle 原型经真实 DSR 发现两列后改成单格 replacement；正常 base+Mc 不改变。
mutable CellAt/at 的字符修改不属于 typed 写入契约，未假称可以观察空续格 clear 的擦除意图。

永久 `tui_display` 628/0、`tui_span` 80/0、原官方边界853/0。最终完整 `tests/verify.sh`
normal/ASan/UBSan 各23/23，104.09/138.44/133.91 s，runner actual exit0；
本阶段 fresh Debug `-Werror` 最终源码全构建与23/23 CTest通过（107.63 s）。
初次 normal/ASan 的 render timeout 保留，未放宽5秒门禁；改为单份正文、单次布局后最终
render 0.86/4.05/1.77 s。Qt/X11导航4/4、TUI/tmux导航11/11。
两个真实 TUI 的132条字素编辑发送和六档宽度48条 carrier发送/删除均实际完成，
SDK原文与唯一消息ID精确；成功及已定位的失败探针隔离库均已删除。
独立当前正式库原生XTerm取得40份真实DSR和4PNG，carrier/CJK 10输出行严格匹配；
主代理与独立代理亲看，before/after helper原图与准确版本证据见[体验品质审查](experience-quality.md)。

本阶段不关闭T04：当前80列ZWJ capture仍有边界残留，完整terminal宽度/font shaping未解决。
Qt暂评87、TUI78不提高；全产品矩阵及两次完整独立fresh review仍未完成。无server/client/SQL/third改动，无push。

## 体验品质审查：孤立肤色修饰符

从 `7019086662a55f7ce2016f97698a527ef4a12359` 继续。孤立肤色被当作有基字，
而旧scalar列政策将其跳过，造成Text/Input零列、实际消失。永久11组追加fixture先取得
1096项/108失败，再用精确 `Extend && Sk` 修正到已有显示载体，正式targeted1096/0。
独立全码点扫描证明仅五种肤色命中，正常emoji＋肤色仍raw；复制/编辑/发送不写入replacement。
span80/0与官方边界853/0保留。

两个真实TUI六档宽度完成120条发送/原子Backspace，其中72条为本次肤色场景；
SDK原文、唯一ID及对端接收一致，自建数据库/进程/端口清理，长期服务未动。
最终正式 `tests/verify.sh` normal/ASan/UBSan各23/23，105.52/147.39/132.58s，runner exit0；
fresh Debug `-Werror`全构建及23/23 CTest通过，102.40s。无编译警告、sanitizer报告、
跳过、suppression或timeout放宽；原render5秒不变，正式0.86/4.10/1.75s。
正式冻结库的原生前后对照八配置、96行、192份DSR实际exit0；六孤立输入四环境的
24次after观测可见且完整行80列。正常手势的XTerm82列及空格组合的84/88列差异仍开放，
原图保留失败行，不把helper或tmux虚拟格当作全产品通过；自有17进程/专用tmux已清理。
完整终端宽度研究实际比较C11库API、XTerm/WezTerm/tmux的DSR与原图，
候选能改善家庭/ZWJ但并未全部匹配，未集成候选、未修改third或终端配置。
证据、版本和失败边界见[体验品质审查](experience-quality.md)。
Qt暂评87、TUI78不提高；T04与整体品质目标、完整日常使用及两轮fresh review继续开放。

## 体验品质审查：搜索键盘选择

从 `86787802751618a84fd84460d0de31f4042b25d9` 继续。实际Tab/Down已改变搜索当前行，
但100/200%列表像素均零变化；各六条视觉断言RED。message delegate现从已有view
selectionMode和Selected/HasFocus画选中/焦点轮廓，不保存新模式或选中状态；
普通NoSelection消息、正文、尺寸及动作命中区域不变。
永久delegate16组合及实际dialog逐行Tab/Down、blur回归GREEN，旧断言全部保留。
真实双Qt四档缩放重新完成搜索/复制/peer编辑删除/草稿发送，统一实际binarySHA，
主代理亲看四档原图。首轮关闭窗口枚举旧XID的driver失败保留，产品未为此修改。
全新Debug Qt/TUI ON `-Werror`构建及23/23 CTest通过，103.48s；本阶段正式
`tests/verify.sh` normal/ASan/UBSan各23/23，105.23/137.10/130.38s，原runner实际exit0。
SSH最终观察返回255后重核原PID已结束、实际marker及结果已落盘，未重复启动；
无编译告警/sanitizer报告/skip/suppression/timeout放宽。自有五Qt/两DB/18893已清理。
证据、前后原图、已定位的测试driver失败边界见[体验品质审查](experience-quality.md)。

新发现Q21：live编辑/删除后的搜索数量口径不清，删除占位仍被计入；T06：TUI搜索删除
前项只clamp索引导致选中ID漂移，public state实际RED。这两项保持开放，不混入键盘绘制修复。
Qt87/TUI78不提高，T04与完整品质Goal继续推进，未push、未修改third或长期服务。

本机当前AT-SPI已可用，独立专用bus/Xvfb实际读取正式Qt登录树和原生Tab/Space焦点事件，
capture exit0；用户名/密码/登录/注册/服务器设置及展开后的地址名称角色均可读取。
只验证登录导航，不冒称Orca语音、错误announce或登录后全矩阵；自有进程已清理。
详细证据和未验证边界见[体验品质审查](experience-quality.md)，旧缺bus记录仅属于历史环境。

## 体验品质审查：搜索命中与实时操作目标

从 `f35e7290738677dd4f3325761a77369491dad55f` 继续收口Q21/T06。搜索结果明确表示
“已加载的查询时命中，正文实时更新；重新搜索获取当前匹配”，不在客户端重新实现
PostgreSQL匹配。Qt使用已有message_model维护唯一正文/版本/墓碑，窄proxy仅保存返回过的
ID membership并排除deleted；未知reaction只在完整正文到达前暂存独立revision。
分页cursor来自当前原始返回页，过滤为空仍能加载更早页；live事件不覆盖请求反馈。
Qt和TUI均在容器变化前临时捕获选中ID，变化后恢复同一对象或最近邻，不增加长期选中状态。
已打开Qt复制菜单仍复制原捕获正文，不因删除/插页移交到替代行。

新增永久断言实际先RED后GREEN，原断言保留。真实双TUI六档宽度删除前项后仍选MID，
可复制正文和未重贴草稿发送与SDK逐byte一致；旧binary同动作实际RED，误选LOW。
真实双Qt四档缩放完成编辑、删除、显式重搜、OSclipboard和草稿发送，final3实际exit0；
AT-SPI结果行1→1→0→0。两次临时driver失败分别为关闭XID枚举和HiDPI坐标换算，
保留失败证据，只修driver，不改产品迎合探针。主代理亲看最终原图，独立review亲看20张。

最终正式 `tests/verify.sh` Qt/TUI ON，normal/ASan/UBSan各23/23，
105.25/140.13/132.08s，runner实际exit0；全新Debug `-Werror`构建及23/23 CTest通过，
104.74s。无编译告警、sanitizer报告、skip或timeout放宽；render0.85/4.02/1.74s，原5秒不变。
本阶段真实Qt/X11导航4/4、TUI/tmux导航11/11。详细清理证据、前后原图和范围边界见
[体验品质审查](experience-quality.md)。未改server/client/SQL/third，无migration或push。

Q21/T06专项修复不代表整个品质目标完成。fresh-empty两条近义提示仍是轻度polish候选；
TUI ZWJ行仍有右边界残留，outer terminal shaping/T04未完成。Qt87/TUI78不提高，
全页面、完整日常使用、无障碍和连续两轮独立全产品fresh review仍继续开放。

## 体验品质审查：操作目的与搜索反馈

从 `00dff64089551e3e452d661162255de462079b05` 继续。九个Qt入口提供稳定操作名称，
profile仍使用真实用户名身份。搜索初始/loading/error/fresh-empty只有一条请求反馈，
live删除归零继续邀请重搜；不保存额外状态，不改变消息或查询语义。
两项永久widgets断言各实际RED1→GREEN0，原断言全部保留。
正式normal新binary `c3081ac1…b0946`的双Qt四档原生AT-SPI、OSclipboard、SDK编辑/
删除/重搜/草稿发送同轮通过，driver实际0；100%资料/联系人/添加好友原生Press成功。
前三次临时driver失败及实际清理证据保留，未修改产品迎合驱动。

全新Debug `-Werror`Qt/TUI ON构建和23/23 CTest通过，107.43s；当前源码正式
`tests/verify.sh` normal/ASan/UBSan各23/23，106.49/144.97/134.25s，runner实际0且
源码SHA guard成功，原5s render超时未放宽。Qt/X11导航4/4、TUI/tmux导航11/11通过，
自有DB/PID/ports/socket精确清理；长期服务未动，无push、无third改动。
原图、专项范围及失败边界见[体验品质审查](experience-quality.md)。

Q22操作目的专项收口，Q23消息行sender/time无障碍上下文仍缺失；T04 outer宽度/shaping、
全页面/完整日常使用/动态announce和连续两轮全产品fresh review未完成。Qt87/TUI78不提高。

## 体验品质审查：原生消息上下文

从 `6519e9d3abe4051636a167df5b77bd40dd87802b` 继续。唯一message_model按需提供
AccessibleTextRole的身份/日期/正文与现有引用、提及、编辑、反应、自己的已读信息；
DisplayRole/text_role/复制/geometry不变，墓碑不泄露正文、引用或附件文件名。
模型永久回归actualRED1→GREEN0，公开QAccessible/widgets和原断言通过。
offscreen model-reset缓存cell空Name的诊断失败保留；没有强制bridge或人工清缓存，
真实AT-SPI同dialog非空重建、编辑后重搜、删除必须另行核验。

双Qt最终完整final3 A100/125/150/200%、C100% actual0，五次/proc binary同a97470a3…a6ad，
Name与真实SDK上下文一致；原文/编辑OSclipboard逐byte、草稿恢复发送、profile等真实动作
通过。前两driveractual1分别为目标未完全到底、临时attachment arrival条件放错位置，
没有为测试改产品或放宽read-position断言；三库/记录PID/18901精确清理、长期服务未动。
主亲看四张原图，专项不是Orca语音/动态announce/全产品a11y验收。

同期13份源码guard一致，fresh Debug Qt/TUI ON -Werror构建及23/23 CTest109.59s；
正式tests/verify.sh normal/ASan/UBSan各23/23，104.67/135.04/129.09s，原runner actual0。
fresh与正式编译选项分别核验，未称正式脚本也有-Werror；无告警/sanitizer报告/skip/
timeout放宽。真实导航Qt4/4、TUI11/11 actual0，主只读cleanup核精确DB/PID/port/socket。
详见[体验品质审查](experience-quality.md)。无server/client/SQL/third改动或push。

Q23 sender/time上下文专项收口；group可见读者metadata等价性、keyboard-to-latest与
Qt emoji字体候选仍需处理。Qt87/TUI78不提高，T04和整体品质目标继续推进。

## 体验品质审查：通用肤色附着关系

把此前隔离context modifier研究正式接入现有FTXUI维护补丁。Unicode18官方属性数据/
派生表/完整许可固定入仓，offline确定性生成器与CMake pin核验版本；正常配置无Python，
TUI OFF不执行property模块或下载utf8proc。显示只承认紧邻base/单VS16例外，非法肤色
仅display使用载体，原文/字素编辑/复制/SDK字节不变，无新长期状态或third修改。
永久display2788断言actualRED1（465 failures）→GREEN0，unicode/display/span3/3，
官方853条分界和旧断言保留；TUI OFF/checksum拒绝/生成表exact-check均实际核验。

新生产archive正常链接helper在四terminal链18×2×4行实际执行0，outer像素与DSR证明
前五类nonbase关系归80列；合法hand/ZWJ残余失败保留，不称全144行列正确。
两轮双真实TUI60/80/160列各21facts actual0，SDK原UTF8/草稿精确；摄影轮39actual outer图。
主亲看载体、copyable、草稿与160列合法ZWJ，仍见ENDN/边界残留，T04不关闭。
copyable display不冒充OSrawclipboard；受控xterm15不冒充自然0。
主独立checker actual0核两DB0/18902无listener/记录PIDgone/自有socket无server，未动长期服务。

同13份冻结源码fresh全构建-Werror23/23及正式normal/ASan/UBSan各23/23已通过，
source guard一致，原5s render未放宽。完整证据/原图/失败边界见
[体验品质审查](experience-quality.md)。未push，Qt87/TUI78不提高；T04与整体目标继续推进。
