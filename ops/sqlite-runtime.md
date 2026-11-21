# Linux 私有 SQLite 运行时

Ubuntu 22.04 的 `3.37.2-2ubuntu0.8` 未包含 SQLite 的 WAL-reset 修复。这个上游问题需要两个连接并发写入或执行 checkpoint；独立搜索读连接本身不执行这两项操作。本机候选选用 [SQLite 3.51.3 官方修复版](https://sqlite.org/releaselog/3_51_3.html)，没有替换发行版库。若维护此方案，升级时重新核对源码、链接和运行时，不以命令行程序的版本代替验证。

## 准备与验证

选择部署时会保持不变、且只有授权管理员能写入的独立安装前缀。构建时传入项目所需的现有 Qt 和 miniocpp 搜索路径。以下命令不会安装或替换全局 SQLite：

```sh
curl -fLO https://sqlite.org/2026/sqlite-autoconf-3510300.tar.gz
ops/build-sqlite-runtime.sh sqlite-autoconf-3510300.tar.gz "$SQLITE_PREFIX"

CC=gcc-16 CXX=g++-16 cmake -S . -B "$RELEASE_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCHAT_BUILD_QT_CLIENT=ON \
    -DSQLite3_INCLUDE_DIR="$SQLITE_PREFIX/include" \
    -DSQLite3_LIBRARY="$SQLITE_PREFIX/lib/libsqlite3.so"
cmake --build "$RELEASE_BUILD" --target chat_server chat_client chat_qt
ldd "$RELEASE_BUILD/chat_server" | grep libsqlite3

ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" /usr/bin/python3 -c \
    'import sqlite3; print(sqlite3.connect(":memory:").execute("select sqlite_source_id()").fetchone()[0])'
CHAT_TEST_SERVER="$RELEASE_BUILD/chat_server" \
CHAT_TEST_SQLITE_LIB="$SQLITE_PREFIX/lib/libsqlite3.so.0" \
    ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/acceptance/wal.py
```

预期 source ID 为 `2026-03-13 10:38:09 737ae4a34738ffa0c3ff7f9bb18df914dd1cad163f28fd6b6e114a344fe6d618`。构建脚本先校验官方压缩包的已知 SHA-256，解压后再比对发布说明公布的 `sqlite3.c` SHA3-256。候选构建的服务端 ELF 直接引用私有库的绝对路径；移动到另一前缀须重新配置链接，不要依赖系统回退。Python 的 `_sqlite3` 仍由系统提供，包装器只为当前命令及子进程指定同一版本库。

隔离验收核对了服务端和 Python 的实际映射路径，并同时发出消息写入和被动 checkpoint。发送幂等、个人清空、搜索权限及分页、数据库完整性和外键检查通过。用相同库执行了月档、备份、恢复和 Qt 回归。压力测试只验证集成，修复依据是官方版本和源码校验。

## 切换与回滚

实际部署前确认服务名、数据库、定时运维入口和备份窗口。停止 Chat 写入及定时归档/备份/checkpoint，保存原服务可执行文件、启动定义、运维入口和可验证热库/月档备份。切换到该前缀构建的服务端，所有 `chat_admin.py` 入口（包括归档、WAL、备份和恢复）改为：

```sh
/path/to/chat/ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" /usr/bin/python3 /path/to/chat/ops/chat_admin.py ...
```

启动后核对服务进程的 `/proc/PID/maps` 及相同包装器下 Python 的 source ID；在重开写入前用隔离探针和已授权的生产只读检查确认健康。若切换失败，停止新服务和运维任务，恢复旧二进制和旧启动/运维入口，不替换全局库、不直接删除 WAL/SHM。对退回未修复库的临时窗口避免并发手动 checkpoint；只有确需恢复时才使用成对备份，不能覆盖切换后已接受的新写入。
