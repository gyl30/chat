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

## 本地运行

直接使用构建目录中的 `chat_server` 和 `qt/chat_qt`。Qt 主题文件由 CMake
复制至 `build/qt/themes/`，无需安装或打包。Silo/S3 服务需独立启动。

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
附件对象为私有 key，S3 写入后由 SQLite 事务发布元数据。撤回事务立即撤销附件访问并记录待删对象，
由服务启动、每五分钟的回收及正常退出时清理。

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
