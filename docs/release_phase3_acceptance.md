# Release Readiness Phase 3 验收记录

## 1. 版本与范围

- BASE_HEAD：`28f588a3d1b7300eeffdd459f9863ce8042d5328`。
- 发布工具及发布包源码：`4d378d1380c01d35253eb70a1ae48587ab7ae395`。
- 部署流程文档：`6c276aa`；本记录为独立后续文档提交，不修改应用程序。
- 分支 `main`，第三方 gitlink 未改变；没有修改业务、SDK、SQL 或标准安装规则。
- 目标是 Ubuntu 22.04.5 / x86_64 的可搬运包，不是通用 Linux 发行系统。
- 按用户确认，不捆绑可选 Fcitx5 Qt6 插件；其分发许可及原生输入另行确认。

本轮新增 `tools/package_linux.py`、`tests/package_linux_test.py`，更正
`THIRD_PARTY.md` 中 Fcitx 编入文件的许可说明，补充 README 和发布/验证文档。
不是以业务代码修改适配目标机。

## 2. 两机环境

| 项目 | DEV 172.20.45.187 | TEST 172.20.54.83 |
| --- | --- | --- |
| 发行版 | Ubuntu 22.04.5 | Ubuntu 22.04.5 |
| 架构 / glibc | x86_64 / 2.35 | x86_64 / 2.35 |
| 内核 | 6.8.0-136 | 6.8.0-124 |
| 编译 / SDK | GCC 16.2.0、CMake 3.31.10、Boost 1.92、Qt 6.5.3 | 无需这些开发包 |
| 字体 | 实际匹配 Noto Sans CJK SC | 实际匹配 Noto Sans CJK SC |
| 数据库 | 独立 PostgreSQL 14.24 用户进程，用于完整回归 | 用户已有 PostgreSQL 服务，实际查询版本 17.11 |

TEST 没有 psql/pg_dump、Xvfb、Openbox、xclip，也缺少 `libpq.so.5` 和
`libxcb-cursor.so.0`。没有安装或修改系统软件：libpq/cursor 随发布包提供；
测试用图形工具从原包解压到本轮用户目录，只作用于独立 Xvfb/DBus。
原有物理 X11 桌面保持不动；不能把独立 Xvfb 结果写成物理桌面或 Fcitx 验收。
开发机也只解压本轮 patchelf 和私有 PostgreSQL 工具，没有 sudo 或全局安装。

## 3. 发布包和完整性

```text
文件：chat-4d378d1380c0-linux-x86_64.tar.gz
大小：231177642 bytes（ls 显示约 221 MiB）
SHA256：65b23afacbf12f2b172caefa464e7420a4160322eacfa2a4f24ec1fa5d10612e
DEV：/home/gyl/chat-review/release-phase3/package-07/
TEST：/home/gyl/chat-review/release-phase3/release-4d378d1/
TEST 实际安装：/home/gyl/chat-review/release-phase3/release-4d378d1/chat relocated/
```

包含三个应用、22 个私有库、9 个 Qt 插件、五个主题、SQL 001–027、完整第三方
声明和 Qt/GCC 三份源码归档。`manifest.json` 记录 237 个文件的 SHA（不含自身）。
两个目录独立运行相同输入的打包脚本，package-07/08 归档 SHA 完全相同；
只证明同环境重复打包，不声称跨工具链重新编译字节一致。

DEV 本地独立解压并整体移动到带空格的新路径，从无关 cwd 启动 Server、Qt 和 TUI。
Server `/health` 返回 200，Qt 进程 maps 与日志显示包内 Qt/offscreen 插件；
TUI 的真实 PTY 登录页显示，键盘 Ctrl+C 退出 0，终端属性恢复。
目标机通过已有安全 SSH/SCP 路径复制、SHA 校验、解压和整体重定位。
最终包全部 34 个 ELF 的 `ldd -r`、私有库实际解析路径和文件 SHA 均通过；
最高被引用的 GLIBC 符号为 2.35，没有未解析符号或开发机 RPATH。

先验收的 package-04 源码为 BASE_HEAD；其全部应用 ELF 与最终 package-07 相同。
最终新增内容是打包元数据和许可来源说明，不把旧 SHA 冒充新包。

## 4. 库与许可边界

实际私有库版本如下；详细逐库来源及 SHA 见包内 manifest：

| 库 | 版本 |
| --- | --- |
| Qt Core/Gui/Widgets/Svg/DBus/OpenGL/XcbQpa | 6.5.3 |
| ICU data/uc/i18n | 56.1 |
| Boost json/container/url | 1.92.0 |
| libstdc++ / libgcc_s | GCC 16.2.0；libstdc++ 6.0.36 |
| OpenSSL ssl/crypto | Ubuntu 3.0.2-0ubuntu1.26 |
| libpq | Ubuntu 14.24-0ubuntu0.22.04.1 |
| libpng | 1.6.37-3ubuntu0.5 |
| libjpeg-turbo | 2.1.2-0ubuntu1 |
| zlib | 1:1.2.11.dfsg-2ubuntu9.2 |
| xcb-cursor | 0.1.1-4ubuntu1 |

