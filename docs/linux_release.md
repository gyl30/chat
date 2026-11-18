# Linux 可搬运发布包

当前发行范围：`chat_server`、Qt Widgets `chat_qt` 和 Qt 主题；不再包含 TUI。
数据库已改用 SQLite，附件使用独立 Silo/S3 服务；不分发 PostgreSQL
或历史 SQL 迁移文件。旧 Phase 3 发布包及其 Qt/TUI/PostgreSQL 验收数据
属于历史记录，**不能作为当前版本的发布验收结果**。

## 构建及依赖

请以 [构建说明](../README.md#构建) 为准。现有打包工具要求 GCC 16.2.0、
Boost 1.92.0、Qt 6.5.3 的固定来源，以及 SQLite/MinIO SDK 的正确运行库依赖。
工具还需要 Python 3.10+、Git、patchelf、ldd 和 readelf。

打包脚本使用当前已提交、干净的 Git 工作树，刷新 CMake 构建，核对 Qt/GCC
源码归档和许可输入，收集 Qt 插件及所需共享库，检查 ELF RPATH、
DT_NEEDED 和动态依赖，写入版本与文件校验清单。它不会安装或启动 Silo。

## 打包

准备与实际 Qt/GCC 来源相匹配的 `notices/`、`sources/`、Qt SDK 和 patchelf；
输出目录必须不存在。

```sh
python3 -B tools/package_linux.py \
  --source "$PWD" --build "$PWD/build" \
  --qt-prefix /path/to/qt-6.5.3/gcc_64 \
  --notices /path/to/release-inputs/notices \
  --sources /path/to/release-inputs/sources \
  --patchelf /path/to/patchelf \
  --output /path/to/new-release
```

包内应包含 `bin/chat_server`、`bin/chat_qt`、`bin/qt.conf`、
`share/chat/themes/`、`lib/`、`plugins/`、`licenses/`、`README.md`
与 `manifest.json`。不应包含 TUI 程序、FTXUI 或 PostgreSQL 迁移脚本。

目前打包流程在 SQLite/Silo 切换后尚未重新做跨机器发布验收；
**不能将脚本存在或本地编译成功视为发布通过**。应按实际链接结果
核对 S3 SDK/SQLite 等共享库及许可材料，修复缺失的打包依赖。

## 运行

```sh
export CHAT_S3_ENDPOINT='127.0.0.1:9000'
export CHAT_S3_BUCKET='chat-private'
export CHAT_S3_ACCESS_KEY='your-access-key'
export CHAT_S3_SECRET_KEY='your-secret-key'
export CHAT_S3_REGION='us-east-1'
export CHAT_S3_HTTPS=0

chat_prefix=/absolute/path/to/chat
"$chat_prefix/bin/chat_server" 18080 16 "$HOME/chat-data/chat.sqlite3"
"$chat_prefix/bin/chat_qt" ws://127.0.0.1:18080/ws
```

数据库目录必须可写、访问受限。生产环境应使用可靠的 HTTPS S3
端点和 WS/WSS 配置，按具体环境选择证书信任方式。Qt 图形驱动、X11/XCB、
字体和可选 Fcitx5 服务仍由目标系统提供。

旧版本的来源/许可及原始发布审查保留在
[历史审查](release_compliance.md) 和 [Phase 3 历史验收](release_phase3_acceptance.md)。
