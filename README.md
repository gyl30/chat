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

群历史以当前成员最近一次入群时的消息 ID 为界。成员可读取水位之后的消息和附件；
会话预览、置顶消息、回复引用和实时更新遵循同一规则。群公告对所有当前成员可见。

## 大文件附件

单个附件上限为 **4 GiB（4,294,967,296 字节）**。Qt 的“文件/图片”选择原文件，待发送气泡显示进度。
右键气泡或打开主菜单的“附件上传”可暂停、继续或取消。断线和退出会暂停上传。
用原安装目录、同一服务器地址及同一账号登录后，可继续未完成的任务。恢复时 SDK 会检查原文件路径、大小、
修改时间和 SHA-256。下载选择目标位置后，SDK 边接收边写入同目录的临时文件，核对总字节数和 SHA-256 后才替换目标文件。
图片内存预览上限为 10 MiB，更大的图片可以保存原文件。

服务启动时在一个事务中创建当前 SQLite 表、索引和触发器。开发期间结构变更后请使用新的数据库文件。
`attachment_uploads` 和 `attachment_parts` 保留对象预留、
设备绑定及从 1 开始的分片编号、64 位偏移、长度、ETag 和确认时间。
每片 16 MiB，尾片可更小。4 GiB 对应 256 片，10 GiB 对应 640 片；大小策略集中在 SDK 与服务端共享的
`client/include/chat/attachment.hpp`。扩大上限无需重建附件表。

| JSON-RPC 方法 | 作用 |
| --- | --- |
| `authenticate` | SDK 在现有密码认证中附带安装凭证 `device_secret`。服务端将其摘要绑定到已认证会话。 |
| `begin_attachment` | 提交 conversation、client_id、文件名、媒体类型、字节数与 SHA-256，持久化完整布局。重复创建可恢复原任务或取得已经发布的消息。 |
| `confirm_attachment` | 提交 upload、number 和去除外层引号的 ETag。服务端用 ListParts 核对长度与 ETag，再记录确认时间，返回下一片。 |
| `query_attachment` / `pause_attachment` / `resume_attachment` | 查询进度、暂停或恢复。恢复时核对第一个未确认分片，补认已经 PUT 成功的分片，按需签发新地址。 |
| `cancel_attachment` | 放弃任务，Abort Multipart Upload 后清理记录。清理中断时由现有回收流程继续。 |
| `finish_attachment` | 所有分片确认后执行 CompleteMultipartUpload，核对对象大小和 SHA-256 元数据，复用消息发布事务、client_id 去重及通知。 |
| `get_attachment` | 根据 conversation 和 message 校验历史访问权限，返回短期下载 URL、大小和 SHA-256。 |

附件 RPC 和共享 SDK 的文件接口需配套使用。

任务在协议中使用发送者的 client_id 作为 upload 标识。每次只签发当前分片的 PUT 地址，有效期 15 分钟；
返回 number 为 0 时可发布。SDK 直接向 Silo PUT，并提交 S3 返回的 ETag。分片通过 Content-MD5 验证传输，
下载端独立计算完整文件的 SHA-256。凭证和任务记录在客户端的私有应用数据目录内，按服务器地址隔离，
账号分别保存进度；预签名地址只存在于内存。头像仍使用原有的 WebSocket 分块流程和 SQLite BLOB。

S3 endpoint 必须能被服务端和桌面客户端通过同一签名地址访问。跨机器使用时，应配置双方可达的主机名及 HTTPS。
服务端凭据需允许对私有桶执行 multipart 创建、列举上传及分片、完成、终止，以及对象 HEAD、GET 和删除。
客户端只获得限时授权的 URL。撤回或失权后，新的下载授权会被拒绝；已签发 GET 地址有效期最多 120 秒，
对象提前删除也会使地址失效。

上传任务最后活动满 15 天后进入回收。启动和周期回收先终止未完成的 multipart，再交回原有对象清理队列；
已完成但尚未发布的对象仍由预留记录保护。发布成功在同一事务内认领对象、写入消息和附件、移除任务。
消息撤回继续由数据库触发器撤销访问并排队删除对象。Silo 的桶生命周期配置需允许 multipart 至少保留 15 天。
