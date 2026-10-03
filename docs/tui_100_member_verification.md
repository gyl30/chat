# TUI 百人群与好友确认验证

2026-10-03 实测。好友确认实现、真实 Qt/TUI 功能矩阵、100 在线和 30 分钟稳态已完成；最终统一验证 normal/ASan/UBSan 均 20/20 PASS。本文区分实际 TUI 操作、SDK 批量操作和只读数据库事实，不把测试声明当作结果。

## 环境与代码

- BASE_HEAD：`9332b2d619b12a9d2e9f2ec94995ef71e1c448af`；开始时 fetch 后 HEAD 与 origin/main 一致，工作区与 submodules 干净。
- 最终统一验证代码：`32cd959529aba35b177b7c2d0aa5259ef9568cd1`，之后仅提交验证报告与状态文档。
- 最后生产修复：`fb3f209a0da5e313389925a1fe2193b81e241796`；完整功能矩阵使用 `20560342cde14f53473e5c0421b71d194d8d0f65`，新增大输入/头像拒绝验证为 `e2eb40e`。后两次只改测试工具，生产代码与稳态测试一致。
- Linux 6.8.0-136-generic；Intel i7-13700KF，24 逻辑 CPU；RAM 31902 MiB。
- GCC 16.0.1 20260315（r16-8100）、C++26；CMake 3.31.10。
- FTXUI v7.0.3 / `f921fad208912747c17d129a8ef75ec7624b6eec`，现有 submodule 未升级。
- tmux 3.6a、UTF-8、Python 3.10.12；真实 Qt 使用 Xvfb/xcb、XTest 键盘鼠标和截图，不用 OCR。
- PostgreSQL 17.11 位于独立数据库主机。进程资源数据不包含数据库主机；延迟是本次观测值，不是性能保证。

## 隔离、迁移与复现入口

每轮规模测试创建唯一数据库，继承标准 libpq `PGHOSTADDR/PGPORT/PGDATABASE/PGUSER/PGPASSWORD`；账号带唯一 run prefix，不依赖历史 ID。业务状态由真实 TUI、`chat::client` 和服务端 API 建立；数据库仅用于迁移、隔离库创建/清理、只读事实核对，以及既有回归中的锁同步。新增规模/真实 UI 矩阵没有直接写 SQL 伪造 contacts、membership、messages、read、roles 或 requests；这不表示整个旧 CTest 中不存在专门的 SQL fixture，旧迁移及上传竞争等回归仍保留其受控测试数据准备。

用户在本轮明确将旧单向关系改为确认好友。SQL 001–025 未改；新增 026。开发数据库实际 preflight 为 41 条 contacts：3 对双向关系、35 条单向，无 self/orphan。停服后二次扫描并迁移：保留 6 条 contacts，将 35 条单向记录转为原方向 pending。逐行核对方向、原创建时间、全部用户 ID/username 不变，未授予任何自动反向好友权限；重启后 health PASS。

统一回归仍使用隔离库。重测试不加入默认 CTest；复现完整终端流程：

```sh
# 先提供标准 PG* 环境；角色需能创建专用测试数据库。
tests/verify.sh
tests/tui_100_member_smoke.sh --work-dir /tmp/chat-tui100-new-run
python3 -B tests/qt_x11_smoke.py
```

规模入口默认 10 次重启、1800 秒稳态，构建 SDK fixture target，成功时清理自有数据库与进程；失败保留诊断和隔离库。需要 tmux、Python、PostgreSQL CLI；ASan 生命周期步骤使用 `build/asan/chat_tui`。Qt 另需 Xvfb、XTest、xclip、Pillow。凭据不在命令行传递，不写入仓库。

本次为避免重复等待，分别运行完整功能矩阵（10 次重启、30 秒短稳态，40/40 cases PASS）和同一生产代码的独立 1800 秒稳态。前者 JSON 如实为 `DIAGNOSTIC_PASS`，没有把 30 秒说成 30 分钟；后者单独 PASS。另有两个独立实际函数补证：61,440-byte TUI 输入和截断 PNG/JPEG 拒绝。

## Fixture 与功能矩阵

