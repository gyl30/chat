# 稳态审计记录

2026-10-01，从实际 `origin/main = 27b2a885c0eef66950410fcb0bc737dc7bcb4502` 开始，审计 `16fd4be..27b2a88`。开始时重新 fetch，工作树 clean，HEAD 与远端一致。范围包含 SQL 008–014、服务端会话和消息路径、client library、Qt 状态与现有测试；本阶段不增加产品功能或数据库字段。

## 发现与修复

需求核对：五处测试连接配置写死 PostgreSQL 主机、数据库或用户。现改用空 libpq conninfo，Qt 子进程继承环境，统一读取 `PGHOSTADDR/PGPORT/PGDATABASE/PGUSER/PGPASSWORD`。错误密码及不存在数据库的负例只覆盖被测字段，不覆盖其他环境配置；连接成功与否由 libpq 决定。

生命周期核对：`client_bridge::connect_to_server` 原来只推进会话列表和历史的 generation，旧搜索、上传、下载结果已经排队时可能进入新连接的界面。新增回归先复现失败，再让新连接同时推进已有五个计数；没有增加计数或新的状态框架。

构建核对：GCC 16 的静态反射在 ASan/Debug 下为群管理四个同名局部解析类型生成了重复汇编符号。改为四个独立类型名后编译通过，解析规则和协议未变化。

仓库原则核对：保留有实际职责的状态，不机械拆文件。没有新增兼容路径、配置框架、一次性包装或未来功能预留字段。

## 状态与职责

| 状态 | 当前用途与审计结论 |
|---|---|
| bridge 五个 generation | 分别丢弃过期的会话分页、历史分页、搜索、上传、下载回调；新连接和 close 均使旧请求失效 |
| `messages_loading/messages_loaded/history_exhausted` | 正在请求、已有历史基线、向前翻页到尽头；首次加载和恢复不能只用一个标志表达 |
| typing 本地空闲、刷新、远端过期及清理计时 | 分别结束本地输入、限制刷新频率、让远端提示失效、清理显示；切换和断线清理，重连不恢复旧输入 |
| attachment upload/download job | 一个请求链保存内容、偏移和完成回调；传输失败结束 pending 请求，服务端上传随 session 销毁或退出当前群清理 |
| `attachment_sending` | 禁用重复文件发送，完成或连接/会话清理时恢复控件 |
| reconnect 状态及计时器 | 区分重试间隔、倒计时、延迟提示及恢复提示；历史从已加载范围起点重新获取，包含编辑和删除 |
| group `pending/available/contacts_ready` | 操作待响应、成员快照有效、联系人可用于邀请；断线禁用操作，恢复后重新打开获取快照 |

没有发现可以安全删除的 shadow state。client 与 server 的单连接请求处理保持顺序，未依据假设增加 edit/delete generation 或通用版本框架。

## 事务、读位与附件

发送和邀请都在事务内锁住同一个 conversation；锁取得后再检查当前成员/角色和最新消息水位。任免管理员、改名、邀请、退出的权限也在锁后查询。群管理事务正常 COMMIT/ROLLBACK，SQL 错误关闭连接再归还 lease；pool 下一次 acquire 重连。新增测试验证事务失败后的重连和显式 rollback 后连接复用，没有给 pool 增加自动事务层。

`joined_message_id` 只用于未读基线，`last_read_message_id` 只由真实 `mark_read` 推进。群双勾取当前其他成员中至少一人的真实读位；成员快照会移除退出者的读位。已有 model 测试覆盖仅自己、部分其他成员、成员变更和单聊；没有永久 per-message 回执表。

附件仍为单文件 10 MiB、32 KiB chunk、数据库持久化。新增真实 WebSocket 测试在 begin/chunk 后断开连接，重连不能 finish 旧上传且可以重新上传；退出群后旧上传失效，另一个会话可以开始新上传。现有测试继续覆盖大小、分块偏移、取消、删除、下载权限以及 Qt 下载断线。权限变更能力由下一产品阶段实现，本阶段不提前增加移除成员路径。

## 小群规模

`get_members` 聚合当前成员，历史返回所有当前成员的读位，`publish_conversation` 查询和遍历所有当前其他成员：这些路径随成员数 M 增长。历史消息仍按会话/消息索引取 50 条；会话列表按成员索引及活动 cursor 分页。没有新增人数上限或大群基础设施。

真实 client/server/数据库测试分别建立 3、10、50、200 人群，核对完整成员数和读位数，每种规模各测量三个 get_members/history/send 样本。只有发送者在线，其他成员离线；因此包含发布的成员查询和遍历，不代表 200 个在线连接的 fanout 吞吐。测量打印在 server 测试的 `GROUP_SCALE` 输出中，含网络和数据库延迟，不作性能阈值断言。

该测试证明当前模型在已测 3–200 人范围内形成完整闭环；200 不是协议上限或容量承诺。实际扩大在线群规模前，应测在线 fanout、消息频率和慢消费者；当前没有证据需要对象存储、Redis、事件总线或永久消息回执表。

