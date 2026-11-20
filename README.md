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
附件对象为私有 key，S3 写入后由 SQLite 事务发布元数据。撤回消息会立即解除该条消息的附件引用。最后一条引用消失后，服务在启动、每五分钟的回收及正常退出时清理对象。

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
失活连接会被结束并释放。一个账号可以在多台设备上同时在线。第一条连接登录时上线，最后一条连接退出时离线；消息和关系变更同步至其他在线设备。

右键会话可清空个人记录或隐藏会话，两项操作同步到本账号的在线设备。确认清空后，服务端将个人清空水位和已读位置推进到当前会话末尾。
历史、搜索、会话预览、群置顶、回复引用、实时更新和消息附件下载都以个人清空水位和最近入群水位中较大者为界。其他成员仍可读取自己的历史，群公告和群文件继续独立可见。
隐藏会话保留历史、草稿和置顶设置。自己、对方或群成员发送新消息后，它会再次出现在列表中。

联系人窗口可管理自定义分组，右键好友可移动分组或编辑最多 32 个 Unicode 字符的私人备注。删除自定义分组会把其中的好友移回「我的好友」。分组与备注同步到本账号各设备，好友的登录名和对方的联系人资料保持独立。

右键消息可单条转发或进入多选。多选时可用鼠标、Shift 和 Ctrl 选择，Ctrl+C 按历史顺序复制，Esc 退出。
转发在目标会话逐条生成新消息，保留原作者；文件沿用同一个对象。源消息撤回后已转发的副本独立存在。服务端在每次转发时同时校验源消息可见性和目标发送权限。

## 全局消息搜索

侧栏主菜单的「全局消息搜索」按正文查找当前账号可读的历史消息，附件可按文件名查找。查询保留空格和标点，
按项目的 Unicode 小写规则匹配连续子串。点击结果会打开只读的聊天上下文并选中原消息，保留正在编辑的聊天和已读位置。

`search_all_messages` 接受 `query`（1–1024 UTF-8 字节）和可选的 `before`，按消息 ID 倒序返回最多 50 条。
有下一页时，`next_before` 为本页最后一条消息的 ID。每条命中包含会话和消息 ID、会话名、发送者 ID 和名称、时间及最多 160 个码点的原文摘要。
`get_messages` 的 `around` 与 `before`、`after` 互斥，按消息 ID 升序返回可见锚点和前后各最多 20 条记录；
此时 `has_more` 表示前面还有更早记录。锚点撤回或权限失效时返回会话不可用。已删除好友的历史私聊仍可搜索和定位。

## 普通文本消息的发送与恢复

发送文本或回复时，SDK 先在本机 SQLite 的 `text_outbox` 中提交消息。Qt 收到本地保存成功的回调后才清空对应草稿；
写入失败会保留输入和回复目标，保存期间新输入的文字也会保留。已登录账号断线后，仍可在已有会话中排队发送文字。

待发送记录按服务器地址和账号隔离。重新登录后，SDK 按入队顺序逐条发送同一会话的消息，其他会话可独立发送。
每次重发均沿用原始 `client_id`、正文和回复目标；服务端按发送者和 `client_id` 去重。SDK 校验确认消息后删除本地记录。
服务器明确返回错误时，气泡显示失败，可右键重试或删除本地记录。已失去访问权限的会话可在主菜单中处理待发送记录。
删除本地记录只停止后续重发；如果服务端已经收到消息，历史消息仍然保留。卸载或清除本地应用数据会移除本机队列。

## 大文件附件

单个附件上限为 **4 GiB（4,294,967,296 字节）**。Qt 可多选文件、将本地文件拖入消息区或输入区，也可在输入框按 Ctrl+V 粘贴图片。
每个文件单独显示上传进度，同一批附件沿用当前回复目标。剪贴板图片以 PNG 保存在应用数据目录中，暂停和重启后可继续上传，发送成功或取消后清理。
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
独立完成上传的对象从登记时起继续预留 15 天。首次消息或群文件引用在同一事务内认领对象。
Silo 的桶生命周期配置需允许 multipart 至少保留 15 天。

## 群文件

在群资料的「群文件」页，可以只上传到群文件，也可以把文件发到聊天。右键自己发过的附件，选择「加入群文件」可将同一对象加入其他群，无需再次上传。成员可按文件名和类别查找、翻页、下载，或保存后打开。群文件上传沿用附件菜单的暂停、继续和取消操作。

SQLite 的 `files` 保存文件元数据，每个对象对应一个私有 S3 key。`message_attachments` 和 `conversation_files` 分别保存消息引用和群文件引用。后加入的成员可以下载此前的群文件，普通消息仍受入群水位限制。上传者可以移除自己的群文件，群主和管理员可移除本群的其他文件。移除群文件、撤回消息或删除群只解除相应引用。最后一条引用消失后才排队回收对象。

| JSON-RPC 方法 | 作用 |
| --- | --- |
| `complete_file` | 完成分片上传并登记文件对象，返回 `object_key`。原设备可用同一上传标识重取结果。 |
| `add_conversation_file` | 当前有发言权限的成员可将自己已完成的文件加入群。重复添加同一对象只保留一条引用。 |
| `list_conversation_files` | 当前群成员可按名称和类别查询，使用加入时间与对象 key 倒序分页。默认 50 条，最多 100 条。 |
| `get_conversation_file` | 根据当前成员身份和有效群文件引用签发短期下载地址。SDK 流式下载并校验 SHA-256。 |
| `remove_conversation_file` | 验证上传者或管理角色并解除本群引用。 |
| `send_message` | 发送者也可以使用自己的 `object_key` 发布新的附件消息，沿用 `client_id` 去重。 |