通过 API 建立真实 100 current members：1 owner、3 admins、96 ordinary members。至少同时运行 5 个真实 tmux `chat_tui`，其余为 SDK 客户端；owner 只有 20 个已确认好友，群成员身份不授予 direct/presence。包含各 5 个 incoming/outgoing、10 对好友双向 SDK 消息交互、60 条入群申请、65 个会话、150 条初始消息（120 条共同搜索关键词）。

下表 TUI 操作均由真实键盘与 capture-pane 验证；批量建号、读位及收件计数由 SDK 完成。离线与竞争边界同时由既有真实 server/client CTest 回归核对。

| 功能 | 结果 | 关键证据 |
|---|---|---|
| Account | PASS | 真注册、五账号登录、错误密码、Unicode/内部空格；首尾 Unicode whitespace 在注册 UI 拒绝 |
| Friend requests / Contacts | PASS | incoming 接受/拒绝、outgoing 取消、pending 不进 contacts；接受后双向出现；重复/反向/离线申请与恢复有 server/client 回归 |
| Direct | PASS | 两个真实 TUI 双向 ASCII/中文/emoji；删除后双方 send/reply/edit/reaction/typing/新附件禁用，保留 history/search/download/read/mute/pin/delete own；重新接受恢复 |
| Presence / Profile | PASS | pending 和共同群非好友不显示 presence/Message；接受后双向可见，删除后消失；头像公开资料不受影响 |
| Group member navigation | PASS | 100 名成员角色清楚，实际首/中/尾选择与滚动；群概要不铺开 100 人，完整列表单独查看 |
| Messages / History | PASS | 五 TUI 实际互收；150 条历史按页加载，编辑/删除/回复混合旧页保持单调、无重复遗漏 |
| Search | PASS | 120 条关键词跨页，Unicode query、编辑新正文、删除排除；返回会话与可复制详情 |
| Reply | PASS | 草稿中的目标被删后取消 reply 但保留草稿，实际发送后 reply=null；已有 quote 明确显示删除占位，不保留旧正文 |
| Edit / Delete | PASS | 自己文字编辑与删除实时到其他终端；附件不可编辑；移除好友后仍可删除自己的旧消息 |
| Reaction | PASS | 实际 add/replace/remove；100 在线时 99 用户分布 25/25/25/24，SDK snapshot 与 TUI 聚合一致 |
| Mention | PASS | Unicode、内部空格、点/横线/下划线、多成员提及；权威 mention IDs 来自 server，非成员被排除 |
| Typing | PASS | 真输入与 Esc、部分停止、自然过期、切页/断开清理；10 个 SDK typers 的每个收件方实际收齐 9/10 通知后清空 |
| Read / Unread | PASS | owner TUI 实际观察 0→1→10→50→99，SDK 权威读位核对；reader 离群降为 98；rejoin 前消息不计 unread，之后唯一新消息计 1，阅读回 0 |
| Attachment | PASS | 五类群附件与双方 direct 三类附件发送/下载 SHA256 一致；已有目标拒绝覆盖；移除好友仍可下载旧附件 |
| Avatar | PASS | TUI PNG→JPEG→clear；独立实测截断 PNG/JPEG 均拒绝且 revision/present 不变；超限拒绝 |
| Mute | PASS | 仍实时收件、unread/read 正常；真实重启后保持 muted |
| Personal pin | PASS | 65 个会话按服务器分页顺序逐项打开核对；置顶/取消正常，不做客户端自排序；重连恢复另核当前群、mute、读位与草稿 |
| Group roles / transfer | PASS | UI 拒绝第 4 admin；另实际 SDK RPC 得到服务端拒绝并保留 1/3/96；降级再升级、转让给 admin、旧 owner 退出/邀回/恢复角色 |
| Group create / manual invite | PASS | 仅已确认好友可搜索、多选、取消，之后输入名称确认；pending/shared group 不是候选；管理员手工邀请绕过 join approval |
| Announcement | PASS | set/replace/clear、多行全文、纯空白清除、普通成员只读；独立于消息、unread、搜索 |
| Group pin | PASS | header 摘要、替换、取消；edit 刷新摘要、delete 自动解除；普通成员仅查看 |
| Invite link | PASS | create/show/revoke/new；旧 token 失效，不自动加好友；离群后可再申请加入 |
| Join approval | PASS | 60 pending 实际两页，无重复漏项；接受/拒绝；revoke 后已有 pending 保留，旧 token 不再创建申请 |
| Removed group member | PASS | 正停留 history/search/members 的真实客户端被移除后清理旧页、compose/typing 与权限，再加入恢复正确 membership |
| Reconnect | PASS | 10 次真实 stop/start；五 TUI 身份/当前群/权威状态恢复，草稿保留；每轮四个接收 TUI 可见唯一消息、95 SDK 收到一次 |
| Resize / Help | PASS | 160×45、120×40、100×30、80×24、70×20、35×10；八个核心页面关键内容与过小提示，恢复页面和草稿正常 |
| Logout / shutdown | PASS | account 确认可取消；五账号退出；ASan 下 online/reconnecting/history/attachment/requests/typing 六态均 exit 0，无 UAF/deadlock |
| Terminal / file safety | PASS | ESC/OSC/BEL 文本不会执行为终端控制；Unicode、空格文件名和不覆盖保存；无 shell 拼接执行内容 |

