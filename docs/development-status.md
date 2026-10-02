# 开发状态

本记录对应截至 2026-10-02 的仓库实际历史。阶段提交和 push 结果以 Git 历史为准，Telegram 调研与裁剪依据见 [调研记录](telegram-group-design-research.md)。

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

另外完成历史大响应接收、编辑消息布局和消息操作按钮对比度修复，分别见 `32f3f3b`、`a545c9a`、`f73b8f4`。

## 数据模型

- `users` 保存账号、最后在线时间和单调递增的 `avatar_revision`；`contacts` 是单向关系。`user_avatars` 保存一个当前头像，是否存在由数据行决定；不与消息附件共表。
- `conversations.kind` 显式区分 `direct/group`；单聊使用真实的两端 user ID，群使用会话 ID、群名和 `owner_id`。
- `conversation_members` 表示当前成员，持有 `is_admin`、`last_read_message_id`、`joined_message_id` 和个人 `muted/pinned`。群主必须是群成员，由延迟外键保证；管理员上限在同一会话锁事务内检查。
- `messages` 指向会话和真实作者，包含回复 ID、编辑时间、删除占位；`message_attachments` 保存附件元数据和内容。删除附件消息会清除文件内容。SQL 016 增加独立 `message_reactions` 和消息的单调 `reaction_revision`，每用户每消息一条回应，删除消息时清除回应。
- SQL 019 的 `message_mentions(message_id,user_id)` 保存群文字提及的真实目标；编辑替换、删除清空，退出成员不删除历史目标。旧消息不回填。
- SQL 020 的 `conversations.pinned_message_id` 保存每群一个当前置顶消息；与个人 membership 的 pinned 独立。初始为空，软删除同事务清空，物理删除通过外键 SET NULL 清空。
- SQL 021 的 `conversations.announcement` 保存每群一个当前纯文本公告，默认空字符串，空字符串表示未设置；数据库约束限定 group 和最多 4096 个 UTF-8 字节。没有公告历史或消息伪装。
- SQL 008 保留旧单聊、自聊、消息、联系人和阅读位置；SQL 009–014 渐进增加上述能力。SQL 013 应用前已确认本次数据库没有既存群，不猜测旧群创建者，也不删除旧消息。
- 新建或重新加入的成员能读取完整历史。邀请取得会话锁后读取最新消息 ID 作为加入水位；实际阅读仍从 0 开始，仅由 `mark_read` 推进。未读统计使用 `id > greatest(last_read_message_id, joined_message_id)`，排除删除消息及群成员自己的消息。

## JSON-RPC 与 realtime

现有 JSON-RPC 2.0、WebSocket 和 cursor pagination 保持统一。废弃的成员 `users` 响应已改为明确的 `members`，不保留双格式兼容。

| RPC | 主要参数或结果 |
|---|---|
| `open_direct_conversation` / `create_group` | 真实用户 / 群名及联系人 ID 列表；返回会话 ID |
| `get_conversations` | `(pinned, activity, id)` cursor；置顶优先，各层内活动时间和 ID 降序；会话资料、未读与个人 muted/pinned |
| `set_conversation_muted` | `conversation, muted`；只修改当前成员自己的偏好，返回当前 muted |
| `set_conversation_pinned` | `conversation, pinned`；只修改本人列表排序偏好，返回当前 pinned |
| `get_messages` | 会话及互斥的 `before/after` 消息 ID；消息、当前成员实际阅读位置、`has_more` |
| `mark_read` | 会话和真实消息 ID；阅读位置只增不减 |
| `get_members` | `members: [{id, username, role}]`，角色为 `owner/admin/member` |
| `set_group_admin` | `conversation, user, admin`；仅群主；返回 `changed` |
| `transfer_group_owner` | `conversation, user`；仅群主转给当前管理员；返回 `changed` |
| `remove_group_member` | `conversation, user`；群主移除 admin/member，管理员仅移除 member；返回 `changed` |
| `rename_group` | `conversation, title`；群主/管理员；返回 `changed` |
| `invite_group_members` | `conversation, members`；群主/管理员邀请自己的联系人；重复邀请不重置成员状态 |
| `leave_group` | `conversation`；普通成员/管理员自退，群主先转让再退出；返回 `changed` |
| `pin_group_message` / `unpin_group_message` | 会话及真实消息 ID / 会话；仅群主、管理员；返回 `changed` |
| `set_group_announcement` | `conversation, text`；群主/管理员设置当前纯文本，空字符串清除；返回 `changed` |
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

当前尚未实现入群审批或邀请链接。群公告、@mention、个人会话置顶和群内单条置顶消息已接入，个人会话置顶不等于群内置顶消息。群主必须先手动转让再退出；群主/管理员没有编辑、删除他人消息的权限。退出或被移除者本地活动历史清空；服务端仍保留群消息，重新加入可重新获取。移除不等于永久封禁，重新邀请恢复普通成员，旧管理员身份和真实读位不继承。

2026-10-02 启动新的长期路线：验证基线、群已读详情、reaction、图片气泡预览、桌面通知、会话 mute/pin、群 mention、群置顶消息、公告、邀请链接和审批，依序独立实施。此列表表示规划，尚未实现的阶段不计入已完成能力。范围仍不扩大到多设备、微服务、Redis、Kafka、event sourcing 或 CQRS。

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