## 验证范围

完整 CTest 的 13 个测试包含 migration、client、PostgreSQL、HTTP/WebSocket、server、Qt model/delegate 和真实多窗口 UI。已有测试覆盖 concurrent send、leave/rejoin、typing/disconnect、edit/delete/reconnect；本次增强真实 send/invite 竞争、事务连接恢复、附件断线/退出及过期 bridge 回调，复用现有公共边界。

最终验证结果记录在 [开发状态](development-status.md)。构建目录均位于被忽略的 `build/` 内，测试环境通过调用进程提供；没有修改或补写 SQL 001–014。


## 运行期可靠性复核（2026-10-10）

本轮基线 `e2be12d86428f211e769447f785558d290d5f475`。只把可复现的问题作为修复依据；
`project-analysis.html` 是用户提供的调查线索，未修改、提交或把其中的描述直接当成事实。
没有新增 SQL/RPC、改变好友或群权限，也没有拆分大文件或替换现有 RPC 分发。

### 失活连接与单账号在线

原实现会自动响应对端 ping，但不主动探测失活。实际双向字节黑洞保持两端 TCP socket
打开，45.70 秒后原 session 仍占用账号，重新认证返回 `-32004 User already online`。
正常关闭、进程 SIGKILL 和 TCP RST 对照均可重新登录（观察上界 0.34–0.39 秒，含认证耗时）；
进程退出不是网络黑洞。这是用户态 relay 消费并丢弃字节的确定性故障，不是物理 Wi-Fi
断开或内核丢包实测。

客户端和服务端现在在现有单连接协程中使用同一探测规则：完成 WebSocket Upgrade 后，
30 秒发送一个带序号的 ping，10 秒内必须收到相同 payload 的 pong。无关业务消息、旧 pong
和错误 pong 不延长 deadline；匹配后重新计时。每次 flush 使用一个绝对写入截止时间，
不能用不断产生的分块延长写入。WS/WSS 走同一规则，不增加线程或应用层 RPC。
失活退出现有 session 清理路径，移除 `online_users` 登记；存活的旧 session 仍使新登录
返回原错误，没有自动踢人。

真实默认时长黑洞 GREEN：WS 39.66 秒、WSS 39.56 秒释放并重新登录成功。
正式 `server_tls` 内复用短时配置，分别断言 WS/WSS 确实丢弃字节、服务端先关闭、
重复在线认证被拒绝、失活后同账号恢复及正常 RPC。配置仅在内部构造边界注入；公共 SDK
和服务器命令行没有额外的测试开关。

探测实现审查又建立两项 RED：大量连续发送时不读取已到达的 pong，会误判失活；
发送路径等待 pong 时收到完整 RPC ACK，随后 timeout 会把 ACK 一同清掉。
现在只在等待 pong 时泵入已有接收队列，保持业务消息顺序；连接错误清理前派发已经收到
的完整消息，再失败尚无响应的请求。关闭/析构的 callback 抑制条件保留。
真实公开 SDK 私有探针证明首 ACK 从错误失败变成成功，其余未收到响应的请求仍失败。
该公开路径探针采用私有短时配置，normal/ASan 实际通过，不能写成默认周期公开 CTest 覆盖。永久 client 测试复用
原 executable，覆盖假时钟、正确/错误/迟到 pong、无关流量、部分帧与取消、批量发送公平性
及超时前收到 ACK 的队列保留。私有 WS/WSS 探针 normal/ASan 各 9/9 通过。

边界：本轮 deadline 从 Upgrade 后开始，DNS/TCP/TLS/HTTP Upgrade 阶段的连接等待规则
未改变。单连接业务处理和事件循环被长时间阻塞时也可能推迟检查；约 40 秒是本次可运行
事件循环下的实测，不能作为所有负载下的全局释放 SLA。物理网络接口断开未执行。

独立复核还确认一个既存生命周期缺陷：多个请求同时失败时，首回调析构客户端后，
`fail_pending` 仍调用后续 handler。实际 RED 观察到 3 次，预期只有首个；最小修复把已有
`suppress_callbacks_` 检查移到每个 handler 前，没有新状态。永久回归复用原 fixture，
用成功 RPC 作安装 barrier、用生命周期释放 future 确认 shutdown；不以 sleep 猜顺序。
私有 normal/ASan 完整 client 测试 GREEN。

### 请求大小与持久化

服务端原有接收 64 KiB 限制和消息通知序列化预算保留。实测临界 ASCII、CJK 和反斜线
转义：某些超过完整请求预算的正文原来引发断线，而非明确错误；拒绝请求没有数据库写入。
SDK 现在先序列化完整 JSON-RPC 请求，超过既有 64 KiB 后直接返回“内容过长，请缩短后重试”，
不加入 pending/send queue，不关闭连接。共享常量表达同一个请求帧限制；没有另加正文字符数
限制，也不降低客户端接收通知预算。