Qt 另使用两个真实 xcb 进程完成 4/4 smoke：搜索申请→incoming→明确接受→双方 contacts→双向中文私聊；两步好友多选建群；百人概要仅 5 人预览、完整成员 100 人及非好友 profile；左下角头像打开 account，退出确认可取消，确认后回登录而进程继续存在。没有用 offscreen widget 调用冒充这轮真实进程操作。

## 100 在线、消息与文件数据

精确 fanout 阶段为 1 个真实 owner TUI + 99 个独立认证 SDK 客户端，权威当前成员与 presence 均为 100。唯一 owner 消息：received=99、duplicates=0、missing=[]、unexpected=[]；99 次 reaction RPC 之后再次核对仍无迟到重复。owner 实际看到 read=99 和 reaction `👍25 / ❤️25 / 😂25 / 🎉24`。

另一 SDK 成员发送 RPC（调用到 callback）为 **17.106 ms**；实际 TUI 发起发送动作到首次确认 99 方收齐为 **410.944 ms 上界**，包含清输入、键入、轮询，不能当作纯网络 fanout 延迟。未用混合 message/read/reaction 的 event 时间戳冒充消息延迟，也未设人为性能门槛。

独立真实 TUI 输入 61,440 UTF-8 bytes 单行消息，接收端头尾与 copy 页可见，SDK 确认完整正文/SHA256/唯一一条。大粘贴到输入尾就绪约 8.147s，到另一 TUI 可见约 8.386s；这包含输入和渲染，不是网络延迟。另将同尺寸、含大量换行和 ESC/BEL 的 SDK 原始消息交给真实 TUI 验证安全显示与导航。

群附件逐一由真实 TUI 上传、另一 TUI 下载，源/目标 SHA256 一致：

| 类型 | bytes | SHA256 | 结果 |
|---|---:|---|---|
| png | 69 | `64abf93fb4c16b4258aa6eff5660a6b97b013a1b9c0cc877fcceb14c6764680f` | PASS |
| jpeg | 633 | `49b8df61cc24dad7dd5a04affc87ff79e7afa80348779fb84d879174be0d90a2` | PASS |
| text | 38 | `0756a0ccc641607c39834e098a53df14bc5c231f880092139bcf1abca260c498` | PASS |
| binary | 524288 | `33bc8aab40703678c3ebe94d2dd8f2afff285dd901f9234e841e4679f8204fd5` | PASS |
| boundary | 10485759 | `02b6de0416e7852b41c07034a382f76a97e803e6890be9592253fbe3d89bf9b2` | PASS |

B→C、C→B 各自再发送/保存 text、PNG、JPEG，六次 SHA256 均一致；具体正文、sender、reply、文件名和捕获路径留在 evidence JSON。

原默认测试中依靠 SQL 伪造 3/10/50/200 成员的测量，已迁移为显式 SDK 工具。实际各 3 次 members/history/send，断言成员数与 read positions 数等于规模、发送成功；主百人群未改变。均值仅为观测数据（ms）：

