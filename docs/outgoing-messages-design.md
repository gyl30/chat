# 发送中消息（参考 Telegram）

状态：已与 Codex 评审三轮，第 3 轮 APPROVE；分阶段实施中（阶段 1：服务端、SQL、SDK）。

## 目标

按 Telegram 的方式发送文字和附件：按下发送后，输入框立即清空草稿和回复，消息以「发送中」出现在聊天记录末尾；正文、文件和回复关系都归这条待发消息所有。成功后换成服务端消息；断线后自动重发且不产生重复；被服务端拒绝的标为「发送失败」，用户可重发或删除。Qt 和 TUI 行为一致。

输入框状态从此不依赖迟到的 ACK，TUI 的 R1（旧附件 ACK 清掉同一目标的新回复）和 R2（取消回复后 ACK 留下已发正文）随之消失。

## Telegram 的做法（依据）

- `messages.sendMessage` 必须携带 `random_id`：“Unique client message ID required to prevent message resending”；服务端据此去重，并以 `updateMessageID{id, random_id}` 把本地消息映射到服务端 ID。（core.telegram.org/method/messages.sendMessage、/constructor/updateMessageID）
- tdesktop：用户确认后，`SendConfirmedFile` 依次开始上传、触发 `sendAction`、插入带 `replyTo` 的本地占位消息（`api/api_sending.cpp`）；界面收到 `sendAction` 即 `cancelReplyOrSuggest()` 清掉回复（`history/history_widget.cpp`）。上传结果只更新那条本地消息。
- Element Web：确认文件后立即清回复，不等待上传（`ContentMessages.ts`）；文字发送注释为 “Clear reply_to_event as we put the message into the queue; if the send fails, retry will handle resending.”
- TDLib：失败状态 `messageSendingStateFailed` 带 `error`、`can_retry`、`need_drop_reply` 等字段，由 `resendMessages` 重发。

## 现状

- 发送：`send_message{conversation,text,reply_to}`；附件 `begin_attachment` → `upload_attachment`* → `finish_attachment{upload,reply_to}`。服务端先检查上传完整性，再在会话行锁内检查发送权限和回复目标，最后插入（`src/send_message_request.cpp:150/214/225/238/279`）。提交后推送给其他成员；发送者不收推送，自聊除外。推送在响应之后的下一轮发送（`src/chat_session.cpp:203/358`）。
- 没有幂等键：ACK 在断线时丢失，客户端无法知道消息是否已保存，重发会重复。
- 文字发送的 ACK 只有 id、时间、回复和提及（`client/include/chat/message.hpp:62`），两端用提交时的正文拼出消息；附件 ACK 是完整 message。
- SDK 没有请求超时；断线时 `fail_pending` 以 `transport` 错误结束所有挂起请求。
- 服务端每个连接同时只允许一个上传（`chat_session::upload_` 单值，第二个 `begin_attachment` 返回 `-32008 Attachment upload in progress`）。TUI 按会话限制上传，两个会话同时上传时后一个会被服务端拒绝。
- 每个用户只允许一个在线连接（`online_users::add`），服务端逐请求 `co_await` 处理同一连接的请求，响应按请求顺序发出。
- TUI 用 `sending_` 门控发送和编辑，草稿和回复保留到 ACK。Qt 文字发送用 `message_sending_` 保留草稿和回复到 ACK；附件发送时即清回复，失败才恢复（`qt/src/chat_widget.cpp:964/1945`），并用全局 `attachment_sending_` 和 `client_bridge::upload_generation_`（`client_bridge.cpp:681`）保证一次只有一个附件。

## 设计

### 1. 服务端与协议：`client_id` 幂等

- SQL 028：`messages.client_id BIGINT NULL`，唯一索引 `(sender_id, client_id) WHERE client_id IS NOT NULL`。旧消息为 NULL，不回填。
- `send_message`、`begin_attachment` 增加必填 `client_id`（正 int64）。`begin` 把它存进当前连接的上传记录，`finish_attachment` 参数不变，用上传记录里的 `client_id` 插入。不保留不带 `client_id` 的旧形式。
- `send_message`、`begin_attachment` 的处理顺序：认证和参数校验后，**先**按 `(sender_id, client_id)` 查已有消息：
  - 命中且会话相同、用户仍是成员：返回该消息的**当前完整状态**（含编辑、删除、附件、回复、提及、表情），不检查发送权限和回复目标，不更新 activity，不推送。`begin_attachment` 命中时直接返回该消息，不建立上传。
  - 命中但会话不同，或未删除且类型不同（文字请求命中附件消息，或反之）：invalid params。命中已删除消息时直接返回墓碑：附件行已随删除清除，无法也无需判断原类型。
  - 命中但已不是成员：`-32006`。
  - 未命中：走现有流程（上传完整性、行锁、权限、回复、插入）。
