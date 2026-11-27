# Linux 隔离验收

在专用测试环境运行。使用项目要求的 GCC 16、C++26 Reflection、CMake、Qt 和[系统开发包](../../README.md#构建)，另需 Ninja、Python 3.10+、websockets 15、boto3、dbus-python、PyGObject、dbus-run-session、Silo 与 mc。把 Boost、Qt 等必要前缀加入 CMake 搜索路径，不使用 SQLite 或 libcurl 安装前缀。若 Silo 不在 PATH 中，用 SILO_BIN 指定可执行文件。

```sh
ops/build-sqlite-runtime.sh "$SQLITE_PREFIX"
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" ops/acceptance/linux.sh /tmp/chat-release
```

脚本构建三个生产目标，在临时目录链接 Qt 驱动。驱动操作实际主窗口、菜单和模型，使用生产 SDK、资源和主题。验收创建临时账号、SQLite、Silo 卷、私有会话总线及 Qt 应用目录；测试客户端连接本机随机端口。多选与转发、分组拖放和收藏分别使用隔离的 Xvfb 和 D-Bus，无需登录桌面。安装 xauth 和 libXtst 以运行虚拟桌面交互验收。失败返回非零。请使用普通的 Python 运行模式，以执行断言。

下面列出的功能脚本可以单独运行。标准回归外的 SDK 协议验收、S3 传输及 4 GiB 边界、WAL 与性能测量、平台实测继续使用各自入口。

| 入口 | 观察的结果 |
|---|---|
| `message_content_probe.cpp` | 明暗主题下气泡换行命中、菜单复制、隔离浏览器请求、Markdown 资源限制与相邻代码块复制 |
| `message_content.py <probe>` | 真 Qt 草稿、进程终止后 Outbox 恢复、格式与全员标记持久化、降级拒绝和原标识重试 |
| `mention_all.py` | 发言和角色授权、幂等、撤回、清空与再入群；`desktop.py` 同时检验三档通知的九种组合 |
| `bundles.py <probe>` | 合并快照、分页、Qt 下载及最后文件引用回收、两个归档月份和独立 Silo 恢复 |
| `polls.py <probe>` | Qt 创建、改票、取消、实时成员明细、断线期间错过的改票、结束竞态和归档恢复 |
| `content_races.py <probe>` | Gate 扣留真实历史、详情、投票人及合并记录分页回包，核对 Qt 最终显示、水位和请求归属 |
| `conversation_folders.py <probe>` | 跨账号隔离、多端顺序与移动、隐藏后恢复、退群重入、解散、折叠搜索和组内置顶 |
| `folder_interaction.py <probe>` | 脚本单独启动 Xvfb 和私有 D-Bus；通过 libXtst 实际拖放，检查右键归组、三个标读菜单、草稿和重启后的折叠状态。需要 xvfb-run、xauth 和 libXtst |
| `bulk_read.py` | 检查真实 SQLite 水位、手工标记、并发新消息、跨设备事件、其他成员回执和客户端第一页以外的会话 |
| `user_profiles.py <probe>` | 昵称与签名边界、隐私、重复昵称、实际 Qt 编辑，消息、回复、转发的作者实时变化及结构化提及 |
| `avatars.py <probe>` | 真实头像 RPC 的格式、CRC／Adler32、解压边界及失败不覆盖；实际 Qt 选择、裁切、旧头像读取、多端刷新及 PNG／JPEG 附件回归 |
| `group_profiles.py <probe>` | 实际头像选择与上传、用户和群同 ID 隔离、上传期间管理员降级、群简介、公告与成员过滤 |
| `clear.py` | 重试相同 H 后，后续未读消息与手工标记保留，实际已读位置保持，已提交任务得到无正文确认 |
| `login_sessions.py [Qt 驱动]` | 真实登录凭证及安装绑定、同会话多连接、并发改密、服务端重启和已撤销 token 拒绝；真实 Silo 完成回包被扣住时撤销，拒绝后续数据库发布；Qt 设备列表与确认、无密码重连、撤销通知／确认延迟、草稿保存窗口、Outbox 对账和原安装续传 |
| `group_moderation.py [Qt 驱动]` | 禁言角色、水位、时钟、幂等确认，真实 Silo 完成与 Chat 发布之间的竞态；解散的原成员通知、归档隔离、重启回收和共享对象；传入驱动时验证 Qt 多端、隐藏会话手动重试、禁言拒绝、失权任务不可恢复、原设备续传，以及重新入群前后的搜索迟到回包 |
| `desktop.py` | 真实 RPC 代理延迟或丢弃指定回包，验证 Qt 草稿、Outbox 双会话队首和去重、通知、三页搜索游标与迟到上下文；验证群文件搜索防抖、50/51 页边界、Unicode 文件名与失权；覆盖图片和保存子窗口、群资料置顶、多个上传目标、隐藏会话上传恢复和真实 SDK 下载 |
| `recovery.py` | 停止隔离 Chat 与 Silo，分别备份数据库、月档及整个对象卷；启动独立恢复实例，用原 Qt 安装的任务日志从第 2 片继续，再检查消息权限与对象引用 |
| `wal.py` | 验证静态服务端与私有 Python 的 SQLite 身份、WAL 并发 checkpoint 和搜索分页 |
| `history.py` | 真实 Qt 覆盖 3,100 条较新消息后的分类和 around；`--baseline` 可用旧服务端、旧版 Qt 测试驱动复现自动补页上限 |
| `reading_navigation.py` | 真实 RPC 与 Qt：同快照未读锚点、跨月只读定位、滚动和重连补页、提及组合/分页、编辑及失权、迟到回包 |
| `bookmarks.py [--probe 路径] [--previous-server 旧版服务端]` | 个人收藏、隐私和失效、分页顺序、附件引用、Qt 多端与保留草稿定位；可选验证旧库升级 |
| `search_filters.py` | 组合条件、无关键词、隐藏私聊、权限、50/51 页边界及字面空格 |
| `message_selection.py <probe>` | Qt xcb 隔离虚拟桌面：Ctrl/Shift 多选、撤回、历史补页、待发/失败行、断线、剪贴板顺序与草稿、会话切换、清空及批量转发 |
| `live_history.py` | 双设备实时编辑与撤回、显示/剪贴板、筛选移除、乱序回包和保留服务端游标 |
| `drafts.py` | 真 Qt 重启与会话/账号/端点隔离；探针扣住本地入队完成回调直至输入新版本，另用隔离数据库故障确认保留文字和回复 |
| `deep_reply.py` | 最近一页之外的草稿引用、重启恢复、实际发送、编辑与迟到校验、撤回/清空/失权 |
| `downloads.py` | 两个会话及群文件的同名任务、取消、真实流截断、重新鉴权和撤回拒绝；`--stalls` 保持两个 TCP 流不再发送，检查客户端 EOF、临时文件、共享工作器、落盘/取消竞争及断线清理 |
| `history_filter_perf.py` | 对照两个 Release 服务端的稀疏历史、全局搜索及其他连接的发送确认；输出原始端到端样本 |
| `search_bench.py` | 在独立磁盘 WAL 库测量真实搜索与发送 RPC，输出每组延迟、CPU、内存、归档读取次数和被动 checkpoint |

默认项目构建只生成产物。使用 `linux.sh` 主动运行标准回归，或用各脚本复查特定场景。

## SDK JSON-RPC 和 Unicode 定向验收

在已有真实隔离服务端、SQLite 和 Release Qt 构建的环境中运行：

```sh
work=$(mktemp -d)
export CHAT_TEST_SERVER="$BUILD/server"
python3 ops/acceptance/build_probe.py "$BUILD" "$work/sdk-qa" ops/acceptance/sdk_json_probe.cpp
python3 ops/acceptance/sdk_json.py "$work/sdk-qa"
```

探针使用实际 SDK 和 Qt 文本分割函数。回环代理核对 65,535／65,536／65,537 字节的区别，
并扣住一条响应，验证解析后的通知、重入请求、乱序响应和断线清理。服务端广播的 64 KiB
限制仍独立于请求上限；请求从 SDK 发出后，可能正常收到业务拒绝。
`drafts.py` 另验证超限文本本地拒绝后，输入、草稿和回复目标仍保留，没有发送 RPC。

[2026-11-24 JSON 和 Unicode 依赖替换验收](results/simdjson-unicode-20261124.md)。

## 消息菜单、联系人与群文件定向回归

在独立虚拟桌面或离屏环境复用同一 Release 探针及本节的隔离服务端、SQLite、Silo：

- `message_selection.py <probe>`：在真实 xcb 菜单打开期间由另一设备编辑或撤回，检查复制的是当前正文，已撤回项不覆盖剪贴板。转发目标窗口保持打开时，分别撤回首条、中间、末条和全部选中项，并覆盖单条转发、个人清空、失权、账号切换和取消。探针核对真实 RPC 顺序、返回及目标会话 SQLite 中的 `forward_source`，无发送场景使用同连接请求作屏障。
- `bookmarks.py --probe <probe>`：在已加载两页并选中末行时，未收藏消息的编辑不重置列表；未加载的收藏编辑后仍可新命中。
- `desktop.py <probe> contacts`：好友申请、拒绝、接受、移除、备注与账户名搜索、资料和分组在第二设备变化时同步。单独使用新账号夹具，不与默认文件任务场景共享。
- `desktop.py <probe> file-search` 或 `desktop.py <probe> outbox`：独立重跑群文件搜索或待发调度，场景也纳入 `linux.sh`。
- `downloads.py <probe>`：保存窗口打开后，用原有群文件操作移除所选对象并等待列表刷新，确认保存不再产生无效任务或下载授权请求。另用 `--stalls` 验证 TCP 取消和共享工作器。

## S3 与附件定向回归

从 `CMAKE_EXPORT_COMPILE_COMMANDS=ON` 的 Release/Ninja 目录构建两个探针。服务端、Qt
驱动和探针使用同一批产物。运行 Python 场景前设置 `CHAT_TEST_SERVER`、`SILO_BIN`，并沿用
私有 SQLite 和独立 Qt 应用数据。无需操作登录桌面。

```sh
work=$(mktemp -d)
export CHAT_TEST_SERVER="$BUILD/server"
python3 ops/acceptance/build_probe.py "$BUILD" "$work/qt-probe"
python3 ops/acceptance/build_s3_probe.py "$BUILD" "$work/s3-probe"
"$work/s3-probe"
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/s3_transport.py "$work/qt-probe"
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/large_attachment.py "$work/qt-probe"
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/downloads.py "$work/qt-probe" --stalls
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/recovery.py "$work/qt-probe" \
    --source-server "$OLD_BUILD/chat_server" --source-probe "$OLD_PROBE"
```

- `s3-probe` 验证静态 curl 的版本、TLS/DNS/IPv6、实际协议、AWS 固定时间签名样例、查询编码及 Yxml。
- `s3_transport.py` 转发到环回 Silo，覆盖实际请求、非默认 region、自定义桶和端口、Content-MD5、
  授权时长、Initiate／Complete 回包丢失、真实 PUT 无确认、HTTP 200 内嵌错误、清理、
  临时 CA 信任、未知 CA 与主机名拒绝、连接拒绝、TLS PUT 停顿后暂停并续传，以及 TLS GET 停顿时退出。
- `large_attachment.py` 用真实 Qt/SDK 上传并下载 4 GiB，核对 256 片确认、Silo 元数据、完整
  SHA-256 和最终文件。临时目录须有至少 12 GiB 可用空间。
- `recovery.py` 的两项 `--source-*` 选项必须配对，用于在旧版暂停、新版恢复。Qt 数据目录、
  恢复的 Silo 分片和上传 ID 不变，首个确认仍应是第 2 片。去掉两项可做同版本恢复。

[2026-11-24 依赖替换验收与构建对照](results/s3-curl-20261124.md)。

## 阅读导航的原生桌面验收

默认仍为离屏自动化。原生验收须从已登录的图形会话运行，沿用同一 Release、私有 SQLite 和隔离 Silo：

```sh
work=$(mktemp -d)
python3 ops/acceptance/build_probe.py "$BUILD" "$work/probe"
export CHAT_TEST_SERVER="$BUILD/server"
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX"   python3 ops/acceptance/reading_navigation.py "$work/probe" --native
# 单独启动一个隔离的深色、进程级缩放及 Ctrl+Enter 场景
QT_SCALE_FACTOR=1.5 ops/with-sqlite-runtime.sh "$SQLITE_PREFIX"   python3 ops/acceptance/reading_navigation.py "$work/probe" --native --theme dark --ctrl-enter
```

出现 `ready` 后，用桌面鼠标和键盘打开长名称测试群、切换前后台／最小化、打开并关闭菜单与只读上下文，再使用滚轮、滚动条、PageUp/PageDown、Tab/Shift+Tab 与空格。调至最小窗口，检查两按钮同时出现时的布局。输入框内 Enter 按隔离配置发送或换行。不要从探针激活窗口或直接设滚动条。

标准输入每行一个 JSON：`{"op":"observe"}` 仅观察 Qt 平台、DPR、窗口与控件几何、焦点、滚动、网络请求和数据库水位；`{"op":"peer","text":"测试消息"}` 由独立测试账号发消息；`{"op":"network","hold":true,"disconnect":true}` 断开实际连接并暂扣历史回包，`{"op":"network","release":true}` 放行；`{"op":"quit"}` 停止。操作窗口的同时可由另一终端或脚本读写该进程的标准输入与输出。只读上下文的未读断言在关闭前执行；关闭后如主窗口已经处于连续最新区间底部，则应正常标读。

2026-11-23 原生回归：Ubuntu 22.04 的登录会话为 GNOME/Xorg，Qt 6.5.3 `xcb`，2560×1440 显示输出；在默认 DPR 1 和独立深色进程 DPR 1.5 下检查默认 1180×760 与最小 980×640 逻辑尺寸。使用 X11 XTEST 向实际桌面发送输入，另用独立 X11 窗口验证切换。基线复现了关闭主菜单不恢复标读、PageDown 未可靠移动及导航按钮和长标题难以辨识；修复后对应回归通过。系统显示设置未改。

输入框标准右键菜单单独检查：在连续最新区间底部输入测试草稿并保留回复目标，右键打开编辑菜单，用 `peer` 发消息。菜单打开期间不得发出 `mark_read`；只按 Esc，不点击或滚动，观察主窗口仍活跃时发出一次标读且数据库水位推进。停在旧位置或菜单期间切到另一个应用时不标读。检查草稿、回复及菜单的撤销／重做不受影响，并与主菜单原有行为对照。GNOME/Xorg 实测基线中 Esc 恢复了编辑器的 `PopupFocusReason` 焦点，但没有新窗口激活事件，水位停留在 109；修复后只对编辑器自己的标准菜单接入现有关闭回调，水位推进到 110，主菜单也没有重复提交。

双机验收要指定实际 Qt 平台，例如 `reading_navigation.py <probe> --native --platform xcb` 或 `--platform wayland`。后者只接受 Qt 原生 Wayland 插件，不把 Wayland 会话中的 XWayland 当通过。SSH 远端应使用已登录用户的环境（`XDG_RUNTIME_DIR`、Wayland socket、会话总线）；登录界面与离屏结果不算原生测试。观察输出提供 Qt 版本/DPR、预编辑和对话框几何。用真实桌面输入操作候选和文件对话框；`capture` 指令可让隔离接收端下载自己刚发出的测试附件，返回 ID、文件路径和 SHA-256，无附件时报告剩余的上传任务。截图取合成测试区域，不保存用户桌面的全图。

`network_stall.py <probe> [--native --platform xcb|wayland]` 单独验证静默传输停顿，默认离屏，仍需按上文准备 Release、私有 SQLite 和 Silo。测试用只绑定回环的原始 TCP 转发，不解析 WebSocket，也不替两端回复 Ping 或 Pong；对已停顿连接不转发 EOF。按生产默认 30 秒探测加 10 秒宽限等待，分别验证下行停顿和双向停顿、原 ID 重试、离线 Outbox、归档外历史补齐、原安装的上传续传、遗漏撤销通知后的凭证拒绝，以及恢复后只剩预期连接和定时器。

## 重复搜索测量

先用上面的命令构建，或单独构建同配置的 Release `server`。计时工具按该版本的编译数据库重编三个临时翻译单元，链接独立服务端。生产程序没有计时开关。比较历史版本时，每个版本使用自己的源码、构建目录和计时程序。

当前发送探针在 `run_db()` 的业务回调内，计时从调用前开始。`run_db()` 先在数据库工作器上执行 `login_active()`，再进入被计时的 `persist_delivery()`。因此当前 `send_queue` 包含工作器排队及登录校验，不能和旧版的纯队列等待直接比较；`send_db` 仍为 `persist_delivery()` 的执行耗时。下方历史样本仍按原口径解读。

```sh
work=$(mktemp -d)
python3 ops/acceptance/measure_server.py /tmp/chat-release "$work/timed/server"
export CHAT_TEST_SQLITE_LIB="$SQLITE_PREFIX/lib/libsqlite3.so.0"
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/search_bench.py "$work/timed/server"
# 单独复测月库打开时间
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/search_bench.py "$work/timed/server" --case nohit --samples 30
```

本次基线为 `6a463be`，独立读连接版本为 `bc271fd`。主机为 i7-13700KF、24 逻辑 CPU、31 GiB 内存、Ubuntu 22.04.5，使用 GCC 16.2 和本地 NVMe。固定生成 120 个群、120,000 条消息；密集用户可见全部，稀疏用户只有一个群及其入群水位之后的 983 条。热门词为“发布”，稀有词每 3,001 条出现一次，深页游标为 12,000。单月将最早 6,000 条归档；双月再归档 6,000 条。

每个组合预热两次、采样 100 次，按最近秩取 P95/P99。搜索与发送来自不同 WebSocket，发起搜索 2 ms 后提交发送。队列时间从提交工作到进入 worker 计算；搜索执行时间包括准备语句、遍历结果和填写返回页；RPC 计时截至测试客户端收到响应。CPU 是该组含预热的服务端进程 CPU 秒，RSS 来自 Linux VmHWM。每组完成后记录 WAL 文件大小并执行一次 PASSIVE checkpoint。

| 无命中场景 | 搜索 RPC P95/P99，前 → 后，ms | 发送排队 P95/P99，前 → 后，ms | 发送确认 P95/P99，前 → 后，ms |
|---|---|---|---|
| 热库 | 139.32/143.17 → 147.37/154.67 | 136.82/140.78 → 0.067/0.084 | 148.31/153.38 → 11.77/12.00 |
| 单月 | 156.97/163.30 → 165.67/176.83 | 154.51/160.92 → 0.064/0.074 | 165.86/172.10 → 11.78/13.72 |
| 双月 | 208.12/230.91 → 195.02/199.73 | 205.36/228.49 → 0.069/0.070 | 216.52/230.98 → 11.76/12.19 |

18 组的服务端 CPU 合计由 99.58 s 变为 100.83 s，观测峰值 RSS 从 18,860 KiB 变为 19,192 KiB。两组 WAL 文件最大均为 21,461,112 字节，各组 checkpoint 均写回当时的全部有效帧。原始字面子串搜索仍随可见历史增长；独立连接主要消除了其他会话发送时的数据库队列等待。同一个 WebSocket 按协议处理循环依次执行请求；热库无命中期间在同一连接追加发送，另测 30 次确认 P95/P99 为 179.04/179.77 ms。完整两组数据见 [Release 测量表](search-release.csv)。

每条主库连接继续独占一个月库句柄和语句。双月无命中另测 30 次，每次读正文 12,000 次、换月打开 240 次，打开和准备月库累计耗时 P95/P99 为 12.95/13.44 ms，整条查询为 199.34/212.60 ms。该分布下保留单句柄；搜索队列串行、有界，每页结束即释放语句及读取快照。

## 恢复与部署核对

一次 Release 冷恢复的一致点为 2026-11-21 00:26:29 UTC，数据库与完整对象卷均已停止写入。恢复目标使用新目录、新 Silo 实例及端口，在独立卷上保留相同逻辑桶名、对象键和 multipart ID。Qt 通过原地址的测试代理重新连接恢复实例，保留原安装密钥、源文件和 SQLite 上传任务日志。

从开始校验备份计时，到恢复实例接受登录为 **0.838 s**，到跨月权限、续传、下载、最后引用回收与对象审计结束为 **1.771 s**。样本含两个月档、一个多引用对象和一个暂停的 17 MiB 上传。恢复前后通过精确 key 和 upload ID 调用 ListParts 对照第一片；另一设备被拒绝，原 Qt 确认的首个续传分片为第 2 片，文件下载内容一致，任务日志已清理。将隔离任务的 touched 回拨 14 天后，恢复活动会刷新 touched。本次测的是时间边界和重启恢复，实际观察时长未达 15 天。

本机 `chat-silo` 容器经只读 admin info 确认为一个在线节点，版本 `RELEASE.2026-11-12T00-00-00Z`、提交 `44e848fedf8c`。admin config get 返回 `stale_uploads_expiry=24h`、`stale_uploads_cleanup_interval=6h`；容器定义和 PID 1 启动环境均未见这两项的变量覆盖，配置的相对环境文件在其工作目录中不存在。该部署的 Silo 从创建计龄，与 Chat 最后活动起 15 天的规则不兼容。本次未发现能证明业务分片已经丢失的证据。其他集群需逐节点核对版本、API 生效值及环境覆盖，再对已知活动任务比较 ListParts、HEAD、数据库引用与回收队列。诊断命令见[运维手册](../README.md)。

提高创建时长阈值可以缓解过早清扫。有总上传寿命上限的部署，阈值至少应覆盖从创建到最后允许活动的最长时间，再加 15 天；允许持续恢复时，单一有限阈值无法覆盖所有任务。要满足完整约定，应先核实对象服务支持由 Chat 管理这些未完成上传的过期，或支持按活动计龄，再单独安排部署变更。此版本的桶生命周期接口不支持 AbortIncompleteMultipartUpload。[Silo multipart 兼容性说明](https://silo.pigsty.io/reference/s3-api-compatibility/#differences-from-s3-apis-for-multipart-uploads)

以下为仓库内静态接入之前的历史记录。彼时本机服务端和运维 Python 3.10 的 `_sqlite3` 都链接到 Ubuntu `libsqlite3-0:amd64 3.37.2-2ubuntu0.8` 的 `libsqlite3.so.0.8.6`，dpkg 文件校验一致。核对该精确版本源包中应用的全部 17 个 Debian 补丁，未含 WAL-reset 修复；Debian 补丁包 SHA-256 为 `6cf7d3db197bd5e76c81876e3a4536c75a322cdbb1c8d6cffc507de5adceb969`，与发布的 dsc 一致。上游修复覆盖 3.51.3，并有较早分支回补。问题要求特定的写入与 checkpoint 并发；新增连接只读且不执行 checkpoint。本轮未观察到 Chat 数据库损坏。其他主机请分别核对服务进程加载的库和运维解释器的库、包修订及源补丁，再决定升级。[Ubuntu 源包清单](https://security.ubuntu.com/ubuntu/pool/main/s/sqlite3/sqlite3_3.37.2-2ubuntu0.8.dsc) · [SQLite WAL-reset 说明](https://sqlite.org/wal.html#the_wal_reset_bug)

### 同一 WebSocket 复测与原始样本

先按本节构建计时程序，确认 Release 二进制对应的 Git SHA（不是测试脚本的版本），然后使用相同环境分别执行：

```sh
for mode in same cross; do
    python3 ops/acceptance/search_bench.py "$work/timed/server" \
        --mode "$mode" --layout hot --case nohit --case idle \
        --sha "$(git rev-parse HEAD)" --raw "$work/search-$mode.json"
done
```

测试客户端使用独立 RPC ID；同一连接只有一个收帧协程。实际提交间隔和搜索响应前已提交的次数写入结果，无需根据计划的 2 ms 间隔猜测是否重叠。历史 CSV 和新的独立测量不可合并。[2026-11-21 本机 Release 记录](results/README.md)。

## 消息收藏验收

先构建 Release 服务端和 Qt 探针（`build_probe.py`），设置 `CHAT_TEST_SERVER` 和 `SILO_BIN`。
下面命令使用独立虚拟桌面与私有会话总线，不使用已登录桌面。省掉 `--probe` 可只跑服务端；
可另传 `--previous-server /path/to/old/chat_server` 验证旧库原地升级、原消息收藏和重启持久化。

```sh
runtime=$(mktemp -d)
env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$runtime" XDG_SESSION_TYPE=x11 QT_QPA_PLATFORMTHEME=generic \
    xvfb-run -a -s '-screen 0 1280x900x24' dbus-run-session -- \
    ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/bookmarks.py --probe "$work/probe"
rmdir "$runtime"
```


## 消息多选验收

设置 `CHAT_TEST_SERVER` 和 `SILO_BIN`，用现有 `build_probe.py` 链接探针后运行：

```sh
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/message_selection.py "$work/probe"
```

脚本自行启动 Xvfb、无桌面服务自动激活的私有 D-Bus 和临时客户端配置。不要改为复用登录显示或剪贴板。
