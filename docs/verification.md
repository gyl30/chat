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