glibc/loader、图形驱动、其余 X11/XCB、字体、DBus/GLib 和 libpq 的
Kerberos/LDAP/SASL/GnuTLS 等由系统提供；完整 68 个 SONAME 见 manifest。
包不依赖开发机 SDK 的绝对运行路径，但不声称完全无外部依赖。
字体只核验匹配，没有复制系统字体。Qt 库及插件从包内相对路径加载。

完整 Qt/GCC 源码、Qt attribution、ICU 56.1 原始声明、静态链接依赖及 Ubuntu
实际安装包版权文本随包保留。GCC 来源和构建说明也在 licenses 中。
Qt SDK 标为 OpenSource 6.5.3，官方同版源码已提供，但 SDK 原始下载 manifest、
完整构建配置与补丁历史没有恢复；没有宣称 SDK 完整源码对应关系或法律审计已闭合。
Fcitx plugin-only 编入 GPL-2.0-or-later 文件，本轮明确排除，不以 IBus/Compose 替代。

## 5. 验证结果

| 项目 | 结果 | 证据 / 边界 |
| --- | --- | --- |
| Normal 完整构建 / CTest | PASS，29/29 | verify-normal-asan-ubsan.log，127.13 s |
| ASan 完整构建 / CTest | PASS，29/29 | 同日志，255.62 s；外部 Qt 等库不是全部插桩 |
| UBSan 完整构建 / CTest | PASS，29/29 | 同日志，182.80 s |
| 打包离线守卫 | PASS，5/5 | package-guards.log |
| 发布包本地重定位 / 三应用启动 | PASS | local-check-final/result.json |
| 重复归档 | PASS | package-07/08/SHA256SUMS |
| TEST 归档 / ELF 校验 | PASS | elf-check-final/result.json，34 ELF / 237 文件 |
| TEST Qt ↔ Qt，WS | PASS | native-runs/ws-qt-qt-01.log；package-04，应用 ELF 相同 |
| TEST Qt ↔ TUI，WS | PASS | native-runs/ws-qt-tui-03.log；最终包，125% 缩放 |
| TEST TUI ↔ TUI，WS | PASS | native-runs/ws-tui-tui-04.log；最终包 |
| TEST Qt ↔ Qt，WSS | PASS | native-runs/wss-qt-qt-01.log；最终包，125% 缩放 |
| TEST Qt ↔ TUI，WSS | PASS | native-runs/wss-qt-tui-01.log；最终包 |
| TEST TUI ↔ TUI，WSS | PASS | native-runs/wss-tui-tui-01.log；最终包 |
| Qt / TUI 原生注册登录 | PASS | WS/WSS 各两次真实注册；数据库只读核对用户 ID 7–10 |
| 物理桌面 / 原生 Fcitx5 | NOT_RUN | 本轮使用独立 Xvfb；插件按用户选择不分发 |

原生驱动复用现有 NativeGui、NativeTui、已安装 Qt 矩阵中的缩放处理和 Chat
测试 fixture。前置账号、联系人及群可由 Chat 协议 SDK 设置；随后关闭占用被测
账号的 SDK 连接。关键消息、好友接受、聊天导航、附件上传/下载/保存均由实际
发布版客户端操作，没有用 SDK 消息替代或伪造 UI。群创建属于夹具前置，未单独
宣称原生建群流程通过。

六个配对实际 runner 均退出 0，`passed=true`、`cleanup_ok=true`，汇总见
`native-client-acceptance-summary.json`（DEV 副本在 `native-evidence-target/`）。
22 次附件传输均源/保存 SHA256 一致：12 个二进制文件、10 个 PNG；首个 WS
Qt/Qt 仅验证二进制，其余五个场景含 PNG。最终 SQL 审核也确认 22 个源 SHA
均实际存在数据库附件中，不仅是客户端保存文件相同。

已通过的 WS/WSS 配对覆盖双向中文/Emoji 私聊及群聊、原生历史查询、两方向不同 payload
的文件源/保存 SHA、正常和异常服务端重启后的草稿保留及新消息。
混合配对还覆盖双向 PNG 与 125% Qt 缩放；主题实际切换 Daylight/Midnight，
截图与 maps 证明 XCB、SVG、中文和 Emoji 实际运行。不是 Qt offscreen 代替目标验收。
WSS 采用专用 CA 和匹配 localhost 的证书，客户端正常校验证书及主机名。
每个配对实际执行一次正常停服和一次故障注入 SIGKILL：正常退出 0、故障退出 -9，
均已 wait 回收；不能把故障注入退出码写成正常退出。TUI 原生 Ctrl+C 退出 0 并恢复
终端，Qt 原生关闭退出 0。TUI 草稿在恢复后重新打开相同聊天验证，不声称自动恢复焦点。

## 6. 原始失败记录