- `finish_attachment` 不查重：同一 `client_id` 的消息只能由某个连接上「`begin` 未命中 → 上传 → `finish`」产生。每个用户只有一个连接，同一连接逐请求处理，新连接必须重新 `begin`，此时会命中。
- 唯一索引只是数据库层的保证。按上述约束不会并发插入同一 `client_id`；万一冲突，走现有插入失败路径（关闭数据库连接、返回 Server error），客户端按失败处理，下次重发会在第一步命中。不另加 `ON CONFLICT` 分支。
- 本人消息带 `client_id`：完整 message 增加可选字段 `client_id`，只在请求者是发送者本人时填写，包括 ACK、`get_messages` 和搜索。推送不带：同一份推送 JSON 会发给多人（`publish_conversation`），而发送者只在自聊时收到自己的推送，见第 4 节。附件 ACK 目前复用共享通知的 `notification.params`（`src/send_message_request.cpp:400`），加入 `client_id` 后 ACK 和通知要分开序列化。客户端收到任何带 `client_id` 的本人消息，就结束对应的待发项。作用与 Telegram 的 `updateMessageID` 相同，待发项与服务端消息的关联因此不依赖请求和响应的先后。
- 当前状态的查询复用 `get_messages` 的单行列表达式：把这段 SELECT 抽成两处共用的常量。
- 文字和附件的成功结果统一返回完整 message：`send_message` 改为返回与 `finish_attachment` 相同的 message，`send_message_result` 删除。客户端按现有版本合并规则并入，不会用待发正文覆盖服务端的编辑或删除。

### 2. SDK

- `send_message`、`send_attachment` 增加 `client_id` 参数，结果改为 `message`。SDK 提供 `new_client_id()`（`std::random_device` 种子的 63 位随机正数）。
- 附件结果校验放宽：重复命中时可能返回已删除、没有附件的消息，SDK 接受 `deleted` 的 message；未删除时仍校验文件名和大小。
- 附件在 SDK 内排队：一个连接同时只执行一个上传任务。断线时，正在执行和排队的任务都以 `transport` 失败。
- 不新增请求超时，也不在 SDK 内自动重试。重发由界面层的待发队列负责。

### 3. 待发队列（Qt 与 TUI 各一份，语义相同）

```
outgoing { client_id, conversation, text 或 {filename, bytes}, reply（完整 quoted_message）, state, error }
state: sending | failed
```

- **发送**：生成 `client_id`，创建待发项，立即清空草稿、回复和打字状态，在该会话末尾显示，调用 SDK。新发送仍要求在线且有发送权限（与现在相同），不再按会话阻止连续发送。
- **成功**：ACK 或历史里出现带同一 `client_id` 的本人消息时，删除待发项，消息按 id 并入历史。之后迟到的 ACK 找不到待发项，只按 id 合并。
- **`transport` 失败**：保持 `sending`，等待重连后自动重发。
- **`rpc`/`protocol` 失败**（无权限、回复目标已删除、文件不合法等）：标为 `failed` 并显示原因。Qt bridge 需把 `error_kind` 一起传给界面，目前只传文本（`client_bridge.cpp:674/690`）。
- **用户操作**：`sending` 只能复制；`failed` 可重发（同一 `client_id`）、删除（只删本地）、复制。回复目标已删除导致失败时，提供「不带回复重发」。
- **重连恢复顺序**：认证成功后的同一轮回调中，依次：
  1. 记下恢复游标（Qt `recovery_cursor()`、TUI 的增量历史起点）；
  2. 按创建顺序把所有 `sending` 项提交给 SDK，每个连接只调度一次；
  3. 发出会话快照、历史等现有恢复请求。

  正确性不依赖这个顺序：历史若先返回已提交的待发消息，会按 `client_id` 结束待发项（例如附件 A 在 SDK 队列里重新上传时，排在后面、已提交但 ACK 丢失的 B 先出现在历史里）。先重发只是让文字尽早结束。游标在第 1 步取得，不受 ACK 插入的消息影响。
- **会话不可访问**：收到权威的移除或会话消失事件时（TUI `app.cpp:761`，Qt `chat_widget.cpp:1513/2408`），提示「N 条未发送的消息已丢弃」，并清除该会话的待发项。分页快照里暂时没有该会话，不算权威移除。暂时没有发送权限（如禁言）时保留待发项，重发由服务端判定。
- **登出或切换账号**：丢弃所有待发项，旧连接的回调已由现有 generation、session 检查过滤。
- **退出**：不持久化。有待发项时，退出前确认，可以取消退出。
  - TUI：新增一个可取消的退出请求入口，Ctrl+C（`ui.cpp:1180`，现为 `shutdown()` 后无条件 `quit_()`）、`:quit` 和菜单「退出程序」（`app.cpp:660`）都走它。确认后才 `quit_()`，`shutdown()` 仍只负责清理。
  - Qt：托盘「退出」先确认，再改 `close_to_tray_` 并退出（`main_window.cpp:336-339`）。窗口关闭的确认放在关闭到托盘的分支之后（`:1118-1126`），关闭到托盘不提示。
- **待发项不参与**已读位置、分页游标、搜索、编辑、删除、表情回应、置顶。

