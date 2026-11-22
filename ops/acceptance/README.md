# Linux 隔离验收

在专用测试环境运行。使用项目要求的 GCC 16、C++26 Reflection、CMake、Qt 和[系统开发包](../../README.md#构建)，另需 Ninja、Python 3.10+、websockets 15、boto3、dbus-python、PyGObject、dbus-run-session、Silo 与 mc。把 Boost、Qt 等必要前缀加入 CMake 搜索路径，不使用 SQLite 或 miniocpp 安装前缀。若 Silo 不在 PATH 中，用 SILO_BIN 指定可执行文件。

```sh
ops/build-sqlite-runtime.sh "$SQLITE_PREFIX"
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" ops/acceptance/linux.sh /tmp/chat-release
```

脚本构建三个生产目标，在临时目录链接 Qt 驱动。驱动操作实际主窗口、菜单和模型，使用生产 SDK、资源和主题。验收创建临时账号、SQLite、Silo 卷、私有会话总线及 Qt 应用目录；测试客户端连接本机随机端口。失败返回非零。请使用普通的 Python 运行模式，以执行断言。

| 入口 | 观察的结果 |
|---|---|
| `clear.py` | 重试相同 H 后，后续未读消息与手工标记保留，实际已读位置保持，已提交任务得到无正文确认 |
| `desktop.py` | 真实 RPC 代理延迟或丢弃指定回包，验证 Qt 草稿、Outbox、通知、三页搜索游标与迟到上下文；验证图片和保存子窗口、群资料置顶、多目标上传菜单和真实 SDK 附件下载 |
| `recovery.py` | 停止隔离 Chat 与 Silo，分别备份数据库、月档及整个对象卷；启动独立恢复实例，用原 Qt 安装的任务日志从第 2 片继续，再检查消息权限与对象引用 |
| `wal.py` | 验证静态服务端与私有 Python 的 SQLite 身份、WAL 并发 checkpoint 和搜索分页 |
| `history.py` | 真实 Qt 覆盖 3,100 条较新消息后的分类和 around；`--baseline` 可用旧服务端、旧版 Qt 测试驱动复现自动补页上限 |
| `search_filters.py` | 组合条件、无关键词、隐藏私聊、权限、50/51 页边界及字面空格 |
| `live_history.py` | 双设备实时编辑与撤回、显示/剪贴板、筛选移除、乱序回包和保留服务端游标 |
| `drafts.py` | 真 Qt 重启与会话/账号/端点隔离；探针扣住本地入队完成回调直至输入新版本，另用隔离数据库故障确认保留文字和回复 |
| `deep_reply.py` | 最近一页之外的草稿引用、重启恢复、实际发送、编辑与迟到校验、撤回/清空/失权 |
| `downloads.py` | 两个会话及群文件的同名任务、取消、真实流截断、重新鉴权和撤回拒绝；`--stalls` 保持两个 TCP 流不再发送，检查客户端 EOF、临时文件、共享工作器、落盘/取消竞争及断线清理 |
| `history_filter_perf.py` | 对照两个 Release 服务端的稀疏历史、全局搜索及其他连接的发送确认；输出原始端到端样本 |
| `search_bench.py` | 在独立磁盘 WAL 库测量真实搜索与发送 RPC，输出每组延迟、CPU、内存、归档读取次数和被动 checkpoint |

默认项目构建不运行上述验收。查询 GitHub 时，仓库有 0 个注册的自托管 runner 和 0 个已有 workflow；本次 GCC 16、Boost 1.92 和 miniocpp 环境的执行证据来自本机。

## 重复搜索测量

先用上面的命令构建，或单独构建同配置的 Release `chat_server`。计时工具按该版本的编译数据库重编三个临时翻译单元，链接独立服务端。生产程序没有计时开关。比较历史版本时，每个版本使用自己的源码、构建目录和计时程序。

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
