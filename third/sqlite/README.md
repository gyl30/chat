# SQLite 3.51.3 amalgamation

这些文件未作改动，取自 SQLite 官方 [3.51.3 发布版](https://sqlite.org/releaselog/3_51_3.html)的 [amalgamation ZIP](https://sqlite.org/2026/sqlite-amalgamation-3510300.zip)。SQLite 源码属于 [public domain](https://sqlite.org/copyright.html)。仅保留 `sqlite3.c` 和同包的 `sqlite3.h`，不构建 CLI 或扩展。

- Source ID: `2026-03-13 10:38:09 737ae4a34738ffa0c3ff7f9bb18df914dd1cad163f28fd6b6e114a344fe6d618`
- ZIP SHA-256: `acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4`
- `sqlite3.c` SHA-256: `9512509b1bccb7461f79bea8aad6280ae4699e925fa4804381b71f59e7efb0c5`
- `sqlite3.h` SHA-256: `19585c8b5230e9d4f223bf31b709ece7b6a0bb3faf00d8310625d8e58cda1b1d`
- `sqlite3.c` SHA3-256（发布说明公布值）: `32d5424f97e0a7fc5ed2f6335afbb58be4e0298bd7117a34e39d345ff13d859e`

更新时从同一个新的官方 ZIP 提取这两个文件，核对官方 source ID 和 `openssl dgst -sha3-256 sqlite3.c`，更新本页校验值。重新构建 C++ 与 [Python 私有运行时](../../ops/sqlite-runtime.md)，核对两者实际 source ID，跑 WAL 并发及归档恢复验收。不要把 autoconf 包的校验值当成 ZIP 校验值。