没有删除失败日志，也没有把诊断重试计为通过：

- package-01：分析已复制但尚无依赖的 Qt 插件，误解析系统旧 Qt；改为分析原 SDK
  插件闭包，后续再严格核验发布包。
- package-02：系统 FreeType 先加载系统 PNG，RUNPATH 未覆盖间接依赖；改相对
  DT_RPATH，并要求全部私有库的实际解析位置属于包内。
- package-03：DEV 可运行，但 TEST 缺 xcb-cursor；只补包内库和对应版权，不装系统包。
- 本机 local-check-01：测试驱动发系统 SIGINT，而 TUI 正常退出契约是键盘 Ctrl+C；
  改驱动输入，正常退出 0 和 tty 恢复另行实际验证，未修改业务代码。
- TUI/WS 01–03：驱动错误要求离线输入框可编辑、误设恢复焦点、将不在 viewport
  的旧消息误判丢失；改为被动草稿读取及现有原生历史搜索，保留实时收发断言。
- 混合/WS 启动及 02：前置 Python 文件权限 API 错误；125% 缩放漏用现成坐标
  适配导致错点和强制回收。保留这些失败，不把强制退出记为正常退出 PASS。
- ICU 可选源码下载超时，未完成归档不进入包或 SHA 清单；完整 ICU 原始许可已提供。
- 最后一个 WSS TUI/TUI 场景外层 SSH 返回 255；独立读取目标 launcher JSON 和
  原始日志确认 native runner 已实际退出 0、通过并清理，随后 SSH/SCP 重新连接和归档
  校验成功。保留传输失败，不把它冒称 SSH 成功。AT-SPI 在独立总线关闭后的尾部警告
  同样保留，未隐藏或修改 runner 的退出判断。

## 7. 数据安全与清理

TEST 只在用户授权创建的 `chat_release3_20261014_8c03afd4` 上迁移和测试；
业务数据库 `chat` 没有被修改。DEV 回归使用独立用户集群与数据库。
凭据在仓库外 0600 文件，证据目录受限；失败图片可能包含错点时的测试口令，
不得公开或提交。原始日志、截图、媒体/附件、归档及私钥均不提交 Git。
所有测试均使用专用临时证书、独立 Xvfb/DBus/tmux；不动用户物理桌面。

最终核验：测试库有 10 用户、145 消息、28 附件；22 个实际原生传输 SHA 均持久化。
目标同库其他活动连接为 0，随后删除该独立测试库，并查询确认不存在；DEV 独立
`chat_release_phase3` 也已删除，核对 PID/exe/data-dir 后停止私有 PostgreSQL。
证据为 `database-persistence-final.json`、`database-cleanup.json`、`database-cleanup.log`。

77 个已记录测试进程 PID/starttime 代际无存活残留（汇总
`recorded_owned_generations` / `live_recorded_owned_generations`）。
只删除本轮明确列出的 DEV 11、TEST 14 个临时凭据、证书、私钥/CSR 文件；两机
`private/` 中普通文件剩余为 0，公开证书指纹及删除路径保存在 `secret-cleanup.json`。
文件删除不可从原路径恢复，需要时重新创建测试凭据/证书。未删除原始测试日志、截图、
失败记录或发布包；测试账号随独立数据库删除失效，原证据仍为受限资料。
未修改目标系统软件、网络、输入法、全局配置或现有数据库/应用。

## 8. 使用和结论边界

打包、SCP、启动、系统依赖及 WSS 命令见 [Linux 发布包流程](linux_release.md)。
DEV 和 TEST 原始证据均保存在 `/home/gyl/chat-review/release-phase3/`，不覆盖历史阶段。
DEV `SHA256SUMS-evidence.txt` 固定保存 632 个关键结果、原始日志、截图、归档和
manifest 文件的 SHA256；不包含已清理的私有凭据。原始证据受限，不进入 Git。
交付包固定对应 4d378d1；后续提交只补部署和验收文档，不改变应用 ELF。

| 最终项目 | 结论 |
| --- | --- |
| 构建 / 本机发布包运行 | PASS |
| 目标 Server / TUI / Qt（独立 XCB/Xvfb） | PASS |
| WS / WSS 六配对、附件与重连 | PASS |
| 可搬运性（两台 Ubuntu 22.04.5 x86_64 / glibc 2.35） | PASS |
| 目标物理桌面 / 原生 Fcitx5 | NOT_RUN；按用户选择不捆绑插件 |
| Qt SDK 完整对应源码来源 / 公开分发许可闭合 | BLOCKED：缺原 SDK 构建及补丁 provenance |

整体结论 **CONDITIONAL GO**：以上明确平台上的实际运行与搬运通过；原生 Fcitx
和 Qt SDK 完整源码对应来源仍有明确限制，不宣称所有桌面/发行版或公开分发条件
全部通过。本轮仅本地提交，不 push：用户授权是在全部验收通过后推送，当前尚未达到
无条件完整发布结论。没有 force push、依赖升级或后续功能开发。
