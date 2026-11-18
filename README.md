# Chat

C++ 聊天服务器、共享客户端 SDK 和 Qt Widgets 桌面客户端。
支持好友确认后的私聊、群聊、消息历史、回复/编辑、附件，以及 WS / WSS 连接。

## 构建

需要 CMake 3.30+、支持 C++26 `-freflection` 的 GCC 16、Boost 1.92.0、
SQLite3、MinIO C++ SDK 1.0.1（`miniocpp` CMake 包）、OpenSSL、libpng、libjpeg。
Qt 客户端需要 Qt 6.5+ Widgets/Svg（Linux 还需要 DBus）。

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCHAT_BUILD_QT_CLIENT=ON
cmake --build build -j12
```

默认只构建生产程序及其依赖，不构建或运行任何第一方测试目标。
离线依赖配置参见 [共享 Unicode 依赖](cmake/README.md)。

## 安装与独立运行

```sh
cmake --install build --prefix "$HOME/.local/chat"
```

安装包括 `chat_server`、启用的 `chat_qt` 和 Qt 主题；
SQLite 数据库由服务端首次打开时初始化。附件存储使用独立的 Silo/S3 服务。
标准 CMake install 不会自动打包外部运行时依赖，也不部署 Silo。

跨机器可搬运打包工具见 `tools/package_linux.py`；其 Qt/GCC 固定版本及依赖要求
仍需依照实际构建环境核对。无需 PostgreSQL、libpq 或旧 SQL 迁移文件。

### Linux 原生 Fcitx5 Qt6 插件

Fcitx5 输入法是可选运行集成，不是 Chat 编译依赖。Qt5 插件不能给 Qt6 使用。
`QT_IM_MODULE=fcitx` 需要匹配当前 Qt SDK 的
`libfcitx5platforminputcontextplugin.so`；标准 Chat install 不会从开发机自动搜集它。
有已经验证、匹配此 Qt 版本的插件时，单独部署到可执行程序旁：

```sh
install -Dm755 /path/to/verified/libfcitx5platforminputcontextplugin.so \
    "$chat_prefix/bin/platforminputcontexts/libfcitx5platforminputcontextplugin.so"
QT_IM_MODULE=fcitx "$chat_prefix/bin/chat_qt" ws://127.0.0.1:18080/ws
```

无需设置 `QT_PLUGIN_PATH`，不覆盖系统 Qt5 插件或修改 SDK。用于本机 Qt 6.5.3 的
已验证构建来自上游 [fcitx5-qt 5.0.10](https://github.com/fcitx/fcitx5-qt/tree/5.0.10)，
提交 `814bc3980fa4f1e0451242c723b437459f82f8cb`。重新构建的最小流程：

```sh
# 在 Chat 工作区外，qt_prefix 指向实际 Qt SDK。
qt_prefix=/path/to/qt-sdk
plugin_source=/path/to/fcitx5-qt
plugin_build=/path/to/fcitx5-qt-build
# 获取官方 tag 5.0.10；需 ECM、XCB/XKBCommon/X11 开发依赖及此 SDK 的 GuiPrivate。
cmake -S "$plugin_source" -B "$plugin_build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$qt_prefix" \
    -DENABLE_QT4=OFF -DENABLE_QT5=OFF -DENABLE_QT6=ON \
    -DBUILD_ONLY_PLUGIN=ON \
    -DCMAKE_INSTALL_PREFIX="$chat_prefix" \
    -DCMAKE_INSTALL_QT6PLUGINDIR="$chat_prefix/bin"
cmake --build "$plugin_build" -j4
cmake --install "$plugin_build"
```

ECM 可来自已安装开发包或工作区外的原包解压目录（用 `ECM_DIR` 指定）；不必安装整套
系统 Qt。此模式静态整合上游 DBusAddons，无需额外复制 Fcitx Qt 运行库。插件使用 Qt
私有 API，换 Qt minor 版本后必须重新核对兼容性。分发插件时同时保留上游完整许可与
版权声明，见 [THIRD_PARTY.md](THIRD_PARTY.md)。

验证时使用真实 Fcitx5 daemon 和图形会话：`QT_DEBUG_PLUGINS=1 QT_IM_MODULE=fcitx`
日志必须显示该原生 `.so` 实际加载，并通过真实拼音候选和发送测试；只发现 metadata、
加载 Compose 或改为 IBus 不算原生 Fcitx 验收。

## SQLite 与 Silo 启动

服务端命令行参数为 `<port> <max-workers> <sqlite-file>`，数据库由服务端打开，
并通过文件锁防止同一个数据库文件被两个服务进程并发使用。

设置运行环境（请使用独立、私有的 S3 桶和受保护的凭据）：

```sh
export CHAT_S3_ENDPOINT='127.0.0.1:9000'
export CHAT_S3_BUCKET='chat-private'
export CHAT_S3_ACCESS_KEY='your-access-key'
export CHAT_S3_SECRET_KEY='your-secret-key'
export CHAT_S3_REGION='us-east-1'
export CHAT_S3_HTTPS=0  # 本地 HTTP；生产环境应使用 HTTPS

mkdir -p "$HOME/chat-data"
./build/chat_server 18080 32 "$HOME/chat-data/chat.sqlite3"
./build/qt/chat_qt ws://127.0.0.1:18080/ws
```

SQLite 使用 WAL、`synchronous=FULL` 和外键约束。部署时确保数据库目录可写，
数据库文件及其 `-wal`、`-shm` 文件均得到适当保护；备份应在数据库一致性边界内进行。
附件对象为私有 key，S3 写入后由 SQLite 事务发布元数据。服务启动与正常退出会恢复可清理对象。

公网或不受信网络可启用 TLS，使用成对的证书和私钥：

```sh
./build/chat_server 18443 32 "$HOME/chat-data/chat.sqlite3" \
    --tls-cert /path/fullchain.pem --tls-key /path/key.pem
```

`GET /health` 提供 HTTP 健康检查；它不代表 S3 服务一定可用。

## WebSocket 协议约束

WS / WSS 的完整客户端 JSON-RPC 请求上限均为 **65,536 字节（64 KiB）**。
按实际序列化后的 UTF-8 字节数计算，包含 JSON-RPC envelope、`method`、`params`、
request `id` 及 JSON 转义产生的内容；不是消息正文的字符数上限。
共享 C++ SDK 会在本地拒绝超限请求并返回可读错误，保留现有连接。
直接使用原始 WebSocket 的第三方客户端必须自行遵守上限；超过服务端接收上限可能
结束连接，不保证收到 JSON-RPC 错误响应。

客户端必须正确实现 WebSocket Ping/Pong：收到 Ping 后返回携带**相同 payload** 的 Pong。
当前服务端在 WebSocket Upgrade 后使用 **30 秒探测周期、10 秒响应宽限**；
不匹配的 Pong、普通业务消息和其他数据不能代替心跳确认。
失活连接会被结束并正常释放对应在线会话。WS / WSS 行为相同；这些是当前固定行为，
没有新增心跳配置参数，也不改变严格单账号在线规则。

## 依赖与分发声明

第三方依赖、固定源码和分发注意事项见 [THIRD_PARTY.md](THIRD_PARTY.md)。
历史发布审查文档涉及 PostgreSQL 的内容仅供归档，不适用于当前 SQLite/Silo 服务端。