| 人数 | members | history | send | 结果 |
|---:|---:|---:|---:|---|
| 3 | 2.253 | 3.286 | 16.028 | PASS |
| 10 | 1.969 | 3.023 | 15.802 | PASS |
| 50 | 43.041 | 2.995 | 15.964 | PASS |
| 200 | 44.905 | 75.487 | 72.187 | PASS |

## 30 分钟稳态

独立实际运行 **1800.399873s**，30 轮周期真实终端动作，5 轮通过 API 删除好友→重新申请接受→presence 恢复。四检查点始终 100 current members、100 online、95 authenticated SDK + 5 TUI。共 34 次 `/proc` 采样，全部 PID 存活且没有复用。

RSS 单位 KiB，按 A/B/C/D/E 顺序；CPU 是检查点前约一分钟每个 TUI 的区间值范围，0 秒没有前样本：

| 分钟 | 五 TUI RSS | TUI CPU 范围 | 每个 TUI threads / FDs | online / SDK |
|---:|---|---|---|---|
| 0 | 12656 / 12428 / 12460 / 12444 / 12468 | 无前样本 | 每个 4 / 7 | 100 / 95 |
| 10 | 12784 / 12472 / 12512 / 12476 / 12504 | 0.135–0.305% | 每个 4 / 7 | 100 / 95 |
| 20 | 12788 / 12528 / 12576 / 12496 / 12524 | 0.100–0.200% | 每个 4 / 7 | 100 / 95 |
| 30 | 12788 / 12528 / 12576 / 12560 / 12580 | 0.118–0.236% | 每个 4 / 7 | 100 / 95 |

每个 TUI 始终 4 threads / 7 FDs，RSS 首末增加 100–132 KiB。server 始终 25 / 141，RSS 22728→22932 KiB；SDK fixture 始终 191 / 383，RSS 19516→19536 KiB。没有观察到 busy loop 或明显线程/FD 泄漏，没有针对很小 allocator high-water 做无依据优化；30 分钟结果不证明多日负载完全无泄漏。

## 发现的问题与最小修复

| 真实复现 | 回归与修复 | commit |
|---|---|---|
| Qt 百人群成员页签被 join request 数量改名 | 新 overview 使旧索引错误；页签断言先 RED，改到实际 requests tab；qt_ui 与 X11 重测 PASS | `17bb73e` |
| Qt 新的朋友入口黑底暗字 | 单按钮渲染先 RED；加入既有浅色按钮选择器；qt_ui/X11 PASS | `dcd5405` |
| TUI 最新正文包含大量换行时侧栏被撑高，群名/下一会话不可见 | 80×24、160×45 四条断言先 RED；复用既有单行 preview；normal/ASan render 和实际 60KiB 场景 PASS | `dd0887d` |
| 长群名挤掉 unread/mute/pin | 两尺寸四条断言先 RED；标题行保留 unread、摘要行保留 pin/mute，仍两行；normal/ASan 与实际宽屏 PASS | `fb3f209` |
| 最终 ASan 的好友删除/附件完成竞争测试间歇泄漏回复对象 | 106-byte RED 可重复；只调整一处后又见 70-byte RED。两条回复改为命名 tuple 持有再引用解构，8 次原路径 ASan PASS，默认完整 server CTest PASS（51.53s）；保留原竞争断言 | `32cd959` |

好友确认及 account/group UX 属于用户明确变更的需求，提交 `4c1572c`；没有把它伪装为旧产品 bug。SDK 增加所需好友 API/DTO，未抽取通用框架或进行无关 Qt/TUI 重构。真实终端驱动开发期间的其余失败诊断属于自动化焦点、异步回调/旧 status 的等待判断，保留证据后修正驱动，再实际复跑；未用改产品语义、固定长 sleep、suppression 或放宽 CTest timeout 消除失败。

## 统一回归

