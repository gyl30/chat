# Chat

C++ 聊天服务器、共享客户端 SDK 和 Qt Widgets 桌面客户端。
支持好友确认后的私聊、群聊、消息历史、回复/编辑、附件，以及 WS / WSS 连接。

## 构建

需要 Git、CMake 3.30+、Ninja 1.11+、支持 C++26 `-freflection` 的 GCC 16 和 **Boost 1.92.0**。
以 Debian/Ubuntu 系为例，配置上述工具链和 Boost 后，系统开发包为：

```sh
sudo apt install pkg-config libssl-dev zlib1g-dev libpng-dev libjpeg-dev \
    nlohmann-json3-dev libpugixml-dev
```

Qt 客户端另需 Qt 6.5+ Widgets/Svg（通常为 `qt6-base-dev qt6-svg-dev`，发行版版本不足时安装满足版本的 Qt SDK），Linux 另需 `libdbus-1-dev` 及 Qt 本身要求的图形开发依赖。可选依赖包括 Brotli/Zstd 和 liburing 2.5+；它们不阻止缺省构建。

```sh
git clone --recurse-submodules https://github.com/gyl30/chat.git
cd chat
# 已有检出则运行：git submodule update --init --recursive
export CC=gcc-16 CXX=g++-16
cmake -S . -B build-server -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBoost_DIR=/usr/local/lib/cmake/Boost-1.92.0
cmake --build build-server --target chat_server chat_client

cmake -S . -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBoost_DIR=/usr/local/lib/cmake/Boost-1.92.0 \
    -DCHAT_BUILD_QT_CLIENT=ON -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build-qt --target chat_server chat_client chat_qt
```