永久测试包含完整 JSON 恰好 65536 字节、ASCII/CJK/转义超限、错误翻译和后续请求。
真实服务器 11 个边界组合通过：成功请求增加一条消息，拒绝请求增加零条，全部连接保留，
后续 RPC 和正常发送成功。已有服务端通知预算在提交前检查，未复现“已持久化但被判断失败”
的边界缺陷。本次不改服务端消息事务。

### 真实界面与最小尺寸

TUI 固定 52 列命令面板在 40×12/45×15 裁切键位和候选，是确定 RED。
面板现在随宽高缩减，并让选中项滚入可见窗口；中文操作名优先，空间足够才显示高级命令别名。
40×12 下另一 RED 是两行历史被固定 sender 标题遮住整个正文；不足三行时保留正文和发送状态，
常规高度继续显示固定标题。追加回复/编辑/公告真实组合测试又发现，历史仅一行时
默认焦点仍落在尾部已读状态，正文不可见；先建立永久 RED，再把这一行的默认焦点放在
正文/附件最后行。没有删除元数据或改变消息行索引，显式滚动仍可查看原已读行。
真实 WS/WSS 同一 driver 确认正文、模式与 Unicode 草稿同时可见。最低可用尺寸不变。

真实 tmux 在 40×12、45×15、50×20、60×20、80×24、100×30、120×40、160×40，
验证 Ctrl+K 打开/搜索/选择/执行/取消、F1、输入焦点、消息菜单/复制、窗口缩放、草稿和
Unicode/多行 bracketed paste，共保留 207 份 plain/styled capture；最终最小历史再保留 13 份。
输入框中的 `?:/ared` 继续作为正文。CLI 帮助修正为真实的 F1/Ctrl+K 键位。
另外分别运行两真实 TUI 的 WS/WSS 组合：45 秒正常 idle、自己的服务重启自动恢复原历史
和多行 Unicode 草稿，Ctrl+Z 进入真实交互 shell、检查 canonical/echo、fg 恢复，以及退出后
归还终端；各保存 28 份 capture，自己的进程、tmux 和数据库清理。最终一行历史 UI 用同一
driver 另测短时组合，各 28 份，不把短观察宣称重复 45 秒验证。真实 TUI 大文本超限发送、
长期暂停、物理断网、完整明暗主题矩阵未执行；超限的 SDK 与真实 Qt 证据不能替代这些。

真实双 Qt 使用验证 SAN 的私有 CA WSS，9 项通过：登录、重复在线拒绝、非好友 direct 隐藏、
超限中文提示和完整 65536 字节草稿保留、45 秒 idle 后收发、反向消息、服务重启后的自动恢复、
重连后非好友仍隐藏及正常退出。前两次测试驱动失败保留原记录，没有计入产品 PASS。
本次没有完整重做好友确认/移除/重新接受的原生矩阵；完整 CTest 是另一层回归证据。

README 补齐真实构建、迁移、启动和 WS/WSS 前提；第三方索引指向实际原始声明，未替 Chat
选择许可证。群规模、BYTEA、分发 if/else、单文件行数及缺少 CI 本轮没有可复现的新缺陷，
没有据此增加架构、缓存或存储层。

原始日志、二进制哈希、PNG、ANSI/plain capture 和失败记录保留在开发机：
`/tmp/chat-runtime-audit-20261010`、`/tmp/chat-sdk-heartbeat-review`、
`/tmp/chat-message-boundary-e2be12d`、`/tmp/chat-qt-heartbeat-native-e2be12d-attempt3`、
`/tmp/chat-tui-palette-e2be12d`、`/tmp/chat-tui-network-final`、
`/tmp/chat-tui-one-row-final`、`/tmp/chat-fail-pending-lifetime-ddffualn`。它们是本轮开发证据，不是仓库自带的可永久重放测试资源。
正式回归位于现有 client、server_tls、tui_render 入口；没有新 executable 或默认重型测试。
本次范围不等于完整 Qt/TUI 品质矩阵或 92 分验收，完整主题、HiDPI、IME、辅助技术等仍未证明。


### 最终完整门禁

最后一次源码冻结后实际运行原 `tests/verify.sh`，exit 0：normal 28/28（144.67 秒）、
ASan 28/28（203.55 秒）、UBSan 28/28（178.22 秒）；Qt/TUI ON，没有 suppression、
跳过测试、降低断言或放宽 timeout。编译警告和 sanitizer 诊断均未见。
`client/websocket/server_tls/tui_render/tui_state/tui_integration` 最终针对性回归 6/6，27.63 秒。
最终 ASan `tui_render` 4.78 秒，接近原 5 秒限制；记录这一余量，不外推任意系统负载下稳定。
首轮完整门禁发生在最后两个组合修复之前，也保留日志，不能替代上述最终结果。
生产和测试文件的冻结哈希已与最终运行后字节核对。

最后完整门禁：开发机 `/tmp/chat-runtime-audit-20261010/verify-final.log`；
先前门禁为同目录 `verify.log`。正式回归可以用仓库验证入口在符合前提的专用数据库重跑。
所有本轮拥有的业务测试数据库、服务端、客户端和 tmux session 清理；用户分析报告原样保留。