### 4. 显示：独立列表

两端服务端消息都按 id 排序，多处把末尾 id 当作最新已读、`after` 游标或最新消息（TUI `messages.cpp:149/161`，Qt `message_model.cpp:421`、`client_bridge.cpp:645`）。待发项放在独立列表里，在历史末尾追加渲染，不混入服务端消息：

- TUI：消息选择范围延伸到末尾的待发项，菜单按项目类型分流。
- Qt：`message_model` 末尾追加待发行，新增状态 role；委托绘制「发送中」「发送失败」；右键菜单同上。
- 附件待发项显示文件名和大小；v1 不显示上传进度。
- 自聊：服务端先发响应，下一轮才发推送（`chat_session.cpp:358/203`），同一连接按序到达，所以 ACK 先结束待发项，推送再按 id 合并，不会重复显示。由测试覆盖。

### 5. 删除和保留的旧逻辑

- TUI：
  - `send()` 拆为普通发送和编辑。
  - 普通发送删除 `sending_` 门控、ACK 中的草稿和回复清除条件、附件 ACK 的回复清除。
  - 编辑保留按会话的提交中门控（原 `sending_` 只服务编辑）和现有的草稿保存与恢复。
  - `tests/conversation_test.cpp:3208/3223` 中「第三次普通发送被阻止」的断言改写为「连续发送都提交且各一条」。
- Qt：
  - 删除 `message_sending_`、`attachment_sending_`、`attachment_reply_`、`attachment_reply_text_`、`upload_generation_`，以及失败时恢复回复的逻辑。
  - 附件完成按 `client_id` 关联；连接 generation 只用于过滤旧连接的结果。
- 编辑消息不在本方案内。

## 分阶段实施与测试

1. **服务端 + SQL + SDK**：
   - 同一 `client_id` 重发只产生一行，返回同一 id，而且是当前状态：先编辑或删除，再重发，返回编辑后的消息或墓碑。
   - 跨会话复用被拒；文字和附件之间复用被拒；`begin` 命中时零上传；移除成员后重发得到 `-32006`。
   - `client_id` 只出现在本人消息上：ACK、历史、搜索带有；他人的历史和所有推送都不带。
   - 提交前断线：用会话行锁阻塞后断开，重发只产生一行。
   - 提交后 ACK 丢失：用中继拦截响应。`tests/native_network.py` 的 `Blackhole.pause()` 只设标志，转发线程可能已越过检查，不能作为屏障。测试工具需要一个同步暂停：等所有转发线程确认停止后才返回。流程是：持锁阻塞请求，同步暂停，释放锁，断言数据库已提交而客户端未收到成功，`sever()` 丢弃旧连接，恢复中继；重连后自动重发得到同一消息。
   - 附件 A 未提交、附件 B 已提交但 ACK 丢失，重连后 A 排在 B 前面重新上传：历史先返回 B 并结束 B 的待发项，A 随后成功，服务端各一条。
   - 两个附件连续提交，SDK 排队，均成功；上传中断时排队任务都收到 `transport` 失败。
   - 迁移测试。
2. **TUI**：集成测试覆盖：
   - 发送后输入框立即清空。
   - 阻塞期间重新回复同一消息，ACK 后新回复保留（R1）；阻塞期间按 Esc，ACK 后草稿为空（R2）。
   - 连续三次发送都提交，各一条。
   - 提交后 ACK 丢失，重连自动重发，服务端只有一条，待发项结束。
   - 连续两次断线。
   - 恢复历史与重发 ACK 的先后顺序。
   - 自聊不重复显示。
   - 回复目标被删除后失败，可以不带回复重发。
   - 成员移除后丢弃待发项并提示。
   - 登出后旧回调无效。
   - 编辑提交期间重复按 Enter 不重复提交；取消编辑能恢复草稿。
   - 有待发项时 Ctrl+C、`:quit` 和菜单退出都会提示，取消后程序继续运行；确认后退出。
3. **Qt**：明确断言：
   - 经 bridge 连续提交两个附件，两项都结束（覆盖 `upload_generation_` 的删除）。
   - 文字连续发送；失败后重发和删除。
   - 提交后 ACK 丢失，重连自动重发。
   - 成员移除后丢弃待发项。
   - 退出确认可取消，关闭到托盘不提示。

   使用 `qt_x11_smoke.py` 和现有 Qt 测试。
4. **文档与冒烟**：更新 `docs/verification.md`，跑全量 ctest、ASan/UBSan 和 TUI 冒烟。

## 评审结论（第 1 轮）

- 待发项使用独立列表，不用超大本地 ID 混入。
- 断线后自动重发，依赖 `client_id` 去重；待发项与服务端消息按本人消息上的 `client_id` 关联（第 2 轮）。
- v1 不持久化，退出前确认，可以取消。
- 文字不等待 ACK：连接内顺序保证文字顺序。附件在 `finish` 时才分配 id，可能排在之后发出的文字后面，可以接受。