按实际安装路径调整 Boost、Qt；在默认搜索路径中时可省略对应参数。不需要安装 SQLite 开发库或 miniocpp SDK，也不需要指定它们的 CMake 路径。[SQLite 3.51.3](third/sqlite/README.md) 静态链接，miniocpp v1.0.1 由子模块构建。C++ 构建不需要 Python。首次配置的下载、重复配置的联网行为见[依赖说明](cmake/README.md#minio-c-sdk)。管理 SQLite 的 Python 需[单独使用同源私有库](ops/sqlite-runtime.md)。隔离验收和搜索测量见[验收入口](ops/acceptance/README.md)。

## 本地运行

直接使用构建目录中的 `chat_server` 和 `qt/chat_qt`。Qt 主题文件由 CMake
复制至对应构建目录的 `qt/themes/`，无需安装或打包。Silo/S3 服务需独立启动。

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

右键会话可清空个人记录或隐藏会话，两项操作同步到本账号的在线设备。清空前从服务端取得会话尾部，确认和重试使用同一截止消息。个人清空水位只升不降，实际已读位置保持原值。草稿、待发送文本和上传任务继续保留。
`clear_history` 请求携带会话 ID `conversation` 和固定截止消息 ID `through`。响应和本人设备的 `conversation_view` 清空事件均返回实际生效的 `through`。
历史、搜索、会话预览、群置顶、回复引用、实时更新和消息附件下载都以个人清空水位和最近入群水位中较大者为界。其他成员仍可读取自己的历史，群公告和群文件继续独立可见。
隐藏会话保留历史、草稿和置顶设置。自己、对方或群成员发送新消息后，它会再次出现在列表中。

联系人窗口可管理自定义分组，右键好友可移动分组或编辑最多 32 个 Unicode 字符的私人备注。删除自定义分组会把其中的好友移回「我的好友」。分组与备注同步到本账号各设备，好友的登录名和对方的联系人资料保持独立。

右键消息可单条转发或进入多选。多选时可用鼠标、Shift 和 Ctrl 选择，Ctrl+C 按历史顺序复制，Esc 退出。
转发在目标会话逐条生成新消息，保留原作者；文件沿用同一个对象。源消息撤回后已转发的副本独立存在。服务端在每次转发时同时校验源消息可见性和目标发送权限。

右键会话可设置全部通知、仅提及或免打扰。仅提及时，群消息须实际 @本人；私聊直接发给本人。免打扰可选 1 小时、8 小时或永久。每台在线设备遵循同一账号设置，并独立判断当前是否正在阅读；自己发送的消息和设置、已读等状态事件不会触发桌面消息通知。
「标记未读」显示个人圆点。它独立于真实未读条数和已读回执，再次打开会话即可清除。个人清空仅移除截止消息及以前的未读标记。

## 全局消息搜索

侧栏主菜单的「全局消息搜索」按正文查找当前账号可读的历史消息，附件可按文件名查找。查询保留空格和标点，
按项目的 Unicode 小写规则匹配连续子串。点击结果会打开只读的聊天上下文并选中原消息，保留正在编辑的聊天和已读位置。

`search_all_messages` 接受 `query`（1–1024 UTF-8 字节）和可选的 `before`，按消息 ID 倒序返回最多 50 条。
有下一页时，`next_before` 为本页最后一条消息的 ID。每条命中包含会话和消息 ID、会话名、发送者 ID 和名称、时间及最多 160 个码点的原文摘要。
搜索支持 `conversation`（当前有权读取的会话）、`category`（`all/images/files/links`）、`sender`（用户 ID）、`start/end`（Unix 毫秒，含起点、不含终点）。可省略 `query`，但至少提供一项有效筛选；显式空字符串无效，空格按原文匹配。Qt 日期按系统本地时区解释，包含所选结束日。筛选组合须显式提交。`message_search_scopes` 列出包括隐藏和已不在侧栏的有权会话。`get_messages` 加 `category` 时使用历史窗口的语义：排除已撤回消息，先筛选后分页，支持与 `query` 组合，不影响普通时间线和 `around`。
`get_messages` 的 `around` 与 `before`、`after` 互斥，按消息 ID 升序返回可见锚点和前后各最多 20 条记录；
此时 `has_more` 表示前面还有更早记录。锚点撤回或权限失效时返回会话不可用。已删除好友的历史私聊仍可搜索和定位。

## 普通文本消息的发送与恢复

发送文本或回复时，SDK 先在本机 SQLite 的 `text_outbox` 中提交消息。未发送输入先作为本机草稿按服务器、账号和会话保存。草稿只存文本与回复 ID，恢复时重新检查可见性。正式入队用同一 SQLite 事务只消费本次草稿版本；Qt 收到本地保存成功的回调后才清空对应编辑框；
写入失败会保留输入和回复目标，保存期间新输入的文字也会保留。已登录账号断线后，仍可在已有会话中排队发送文字。

待发送记录按服务器地址和账号隔离。重新登录后，SDK 按入队顺序逐条发送同一会话的消息，其他会话可独立发送。
每次重发均沿用原始 `client_id`、正文和回复目标；服务端按发送者和 `client_id` 去重。SDK 校验确认消息后删除本地记录。若重试时本人仍是会话成员，但原消息已在可见范围之外，服务端只返回 `hidden: true` 和 `id`、`conversation`、`from`、`client_id`，Qt 据此结束任务。
服务器明确返回错误时，气泡显示失败，可右键重试或删除本地记录。已失去访问权限的会话可在主菜单中处理待发送记录。
删除本地记录只停止后续重发；如果服务端已经收到消息，历史消息仍然保留。卸载或清除本地应用数据会移除本机队列。

## 大文件附件

单个附件上限为 **4 GiB（4,294,967,296 字节）**。Qt 可多选文件、将本地文件拖入消息区或输入区，也可在输入框按 Ctrl+V 粘贴图片。
输入栏的截图按钮支持预览整个桌面或拖动选区，点击「发送截图」后走相同的 PNG 上传与清理流程。Linux 在 X11 中读取桌面，在 Wayland 会话中由 XDG Desktop Portal 授权取图。设置中的浅色、深色和跟随系统可在聊天过程中直接切换。
每个文件单独显示上传进度，同一批附件沿用当前回复目标。剪贴板图片以 PNG 保存在应用数据目录中，暂停和重启后可继续上传，发送成功或取消后清理。
右键气泡或打开主菜单的“附件上传”可暂停、继续或取消。断线和退出会暂停上传。
用原安装目录、同一服务器地址及同一账号登录后，可继续未完成的任务。恢复时 SDK 会检查原文件路径、大小、
修改时间和 SHA-256。下载选择目标位置后，SDK 边接收边写入同目录的临时文件，核对总字节数和 SHA-256 后才替换目标文件。
图片内存预览上限为 10 MiB，更大的图片可以保存原文件。主菜单的“下载任务”同时显示消息附件和群文件下载，可单项取消、失败后重新申请授权并从头重试。已完成项可打开文件或所在目录；不提供暂停、续传或重启恢复。

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
部署时请核对 Silo 的服务级 `api` 配置。版本 `44e848fedf8c` 的 `stale_uploads_expiry` 默认 24h，清扫周期 `stale_uploads_cleanup_interval` 默认 6h。Chat 从最后活动计时，Silo 从上传创建计时，旧格式元数据另有时间回退。即使设置为 360h，第 14 天恢复的上传仍可能提前失去分片。应按允许的上传总寿命（含最后一次活动后的 15 天）核对有效配置；持续活动且总寿命无上限时，任何有限过期值都无法保证覆盖。当前生产实例版本与生效值尚未核实。详见[只读核验步骤](ops/README.md#silo-核验与回收)。

## 群管理：禁言与解散

群资料 → 成员：群主与管理员可右键限制普通成员，支持限时、永久和提前解除。管理页可开启全员禁言，
只限制普通成员；关闭后个人禁言不变。服务器按当前角色及服务器时间裁决。退群或被移除后重新加入，
不会解除尚未到期的个人禁言；禁言不是个人的通知免打扰。

禁言限制发送、回复、转发、编辑、添加或更换表态，以及发布附件或新增群文件引用。历史、搜索、下载、撤回本人消息、
取消自己的表态、移除本人群文件及个人清空不受额外限制。草稿保留，待发送被拒绝时保留原正文与 `client_id`，
解禁后手动重试；已经入库的同一笔发送仍可对账。上传暂停后只能由原设备继续，保留期仍按最后活动计 15 天。

只有群主可在群资料 → 管理 → 解散群，经确认后执行。所有成员立即失去群历史、群资料与群文件访问；
原成员的在线设备收到失效事件，重连按当前服务端状态收敛。本地草稿和未送达的消息在主菜单「不可访问会话的本地内容」
中可复制或删除，失效回复不再显示。解散只释放本群的引用，不删其他会话的转发、副本引用、已保存文件或整个月档。
未完成分片按持久回收记录终止，即使服务重启也能继续。已签发下载地址仍按原有效期处理。

| JSON-RPC 方法 | 参数与结果 |
| --- | --- |
| `set_group_member_mute` | `conversation`、`user`、`seconds`。正数为服务器计时的秒数（最多一年），`-1` 永久，`0` 解除。 |
| `set_group_mute` | `conversation`、`muted`。只切换全员设置。 |
| `dissolve_group` | `conversation`。首次成功返回 `changed:true`，同一原群主重试返回 `changed:false`。 |

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

## 月度消息归档

在服务端主机经 SQLite 包装器执行 `ops/with-sqlite-runtime.sh "$SQLITE_PREFIX" python3 ops/chat_admin.py --db /srv/chat/chat.sqlite3 archive 2025-01`，归档一个已结束的 UTC 月份。
每个月档位于数据库旁的 `chat.sqlite3.archives/`，由同一服务账号读写。工具先复制消息快照，校验行内容、SQLite 完整性及 SHA-256，再分批将热库中仍与快照一致的正文切换到月档。收发消息可以继续。重复执行同一月份会校验已有文件，失败任务可用同一命令继续。
`archive-status` 列出各月状态、ID 范围、校验值和最近错误，`archive-check` 重新检查归档。请将数据库和月档目录一起备份。

消息 ID、会话成员、个人水位、撤回状态和附件引用留在热库。归档后编辑的正文使用热库新版本。历史、全局搜索、引用、置顶与定位使用相同读取路径。原始快照仅供服务器和管理员访问，包含归档时的正文。搜索仍按可见消息逐条匹配，耗时随可读历史增长。

部署、在线备份、恢复、WAL 检查、Silo 核验与日志轮转见[运行手册](ops/README.md)。
