# 当前核心 IM 路线完成记录

本记录对应 2026-10-01 的仓库实际历史。阶段提交和 push 结果以 Git 历史为准，Telegram 调研与裁剪依据见 [调研记录](telegram-group-design-research.md)。

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

另外完成历史大响应接收、编辑消息布局和消息操作按钮对比度修复，分别见 `32f3f3b`、`a545c9a`、`f73b8f4`。

## 数据模型

- `users` 保存账号与最后在线时间；`contacts` 是单向关系。
- `conversations.kind` 显式区分 `direct/group`；单聊使用真实的两端 user ID，群使用会话 ID、群名和 `owner_id`。
- `conversation_members` 表示当前成员，持有 `is_admin`、`last_read_message_id` 和 `joined_message_id`。群主必须是群成员，由延迟外键保证；管理员上限在同一会话锁事务内检查。
- `messages` 指向会话和真实作者，包含回复 ID、编辑时间、删除占位；`message_attachments` 保存附件元数据和内容。删除附件消息会清除文件内容。
- SQL 008 保留旧单聊、自聊、消息、联系人和阅读位置；SQL 009–014 渐进增加上述能力。SQL 013 应用前已确认本次数据库没有既存群，不猜测旧群创建者，也不删除旧消息。
- 新建或重新加入的成员能读取完整历史。邀请取得会话锁后读取最新消息 ID 作为加入水位；实际阅读仍从 0 开始，仅由 `mark_read` 推进。未读统计使用 `id > greatest(last_read_message_id, joined_message_id)`，排除删除消息及群成员自己的消息。

## JSON-RPC 与 realtime

现有 JSON-RPC 2.0、WebSocket 和 cursor pagination 保持统一。废弃的成员 `users` 响应已改为明确的 `members`，不保留双格式兼容。

| RPC | 主要参数或结果 |
|---|---|
| `open_direct_conversation` / `create_group` | 真实用户 / 群名及联系人 ID 列表；返回会话 ID |
| `get_conversations` | 活动时间和会话 ID cursor；会话类型、人数、最新消息、独立未读 |
| `get_messages` | 会话及互斥的 `before/after` 消息 ID；消息、当前成员实际阅读位置、`has_more` |
| `mark_read` | 会话和真实消息 ID；阅读位置只增不减 |
| `get_members` | `members: [{id, username, role}]`，角色为 `owner/admin/member` |
| `set_group_admin` | `conversation, user, admin`；仅群主；返回 `changed` |
| `rename_group` | `conversation, title`；群主/管理员；返回 `changed` |
| `invite_group_members` | `conversation, members`；群主/管理员邀请自己的联系人；重复邀请不重置成员状态 |
| `leave_group` | `conversation`；普通成员/管理员自退，群主拒绝退出；返回 `changed` |
| `send_message` / `edit_message` / `delete_message` | 会话、真实消息或回复 ID；编辑/删除仅作者且仍为当前成员 |
| `search_messages` | 会话、字面查询和 `before` cursor |
| 附件 RPC | begin/upload/finish/cancel/get；32 KiB 分块，单文件最多 10 MiB |
| `set_typing` | 会话、开始/结束；不写消息或阅读位置 |

消息、编辑删除、已读和输入提示分别使用 `message/message_updated/read/typing` notification。群变化使用 `conversation` notification，包含真实会话 ID；事务提交后通知当前成员，退出者额外收到一次会话变化通知。之后的群访问被成员权限检查拒绝，后续群消息不再发送给退出者。

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

当前不做群主转让、踢人、入群审批、邀请链接、@mention、公告、mute、pin 或 reaction。群主不能直接退出；群主/管理员没有编辑、删除他人消息的权限。退出者本地活动历史清空；服务端仍保留群消息，重新加入可重新获取。

后续可以按真实使用需求独立评估群主转让/踢人、已读成员列表、附件存储规模和图片缩略图。它们不是本轮未完成项，不自动扩大到多设备、微服务、Redis、Kafka、event sourcing 或 CQRS。

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

已验证 3–200 人范围的完整成员、读位和发布路径；这是测量范围，不是人数上限或在线吞吐承诺。未引入大群基础设施。群主转让和成员移除属于下一独立阶段，此处不记作完成。
