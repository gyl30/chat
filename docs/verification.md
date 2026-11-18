# 完整本地验证

在仓库根目录运行：

```sh
tests/verify.sh
```

入口顺序配置、构建和运行正常 Debug、ASan、UBSan 的完整 CTest，三套均启用 Qt 和 TUI（`CHAT_BUILD_QT_CLIENT=ON`、`CHAT_BUILD_TUI_CLIENT=ON`），构建使用 `-j12`，最后执行 `git diff --check`。首次失败立即退出，不排除测试、不设置 sanitizer suppression。可以从任意目录调用该脚本。

## 环境前提

- CMake 3.30 或更新。
- 支持 `-std=c++26 -freflection` 的 GCC 16；2026-10-10 本机验证使用 GCC 16.2.0。静态反射由现有 simdjson CMake 配置启用。
- Boost **1.92.0** 的 CMake package；当前安装于 `/usr/local`。非默认安装位置可通过 `CMAKE_PREFIX_PATH` 提供，首次配置的编译器使用常规 `CC/CXX` 环境变量。
- OpenSSL、PostgreSQL/libpq、libpng、libjpeg、Qt 6.5+ Widgets/Svg 开发包及 Qt offscreen 平台插件；`server_tls` 还需要 OpenSSL 命令行工具生成临时测试证书。
- TUI 使用固定的 FTXUI v7.0.3 submodule，无 Qt 或 curses 依赖。
- 按仓库 gitlink 初始化第三方依赖：`git submodule update --init --recursive`。

数据库连接完全继承 libpq 的 `PGHOSTADDR/PGPORT/PGDATABASE/PGUSER/PGPASSWORD` 环境变量，不在脚本中保存地址或凭据。使用已依序应用 `sql/001_*.sql` 至当前最新 migration 的**专用测试数据库**；账号需要建表及 `CREATE SCHEMA` 权限。若通过 `.pgpass` 等 libpq 机制认证，不必另设密码变量。

仅在新建的空测试库上首次应用迁移：

```sh
(
    set -e
    for migration in sql/[0-9][0-9][0-9]_*.sql; do
        psql -v ON_ERROR_STOP=1 -f "$migration"
    done
)
```

既有测试库只应用尚未执行的新 migration，不能重放整个目录。验证脚本本身不迁移数据库。migration 测试在独立临时 schema 验证升级并清理；它不会建立其他集成测试所需的默认 schema。

三套 CTest 及各套内部均顺序执行，显式 `--parallel 1` 避免环境中的并行设置干扰。服务器和 Qt 测试共享默认 schema，Qt UI 使用本机 TCP 18769；不要同时启动另一个验证进程或占用该端口。Qt delegate/UI 的 CTest 已配置 `QT_QPA_PLATFORM=offscreen`，不需要显示服务器。中断可能留下测试 fixture，因此不要对保存真实用户数据的数据库运行集成测试。

## GitHub Actions 评估