| 阶段 | normal | ASan | UBSan |
|---|---|---|---|
| 原始基线 | 19/19，85.40s | 19/19，108.53s | 19/19，96.53s |
| 好友实现 | 20/20，92.94s | 20/20，124.31s | 20/20，115.75s |
| 全部真实验证之后 | 20/20，93.65s | 20/20，125.41s | 20/20，112.12s |

首次最终 `tests/verify.sh` 的 normal 20/20（88.72s）；ASan 的 server 测试检测到 raw WebSocket 测试读帧协程分配的 106-byte 泄漏，19/20 通过、124.15s，入口因此停止，UBSan 未运行。该失败没有忽略；针对相同 raw WebSocket 路径增加诊断参数后再次复现。修复限于测试返回对象的显式持有，未修改生产代码或第三方；没有独立证据断言是编译器根因。修复与重新完整验证结果在上表最终行记录。

最终实际执行原 `tests/verify.sh`，Qt/TUI 均 ON。新增 `friendship` 复用同一 `chat_server_test` fixture；migration 覆盖 025→026 及 fresh 001→026；旧单向测试改写为 pending/accepted/removed/reaccepted 状态机。真实数据库锁同步覆盖 accept/reject、accept/cancel、accept/open_direct、remove/send、remove/attachment finish。原 Qt models/delegate/UI 与 server/client 全部保留。重测试没有塞进默认 CTest。

ASan 真实退出另外用暂停自有 server 和精确 socket Recv-Q 增量证明 history/attachment/requests RPC 在途，再退出 TUI；10 MiB 附件上传停在 begin 响应前。六种状态全部 exit 0，无 sanitizer 或 Resource deadlock avoided 报告。

## 证据、提交与限制

开发机证据目录（不包含在仓库提交中）：

- 完整功能矩阵：`/tmp/chat-tui100-matrix-1003112717`，40/40；`cases.jsonl`、captures、SDK/只读 SQL、附件 hashes、summary。
- 30 分钟：`/tmp/chat-tui-soak-2sojsdcq`，`soak-metrics-summary.json`、四检查点、每分钟资源、二进制 SHA。
- 大输入与两个格式拒绝：`/tmp/chat-tui-jpg_194446_3445421`，直接执行提交中的实际函数。
- Qt X11：`/tmp/chat-qt-x11-20261003-184704`，19 张截图及 API/SQL 事实。
- 独立复核 99 fanout / 10 restarts：`/tmp/chat-tui-tail_192404_3365542`。
- ASan 首次最终失败：`/tmp/chat-tui100-final-verify-attempt1.log`；重复 RED：`/tmp/chat-asan106-raw-red-3.log`、`/tmp/chat-asan70-trace-2.log`；修复后八次：`/tmp/chat-asan-pair-{1..8}.log`，默认完整 server：`/tmp/chat-asan-pair-server.log`。
- 最终统一验证：`/tmp/chat-tui100-final-verify.log`；开发库迁移审计：`/tmp/chat-friends-dev-migration-audit.json`。

逻辑提交：`4c1572c` 好友模型与 UX；`17bb73e`、`dcd5405` Qt 修复；`143db99` SDK 规模 fixture；`dd0887d` TUI 单行摘要；`df3d487` 真实 Qt 工具；`fb3f209` TUI 状态空间；`2056034` 真实百人矩阵/在线/稳态工具；`e2eb40e` 大输入与头像拒绝补证；`32cd959` 测试异步回复持有。报告在随后独立提交收口。

限制：TUI 单行输入、Enter 发送，没有新增多行编辑器；多行原始消息/公告由 SDK 创建后经真实 TUI 全文查看、编辑框原值确认和清除。图片仅文本附件，下载保存不自动打开，无 inline graphics/桌面通知。复制采用可选中文本页面。真实终端自动化是 tmux 键盘/capture-pane，不等于人工体验评审。延迟测量期间可能有其他隔离探针并行，非 benchmark。成功轮隔离数据库和自有 tmux/server/helper 全部清理；失败诊断库按工具约定保留，不涉及真实用户数据。

交付前执行 `git diff --check`，并在本报告提交推送后重新 fetch，确认 HEAD 与 origin/main 一致、工作区及全部 submodules 干净。最终报告提交仅更新文档，不改变上述已验证代码。
