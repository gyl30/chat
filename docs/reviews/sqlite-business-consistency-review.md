# SQLite 业务一致性专项审查

## 1. 审查基线与环境

2026-11-19 执行 `git fetch origin` 后，`main`、`HEAD` 和 `origin/main` 均为 `89daf1c3814011ca58564d658f5e84b2aa671b05`（精简群邀请与对象存储错误传递）。工作树起始干净，给定基线之后没有新提交。审查以该提交的生产源码、当前 README 和现行业务设计为依据。历史文档中的 PostgreSQL、TUI 和旧测试结果用于追溯。 [现行 README](../../README.md#L1-L21) · [开发状态](../development-status.md#L68-L96)

验证环境为 GCC 16.2.0、CMake 3.31.10、SQLite 3.37.2、Qt 6.5.3 和 Node.js 22。运行检查使用内存 SQLite 或临时目录中的独立库，WebSocket 连接使用回环地址。未读写开发库，未访问真实 Silo 桶。

**结论**　当前证据未发现 P0 或 P1。确认两项 P2 应用表现、一项 P3 数据库约束缺口。另有一项 P2 S3 回收风险待实测、一项 P2 申请处理语义待确认。消息去重、好友状态转换、群主转让和读位单调性均有本轮实测证据。

## 2. SQLite 数据模型及业务不变量

| 范围 | SQLite 保证 | 业务层额外保证 |
| --- | --- | --- |
| 用户和头像 | 用户名唯一，头像按用户唯一，BLOB 长度和媒体类型受约束。 | 账号语法、图片校验及头像状态与 revision 同步。 [Schema](../../src/schema.cpp#L9-L23) · [用户存储](../../src/user_store.cpp) |
| 好友及私聊 | 定向 contacts 主键，好友申请按无序用户对唯一，direct 按规范化用户对唯一。 | 接受和删除好友分别原子写入或清除双向关系，创建 direct 时同时建立成员记录。 [Schema](../../src/schema.cpp#L25-L43) · [私聊事务](../../src/conversation_store.cpp#L63-L117) |
| 群和成员 | 每群一列 owner，非空 owner 受延迟复合外键约束为当前成员。成员和待审批按会话、用户唯一。 | 创建时设置 owner，禁止群主直接退出或被移除，最多三位管理员。Q3 记录空 owner 约束缺口。 [Schema](../../src/schema.cpp#L45-L85) · [角色事务](../../src/conversation_store.cpp#L445-L578) |
| 消息、回复和置顶 | 消息 ID 全局递增，非空 `client_id` 按发送者唯一，撤回正文保持为空，回复和置顶引用受外键约束。 | 检查目标属于当前会话且未撤回。撤回保留回复引用并解除群置顶。 [Schema](../../src/schema.cpp#L45-L68) · [消息和索引](../../src/schema.cpp#L87-L102) · [写入校验](../../src/message_store.cpp#L48-L75) |
| 附件、回应、提及 | 每消息至多一条附件元数据，object key 全局唯一。撤回触发删除附件元数据并写入回收记录。每用户每消息至多一个回应。 | S3 写入前登记预留，发布事务一次性领取预留并写入元数据。撤回同步清除回应、提及和置顶。 [Schema 与触发器](../../src/schema.cpp#L104-L152) · [发布](../../src/message_store.cpp#L117-L145) · [撤回](../../src/message_changes.cpp#L45-L93) |
| 已读和个人偏好 | 已读及入群水位非负，mute 和 pin 属于成员行。 | 当前成员只能用同会话消息更新自己的读位，更新取 `max`。入群水位仅参与未读基线。 [Schema](../../src/schema.cpp#L74-L83) · [读位写入](../../src/message_history.cpp#L246-L282) |

导入数据或新增写入器时，还要遵守应用层保证的好友双向性、管理员数量、成员读位、回复和置顶等规则。

## 3. 消息生命周期

- **创建与重试**　`send_message → persist_delivery → insert_message` 在同一 `BEGIN IMMEDIATE` 下查找 `(sender, client_id)`、检查权限、写入、生成 ACK 和通知、提交。重放已有键返回同一消息的当前状态。换到其他会话，或匹配另一种仍有效的消息类型时拒绝。不同发送者可使用同一键。删好友后原作者仍可重放已发布消息，群成员已被移除则拒绝重放。并发、重试及重启后重放均实测。 [发送事务](../../src/send_message_request.cpp#L96-L157) · [重放和权限](../../src/message_store.cpp#L9-L79) · [待发设计](../outgoing-messages-design.md#L26-L43)
- **编辑、撤回、回复、置顶**　编辑要求作者、当前成员和发送权限，附件消息正文不可编辑。同一作者连续编辑按最后提交生效，`edited_at` 单调。撤回要求作者与成员身份，事务内清除正文、回应、提及、匹配的置顶和附件访问。重复撤回不会再次增加 revision。撤回先提交时后续回复及置顶拒绝，回复先提交时引用继续显示撤回占位。生产 SQL 和 WebSocket 验证了撤回、旧引用、搜索及重放，未做完整的置顶交错运行。 [编辑和撤回](../../src/message_changes.cpp#L25-L93) · [事务和通知](../../src/update_message_request.cpp#L43-L103) · [回复校验](../../src/message_store.cpp#L60-L77) · [群置顶](../../src/conversation_store.cpp#L324-L369)
- **回应**　当前可发送者只变更自己的回应。写入、递增 revision 和构造返回值由同一事务完成。重复设置同一表情为无变更，撤回后拒绝新回应。两个发送者并发设置后保留两条回应。Qt 依 revision 合并，旧回应不能恢复已撤回状态。 [回应写入](../../src/message_changes.cpp#L96-L155) · [回应事务](../../src/reaction_request.cpp#L45-L103) · [Qt 合并](../../qt/src/message_model.cpp#L375-L435)
- **历史、搜索、会话预览**　固定消息 ID 前后翻页，每页在同一读事务内拿到成员、读位、消息和相关回应、提及。搜索过滤已撤回正文，历史和会话摘要保留撤回占位，旧回复继续引用当前正文或撤回占位。Q1 涉及历史搜索窗，Q2 涉及会话列表跨页。 [历史事务](../../src/message_history.cpp#L133-L213) · [会话摘要](../../src/conversation_list.cpp#L37-L65)
- **附件**　先登记对象 key，S3 PUT 后确认 SHA-256，HEAD 确认长度。最终发布事务重新检查权限、回复目标和已发布键，再领取预留并写入消息及附件元数据。任何一步 SQLite 失败都会回滚整个发布事务。下载每块按当前成员和有效消息检查，已授权的在途块可随 S3 响应完成。撤回后拒绝后续块，物理清理异步完成。Q4 涉及慢上传和回收。 [附件发布](../../src/send_message_request.cpp#L193-L251) · [S3 校验](../../src/s3_objects.cpp#L58-L88) · [下载授权](../../src/attachment_access.cpp#L5-L28) · [分块下载](../../src/attachment_io.cpp#L80-L105)

## 4. 好友关系和私聊

- 交叉发送好友申请只产生一条待处理记录，反向发起者看到已有 incoming。只有接收方可接受或拒绝，只有申请者可取消。接受先提交时两条 contacts 同时存在，之后取消不会解除好友。取消先提交时后续接受返回申请不存在。撤销或拒绝后再申请的迟到动作涉及 Q5。 [好友事务](../../src/friendship_store.cpp#L51-L173) · [响应及通知](../../src/contact_request.cpp#L75-L136)
- 并发打开同一好友私聊复用同一 ID。open 和 send 权限都与写入同处写事务。删好友先提交时，后续 open 或 send 被拒绝。send 先提交时，消息留在历史。已有连接继续受实时授权约束，编辑、回应和 typing 也查当前关系。已经分块上传的附件仍需通过最终发布授权。 [Direct 创建](../../src/conversation_store.cpp#L63-L117) · [发送权限](../../src/conversation_store.cpp#L243-L257) · [Typing 授权](../../src/chat_session.cpp#L81-L107) · [附件发布](../../src/send_message_request.cpp#L238-L251)
- **BY DESIGN**　删除好友保留双方的历史、旧附件、读位和个人偏好，聊天列表隐藏该 direct，服务器继续允许历史、搜索、旧附件、已读和撤回本人旧消息。好友待确认不进入联系人或聊天列表。当前没有拉黑或解除拉黑 RPC。 [产品规则](../development-status.md#L68-L96) · [会话过滤](../../src/conversation_list.cpp#L11-L21) · [RPC 路由](../../src/chat_session.cpp#L194-L279)

## 5. 群组成员和权限

- 建群在同一事务写入群主和初始成员。手工邀请校验操作者当前群角色和自己的好友关系，读取最新消息水位、写入成员和清除申请由同一事务执行。重复邀请保留已有成员的读位和偏好。 [建群](../../src/conversation_store.cpp#L120-L159) · [手工邀请](../../src/conversation_store.cpp#L376-L441)
- 提升、降级、转让、移除、退群均在取得写事务后检查成员、角色和目标。并发提升无法同时通过管理员数检查。转让对调群主与目标的管理员标志，再设成员为 owner，在同一事务提交。实际并发发起转让和移除后群主唯一，已退出账号发起的后续操作被拒绝。退出、移除与邀请的最终成员状态由提交顺序决定，重新加入建立新成员行。 [角色事务](../../src/conversation_store.cpp#L445-L578)
- 邀请码入群不创建好友。撤销先提交时，旧码不能再新建成员或申请，已提交的申请仍可审批。手工邀请直接入群并消耗待处理申请。审批通过把消费申请和写入成员绑定在同一事务，同一条待处理记录只能被消费一次。申请被消费后又重建的情况见 Q5。 [邀请和撤销](../../src/group_access_store.cpp#L80-L152) · [入群](../../src/group_access_store.cpp#L155-L230) · [审批](../../src/group_access_store.cpp#L317-L351)
- 公告和群置顶先检查当前角色，再写群资料。公告不写消息表，不改变活动时间和未读。当前没有解散群组的生产 RPC。外键级联能删除部分关联记录，当前无解散通知或客户端清理流程可验证。 [群资料更新](../../src/conversation_store.cpp#L288-L374) · [群路由](../../src/chat_session.cpp#L240-L257)

## 6. 已读位置与未读统计

- `mark_read` 用一条带成员和同会话消息条件的 UPDATE 取 `max(last_read_message_id, id)`，只更新认证用户的成员行。实测新后旧仍返回较新读位，非成员和其他会话消息拒绝更新。查询的 `max` 基于当时行值。同账号第二设备同时认证返回 `-32004`，当前未开放双设备同时阅读。 [读位 SQL](../../src/message_history.cpp#L246-L282) · [RPC 结果](../../src/mark_read_request.cpp#L87-L111) · [认证](../../src/authenticate_request.cpp#L83-L99)
- 列表和单会话以同一口径统计 `id > max(last_read_message_id, joined_message_id)` 的未撤回消息，排除自己发送的消息，保留历史 self-direct 特例。 `count(*)` 保证非负。撤回后未读可下降，分页本身不写读位，两次查询之间的提交仍可改变未读。 [列表未读](../../src/conversation_list.cpp#L61-L63) · [单会话未读](../../src/conversation_store.cpp#L217-L240)
- 退出或移除删除读位。复入以最新消息为未读基线，实际读位从 0 开始。Qt 只把当前成员的实际读位用于已读详情。群变更和断开时清理旧群读位，之后合并新快照。未覆盖断开、退群复入与多轮分页同时交错的 Qt 场景。 [复入 SQL](../../src/group_access_store.cpp#L49-L54) · [Qt 读位](../../qt/src/message_model.cpp#L438-L492) · [群变更处理](../../qt/src/main_window.cpp#L911-L926) · [断开处理](../../qt/src/chat_widget.cpp#L1437-L1444)

## 7. SQLite 事务与并发

| 检查 | 结论及边界 |
| --- | --- |
| 执行模型 | 网络循环把任务提交到容量 1024 的数据库队列。默认单 worker、单连接，任务串行执行，结束后投递 continuation。使用 FULLMUTEX，服务持有库文件排他锁。S3 通过独立 worker。 [数据库](../../src/database.hpp#L19-L53) · [Worker](../../src/blocking_worker.hpp#L20-L65) · [打开连接](../../src/sqlite_database.cpp#L87-L150) · [文件锁](../../src/main.cpp#L215-L230) |
| 跨 `co_await` | SQLite 事务在执行线程的同步任务内完成，没有发现业务事务持锁等待 WebSocket 或 S3。附件预检之后可以有其他事务改变授权，最终发布事务重新检查。 [数据库入口](../../src/database.hpp#L24-L41) · [附件 RPC](../../src/send_message_request.cpp#L202-L251) |
| 写事务与单语句 | 好友、会话、角色、入群、消息、回应和读位使用 `BEGIN IMMEDIATE`。个人偏好把成员条件直接写入 UPDATE。权限、管理员数量等应用层判断和实际写入在同一事务。 [事务封装](../../src/sqlite_database.cpp#L334-L363) · [个人偏好](../../src/conversation_store.cpp#L204-L214) |
| 读快照 | 历史、搜索、群申请列表在各自任务内取同一快照。会话列表每页一致，跨页可变排序见 Q2。 [历史快照](../../src/message_history.cpp#L133-L213) · [申请快照](../../src/group_access_store.cpp#L263-L314) · [列表查询](../../src/conversation_list.cpp#L10-L70) |
| 提交、回滚和复用 | COMMIT 错误返回调用方，RAII 守卫回滚仍活动的事务。回滚失败且事务仍活动时关闭连接，下一任务重开。启动验证 WAL 和外键，设置 `synchronous=FULL`。本轮验证了延迟外键导致的 COMMIT 拒绝及回滚，另验证正常重启。未注入实际磁盘写故障和 ROLLBACK I/O 失败。 [连接参数](../../src/sqlite_database.cpp#L113-L150) · [回滚](../../src/sqlite_database.cpp#L352-L382) · [重开](../../src/database.hpp#L27-L41) |
| 停止 | 网络循环先驱动会话退出和周期回收停止，随后最终回收。worker 停止接收并排空队列。实测正常停止和重启，未覆盖满队列关闭和 S3 传输中进程崩溃。 [停止顺序](../../src/main.cpp#L255-L271) · [队列关闭](../../src/blocking_worker.cpp#L14-L71) |

## 8. 数据库与实时通知

- 消息、回应、好友、群与读位变更先提交，再发通知。消息发送、编辑、撤回和回应在提交前构造关键载荷。回执连接断开后，同一键仍可重放已提交消息，接收方也可从历史取得。队列溢出会关闭慢连接，通知查询失败影响当次投递。 [消息提交](../../src/send_message_request.cpp#L134-L157) · [发布](../../src/send_message_request.cpp#L260-L290) · [队列](../../src/chat_session.cpp#L41-L52) · [通知查询](../../src/chat_session.cpp#L81-L149)
- 数据库写入顺序由单 worker 串行化，接收者查询另开任务。不同源会话的通知可能按不同于提交的时刻到达。群移除会清理该会话的内存上传并通知离开者。好友通知提示刷新，群审批通知发布前检查当前申请和成员状态。 [好友通知](../../src/contact_request.cpp#L104-L120) · [群移除](../../src/group_management_request.cpp#L210-L229) · [审批通知](../../src/group_join_request.cpp#L121-L166)
- 在线用户表按连接身份移除，旧离线通知查到新连接时跳过。Qt 通过连接代次隔离旧结果，重拉联系人、申请、会话和当前聊天。恢复游标从已加载的最早消息之前开始，能重获该范围中的编辑、撤回及回应状态。离线通知本身不持久重播。 [离线边界](../../src/chat_session.cpp#L54-L79) · [在线表](../../src/online_users.cpp#L4-L26) · [旧结果过滤](../../qt/src/client_bridge.cpp#L649-L664) · [恢复请求](../../qt/src/main_window.cpp#L1694-L1706) · [恢复游标](../../qt/src/chat_widget.cpp#L2529-L2533)

## 9. 测试覆盖及本轮验证

| 验证 | 实际结果 |
| --- | --- |
| Debug 生产构建 | `cmake --build build --target chat_server -j12` 与 `cmake --build build --target chat_qt -j12` 均通过。 |
| 当前 CTest | 在 `/tmp` 新目录执行 `cmake -S . -B "$audit_build" -DCHAT_BUILD_QT_CLIENT=ON`，沿用现有 Qt、MinIO 依赖路径和本地 utf8proc 源。配置成功。 `ctest --test-dir "$audit_build" --show-only` 显示 0 项，`ctest --test-dir "$audit_build" --output-on-failure` 显示无测试。旧 `build` 登记的 31 项属于已删除的历史套件，未运行。 [README](../../README.md#L9-L21) · [CMake](../../CMakeLists.txt#L9-L13) |
| 临时库、真实 Server 和 WebSocket | Node.js 内置 WebSocket 连接三个临时账号。验证交叉好友申请、接受与取消竞争、双向并发 open、同账号重复认证、重复 `client_id` 与不同作者并发发送、读位单调与成员授权、回应 revision、撤回后引用及搜索、群主转让与移除竞争、删好友后禁止发送但保留历史。SIGTERM 正常停止，重启后历史及重放正确，`integrity_check` 为 `ok`，`foreign_key_check` 无行。只发文本及相关操作，未访问 S3。 |
| 内存 SQLite | 加载真实建表 SQL 并执行生产读位、列表及附件回收 SQL。验证 direct、申请、客户端键唯一性，读位授权与跨会话拒绝，延迟外键失败及回滚，附件撤回触发器的提交与回滚，复现 Q2、Q3，并验证 Q4 的数据库侧状态。 |
| 真实 Qt 组件 | Qt 6.5.3 offscreen 加载当前构建的 `chat_history_dialog` 对象文件。用真实窗口触发查询切换、旧响应和再次搜索，复现 Q1。未运行完整桌面端到端场景。 |
| 历史覆盖 | `8682f89^` 的旧 `tests/database_test.cpp` 曾覆盖并发写、回滚失败后连接复用、群邀请与发送竞争、审批争抢及消息重放。相关文件在 `8682f89` 删除。 |
| 未执行 | 真实 Silo 故障与慢队列、SQLite 磁盘 I/O 故障、满队列强制关闭、WSS、ASan/UBSan 和持续多客户端压力。 |

## 10. 问题清单与证据

### Q1　P2 · CONFIRMED　切换历史搜索时永久停在加载

- **位置和调用链**　历史窗搜索框 Enter 或清除关键词 → `request_more` → history/search RPC → `set_history` 或 `set_search_results`。 [搜索输入](../../qt/src/chat_history_dialog.cpp#L189-L207) · [请求门限](../../qt/src/chat_history_dialog.cpp#L238-L247) · [响应判定](../../qt/src/chat_history_dialog.cpp#L249-L277) · [Qt 桥接](../../qt/src/main_window.cpp#L841-L853)
- **触发和复现**　首个历史请求返回前输入 `alpha` 并按 Enter。交付首个历史结果，再输入 `beta` 并按 Enter。Qt 组件实测只发出 1 次历史请求、0 次搜索请求，窗口保持“正在加载”。断开处理重置 loading 后，再次 Enter 才发出搜索请求。
- **影响**　当前历史窗无法搜索已持久化的消息。重开历史窗或断开后重试可恢复。
- **根因**　切换查询清除了列表却保留旧请求持有的 `loading_`。新请求被该标志拦住，旧响应又因查询已变而提前返回，无法结束加载。
- **最小修复及测试覆盖**　切换时结束旧加载并请求新页，按请求身份拒绝迟到响应，覆盖切换回同一词的时序。当前无跟踪测试覆盖，本轮临时 Qt 组件验证已复现。

### Q2　P2 · CONFIRMED　跨页活动变化漏掉会话，Qt 可能关闭仍可访问的聊天

- **位置和调用链**　发送更新 `conversations.activity` → 列表 RPC 按活动排序 → bridge 串行合并多页 → `chat_widget::set_conversations` 应用聚合列表。 [活动写入](../../src/message_store.cpp#L147-L154) · [排序与游标](../../src/conversation_list.cpp#L11-L21) · [合并分页](../../qt/src/client_bridge.cpp#L217-L267) · [关闭缺席会话](../../qt/src/chat_widget.cpp#L1487-L1502)
- **触发和复现**　用户有 51 个会话，X 初始排第 51。取第 1 页后，有成员在 X 发送消息，X 移到旧游标之前。用第 1 页的游标继续查询。本轮实际执行生产 SQL，第 1 页 50 行，第 2 页 0 行。库中仍有 51 个成员关系，重新查首页即包含 X。若 X 正是用户正在阅读的会话，接收消息的分支不会主动重拉列表。 [接收消息后的刷新条件](../../qt/src/main_window.cpp#L1002-L1015)
- **影响**　列表可暂时漏掉仍有访问权限的会话。若 X 为当前会话且缺项列表被应用，Qt 会关闭聊天，用户需刷新后再打开。关闭分支已读代码确认，未编排 Qt 与两页 RPC 的完整时序。会话、消息、读位和草稿仍保留。
- **根因**　每页独立读取可变排序键，Qt 直接按聚合列表确认活动会话缺席。现有设计已承认跨页漏项，并保留草稿和待发数据。本项针对界面的误关闭。 [待发与草稿约定](../outgoing-messages-design.md#L64-L72)
- **最小修复及测试覆盖**　活动会话从聚合列表缺席时，核验其可见性或刷新后再决定是否关闭。明确移除或好友删除仍然及时关闭。现无跟踪测试覆盖，生产 SQL 已复现第 51 项跨过游标。

### Q3　P3 · CONFIRMED　SQLite 允许群的 owner 为 NULL

- **位置和调用链**　初始化建表 → group 分支 CHECK → 延迟复合外键。group 分支没有 `owner_id IS NOT NULL`。生产 `create_group` 和 `transfer_group_owner` 都显式写入成员 ID。 [群 CHECK 和外键](../../src/schema.cpp#L45-L68) · [建群](../../src/conversation_store.cpp#L120-L159) · [转让](../../src/conversation_store.cpp#L542-L557)
- **触发和复现**　内存审查库开启外键，插入标题、活动时间合法且 owner 为 NULL 的群，SQLite 接受。对照插入非空 owner 且该用户尚非成员的群，COMMIT 被拒绝、回滚后记录消失。未找到可通过现有生产 RPC 写入空 owner 的路径。
- **影响**　若手工导入或未来写入器遗漏 owner，数据库可持久化无主群，群主专属管理动作无法进行。
- **根因**　同一列需允许 direct 无 owner，group 的条件 CHECK 没有补齐必填约束。复合外键在子键含 NULL 时不阻止该行。
- **最小修复及测试覆盖**　先扫描已有群，再在独立 schema 变更中补非空约束。当前无跟踪测试覆盖，本轮已用真实 DDL 复现。

### Q4　P2 · POTENTIAL　慢上传与回收交错可能遗留未登记对象

- **位置和调用链**　`finish_attachment → stage_attachment → reserve → PUT/HEAD` 与 `recover_objects → claim_attachment_cleanup_batch → DELETE → acknowledge` 跨资源交错，失败后进入 `discard_unpublished_attachment`。 [预留与 PUT](../../src/attachment_io.cpp#L42-L77) · [回收周期和截止时间](../../src/main.cpp#L59-L87) · [领取和确认](../../src/attachment_cleanup.cpp#L28-L70) · [发布领取](../../src/message_store.cpp#L117-L132) · [失败清理](../../src/attachment_io.cpp#L20-L39)
- **触发和时序**　预留在 S3 排队前创建，回收按一分钟年龄领取。若 PUT 仍在执行或超时后仍可能在 Silo 完成，且 Silo 对当时不存在的对象返回删除成功，回收可能先确认并删除预留，之后 PUT 才成功。最终发布因无预留而回滚，失败清理又查不到该对象的登记。本轮验证了 SQL 领取和确认能去掉唯一持久跟踪键，未验证实际的 Silo 对象时序。 [S3 worker](../../src/s3_objects.hpp#L47) · [超时设置](../../src/s3_objects.cpp#L14-L15) · [PUT 和 HEAD](../../src/s3_objects.cpp#L70-L88)
- **影响**　附件发送可能失败，并遗留无消息引用、无回收记录的对象。原先的发布领取与回收领取互斥，未见该时序删除已发布对象。
- **根因**　预留年龄同时用于决定何时回收和判断 PUT 是否仍可能成功。年龄只能支持前一个判断。
- **最小修复及测试覆盖**　先在临时 Silo 上按明确顺序复现。若成立，使活跃 PUT 与回收领取互斥，并保留超时后仍可能完成的对象跟踪。当前没有跟踪的 Silo 测试。

### Q5　P2 · NEEDS CLARIFICATION　过时的申请操作可作用于同一人的新申请

- **位置和调用链**　好友 `respond_friend_request → change_friendship` 按用户对处理当前 incoming。群 `respond_group_join_request → respond_to_group_join_request` 按群和申请人消费当前行。两类记录均有创建时间，响应参数未标识操作的是哪一次申请。 [好友参数](../../src/contact_request.cpp#L16-L24) · [好友读取](../../src/friendship_store.cpp#L60-L98) · [好友响应](../../src/friendship_store.cpp#L121-L153) · [群参数](../../src/group_join_request.cpp#L35-L45) · [群列表与消费](../../src/group_access_store.cpp#L275-L351)
- **触发和时序**　接收方看到 A 的好友申请。A 取消再发，接收方对原记录的接受在新申请写入后到达。群内对应情形是一位管理员看到申请，另一位先拒绝，申请者重提，前一位管理员的旧响应后到。每次操作仍检查当前权限，同一条持续存在的 pending 只消费一次。
- **影响**　如果语义要求“处理当时展示的那一次申请”，迟到的接受或拒绝可能作用于新申请。若定义为“处理此人当前的待处理申请”，现有结果符合契约。
- **根因**　请求只携带主体键，响应接口无法区分两次申请。
- **最小修复及测试覆盖**　先确定契约。若需绑定特定申请，使用稳定申请身份，在事务中条件消费，旧结果触发刷新。旧测试曾覆盖同一申请争抢，本轮未运行跨代端到端时序，当前无跟踪测试覆盖。

## 11. 风险汇总

| 等级 | 已有证据 | 剩余验证 |
| --- | --- | --- |
| P0/P1 | 审查和定向实测未发现确定的持久化破坏、重复消息或权限绕过。 | SQLite 磁盘故障、非正常退出、满队列和持续并发仍需单独验证。 |
| P2 确认 | Q1 真实 Qt 组件复现。Q2 生产 SQL 复现，Qt 关闭行为有明确分支。 | 完整桌面端到端时序未编排。 |
| P2 待定 | Q4 的数据库侧状态成立，Q5 的消费条件已核实。 | Q4 需真实 Silo 故障验证，Q5 需确定当前申请和特定申请的契约。 |
| P3 确认 | Q3 由实际建表 SQL 复现。 | 未找到受影响的生产写入，也未扫描生产库。 |
| 设计边界 | 单账号单连接，通知尽力投递，非好友历史保留，邀请码撤销保留已有申请。 | 拉黑、群解散和同账号多设备同时在线当前没有产品入口。 |

## 12. 后续修复优先级

1. **Q1**　先修历史搜索卡死，以切词、迟到响应和切回原词的时序验证。
2. **Q2**　确认活动会话缺席时的可见性。验证跨页漏项、删除好友和成员被移除的不同结果。
3. **Q4**　先用临时 Silo 验证迟到 PUT 和回收确认。若复现，调整领取及确认的边界。
4. **Q5、Q3**　确认 Q5 的申请身份语义。若需要强化数据库约束，Q3 独立变更并在应用前检查现有群。

本轮新增此报告，未修改生产代码或既有测试，未提交、推送或部署。仓库本地 `.git/info/exclude` 的 `/docs/` 规则会忽略此新文件，因此普通 `git status` 不显示报告。