当前没有接入远端 CI。Qt offscreen 和 PostgreSQL service 可在 hosted runner 上使用，主要缺口是经过项目完整验证的工具链分发：本机使用经过验证的 GCC 16 构建和自行安装的 Boost 1.92，仓库还没有可复用的固定工具链镜像或安装基线。[GCC 官方](https://gcc.gnu.org/gcc-16/changes.html) 已记录反射支持，但 C++26 支持仍属实验能力；[Ubuntu 24.04 runner 清单](https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md) 的预装 GCC 最高为 14，不能直接构建当前项目。Boost 1.92 已有[官方发布](https://www.boost.org/releases/1.92.0/)，不能用 runner 自带的旧版本替代。

本阶段先保留已经实际跑通的本地入口，不提交依赖滚动 PPA 或未经验证编译器组合的 workflow。以后若建立固定且可复现的 GCC/Boost 获取方式，CI 应使用 PostgreSQL service 初始化空测试库并调用同一入口，保持服务器、Qt 和 sanitizer 全部测试；不以删测试或关闭静态反射来迁就 runner。

## 独立安装验收

Phase 3 可搬运 tar.gz 的生成、依赖边界和跨机器验收另见
[Linux 发布包](linux_release.md) 与 [验收记录](release_phase3_acceptance.md)。
不要把普通 install、可搬运运行库验收、原生 Fcitx 分发审核视为同一个结论。

启用所需客户端构建后，用 `cmake --install build --prefix <独立目录>` 验证安装，
不要只检查构建树可运行。默认应有三个 `bin/chat_*` 和五个 `share/chat/themes` 文件；
关闭 Qt/TUI 时对应目标不安装。原生 Fcitx5 Qt6 插件作为 README 描述的独立部署步骤。

从源码/构建目录外启动安装程序，清除开发环境的 `LD_LIBRARY_PATH`、`QT_PLUGIN_PATH`
和平台插件路径覆盖；保留正常桌面会话、隔离测试库的 libpq 配置和 WSS 所需的有效信任库。
记录 `ldd`、`ldd -r`、`readelf -d` 与实际进程加载位置。外部 SDK 的运行库/插件仍须存在，
不得将普通 CMake install 描述为自包含包。Qt 主题应来自安装目录，SVG 来自内置资源。

用真实安装版 Qt/X11、TUI/PTY 或 tmux及安装版服务器验证 WS/WSS 基本收发、亮暗主题、
原生 Fcitx 候选与两种发送键。仅夹具建立/最终事实检查可复用 SDK/SQL；被测消息通过原生
客户端操作。使用专用测试数据库与临时证书，不修改业务库、系统输入法或 Qt SDK。
记录失败、修复、进程退出及资源清理。安装规则改变时保留原有 Normal/sanitizer 回归；
仅外部插件复制无需重复执行全部服务器测试。

## 部署、升级与恢复验收

全部操作使用本轮创建、身份和所有权已核对的专用数据库，不能假设已有测试库可清空。
记录 PG 地址、端口、角色、数据库名和程序版本；凭据不写日志。
安装前缀的 `share/chat/sql` 应含全部已发布迁移，与源码逐文件 SHA-256 一致。
新空库以 `ON_ERROR_STOP` 顺序应用；历史 025 库先备份，只执行 026–028。

历史夹具在迁移前覆盖密码哈希、双向/单向联系人、会话、群角色、消息回复/编辑/删除、
已读、置顶、公告、reaction、附件和头像。比较全部字段与序列状态，
只允许 026 的联系人→申请转换、027 的旧邀请码撤销及 028 新增的空 `client_id` 列。
使用 `pg_dump --format=custom` 和 `pg_restore --single-transaction --exit-on-error`
恢复到另一新空库；核对完整记录及附件 SHA-256，不以记录数替代内容检查。

从源码外启动安装版 Server、Qt/X11 和 TUI，分别检查 WS/WSS 的注册、好友、单聊、群聊、
文件/图片、正常与异常服务端退出、自动重连和重新认证、消息/已读/草稿、断线时未确认消息的自动重发（服务端只存一条）及恢复后新消息。
数据库升级/恢复验证和客户端重连验证分别记录，不把重放迁移当作恢复。
失败保留原始日志；有界重复心跳测试不得删除断言、增加 sleep 或放宽超时。
只清理本轮创建且身份可核对的数据库、集群与进程。

## SQLite 与 Silo 存储层阶段验证

重构分支新增 `database` 和 `s3_objects`。当前生产服务仍使用原数据库入口，旧的
集成测试仍需要上文的专用 PostgreSQL 库。两个新测试直接验证将来由服务端接入
的存储模块，不能替代切换后的 Qt/TUI 端到端验收。

构建需要 SQLite3 开发包和 MinIO C++ SDK 1.0.1 的 CMake 包。将从源码构建的
SDK 安装前缀加入现有 `CMAKE_PREFIX_PATH`。已验证 Ubuntu 22.04 上从源码构建的
SDK。上游 Linux 二进制包依赖本机 glibc 2.35 不提供的符号。

```sh
cmake -S . -B build -DCHAT_BUILD_QT_CLIENT=ON -DCHAT_BUILD_TUI_CLIENT=ON
cmake --build build --target chat_database_test chat_s3_objects_test -j12
ctest --test-dir build -R '^(database|s3_objects)$' --output-on-failure
```

SQLite 测试创建并清理自己的数据库文件。S3 测试使用独立 Silo 实例，测试环境通过
`CHAT_TEST_S3_ENDPOINT`（`host:port`）、`CHAT_TEST_S3_ACCESS_KEY` 和
`CHAT_TEST_S3_SECRET_KEY` 提供连接参数。夹具在这个实例创建私有测试桶并清理。
缺少连接参数时测试返回 77，CTest 显示跳过，验收记为 BLOCKED。
此测试使用本机 HTTP 并通过独立的回环故障夹具检查拒绝连接、截断响应和超时。
测试向真实 Silo 同时提交两个上传和一个范围读取。回环夹具另外保留一条不响应的连接，
验证短读能在慢请求到期前完成。单工作线程实测短读等待约 15 秒，改为两个线程共用
容量为 8 的队列后低于 1 毫秒。SQLite 仍由单线程执行。

`database` 通过 SQLite authorizer 复现回滚失败。仍在事务中的连接会关闭，
下个任务重新打开同一个数据库文件，验证只保留新提交的数据。历史分页测试使用
`sqlite3_trace_v2` 统计包含事务在内的语句。一页 50 条消息从 104 条 SQL 降为 6 条，
该夹具单次耗时从 736 微秒降为 343 微秒。

### 存储工作器的停止顺序

固定版本的 Capy 使用协作式停止。 `run_async` 持有任务帧到完成，并通过 work guard
保持执行器有效。 `task::handle()` 禁止主动销毁仍在等待中的任务。 Corosio 的
`tcp_server::stop()` 请求会话停止，事件循环排空后才调用 `server.join()`。
在 Chat 的会话协程中，断开连接会结束 socket 操作，正在等待的存储任务仍由该协程持有。
接入新存储后应保留这个顺序，让会话完成后再销毁数据库及对象工作器。

`database` 测试同时暂停两个工作器的任务，发出停止请求并暂停事件循环，
排空工作器后重启上下文，验证两个结果各交付一次。现有测试另覆盖队列满、
连续完成和关闭排空。此测试不假设主动销毁悬挂协程合法，之后仍需通过生产 RPC 验证断线。
