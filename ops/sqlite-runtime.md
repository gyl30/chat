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

按[发布切换与回滚手册](release-cutover.md)操作。服务端和全部运维 Python 入口必须一起核对实际加载路径；只回滚 Chat／SQLite 时不要附带重新启用 Silo 的 24h 清扫。系统 SQLite 不变。
