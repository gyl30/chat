# SQLite 运行时

Chat 的服务端、SDK 和 Qt 使用 [third/sqlite](../third/sqlite/README.md) 中同一份 SQLite 3.51.3 amalgamation，以 C 源码构建后静态链接，不依赖发行版 SQLite。现有 WAL、外键、synchronous 由连接初始化，`SQLITE_THREADSAFE=1`。3.51.3 包含 [WAL-reset 修复](https://sqlite.org/releaselog/3_51_3.html)；只读搜索连接不执行 checkpoint，不是该错误的触发条件。

静态链接不会更新 Python 的 `_sqlite3`。Linux 运维仍用 Python 标准库，通过包装器仅给当前命令加载同一份源码生成的共享库，不替换系统库。选择新的、只有授权管理员可写的安装前缀，脚本只需 Bash 和 C 编译工具链：

```sh
CC=gcc-16 ops/build-sqlite-runtime.sh "$SQLITE_PREFIX"
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" /usr/bin/python3 -c \
    'import sqlite3; print(sqlite3.connect(":memory:").execute("select sqlite_version(),sqlite_source_id()").fetchone())'
ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" /usr/bin/python3 ops/chat_admin.py --db "$DB" check
```

新版 C++ 程序不需要这个前缀。核对实际已链接的服务端身份：

```sh
"$RELEASE_BUILD/server" --sqlite-version
CHAT_TEST_SERVER="$RELEASE_BUILD/server" \
CHAT_TEST_SQLITE_LIB="$SQLITE_PREFIX/lib/libsqlite3.so.0" \
    ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" /usr/bin/python3 ops/acceptance/wal.py
```

预期版本 `3.51.3`，source ID 为 `2026-03-13 10:38:09 737ae4a34738ffa0c3ff7f9bb18df914dd1cad163f28fd6b6e114a344fe6d618`。验收使用正在运行的服务端对应 ELF 查询版本和 source ID，并分别核对 Python 的 SQL 输出与 `/proc/self/maps` 加载路径，不依赖服务端进程映射中有 SQLite 共享库。

部署和回滚按[发布切换与回滚手册](release-cutover.md)。只回滚 Chat／SQLite 时不要附带重新启用 Silo 的 24h 清扫。旧候选在仓库内接入前链接过私有前缀，不能把旧候选误当成当前静态构建。
