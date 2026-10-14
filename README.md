# Chat

C++ 聊天服务器、共享客户端 SDK、Qt Widgets 桌面客户端和 FTXUI 终端客户端。
支持好友确认后的私聊、群聊、消息历史、回复/编辑、附件，以及 WS / WSS 连接。

## 构建

需要 CMake 3.30+、支持 C++26 `-freflection` 的 GCC 16、**Boost 1.92.0**
CMake package，以及 OpenSSL、PostgreSQL/libpq、libpng、libjpeg 开发包。
Qt 客户端需要 Qt 6.5+ Widgets/Svg（Linux 还需要 DBus）；TUI 使用仓库固定的 FTXUI 子模块。
Qt/TUI 构建还会下载固定版本的 Unicode 依赖，Qt 另下载 emoji-segmenter；
离线配置见 [共享 Unicode 依赖](cmake/README.md)。

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCHAT_BUILD_QT_CLIENT=ON -DCHAT_BUILD_TUI_CLIENT=ON
cmake --build build -j12
```

首次配置可用 `CC` / `CXX` 选择编译器，用 `CMAKE_PREFIX_PATH` 指定非默认
Boost/Qt 安装目录。当前经过验证的具体工具链见 [完整验证说明](docs/verification.md)。

## 安装与独立运行

标准安装包含已启用的客户端、服务端、SQL 迁移和 Qt 主题：

```sh
cmake --build build -j4
chat_prefix="$HOME/.local/chat"
cmake --install build --prefix "$chat_prefix"
cd /tmp
"$chat_prefix/bin/chat_server" 18080 32 8
# 在其他终端启动客户端：
"$chat_prefix/bin/chat_tui" ws://127.0.0.1:18080/ws
"$chat_prefix/bin/chat_qt" ws://127.0.0.1:18080/ws
```

默认目录布局为 `bin/chat_server`、`bin/chat_tui`、`bin/chat_qt`、
`share/chat/sql/001_*.sql` 至 `027_*.sql`，以及
`share/chat/themes/{common.qss,daylight.qss,daylight.json,midnight.qss,midnight.json}`。
Qt/TUI 关闭构建时不安装对应客户端。Qt 从可执行程序位置相对查找主题，SVG 图标编入
程序，不需要源码、构建目录或特定工作目录。用户主题覆盖仍在 Qt 用户配置目录下。
安装不创建数据库、不执行迁移；启动前仍需按下一节准备数据库和 libpq 连接配置。

这是**依赖宿主运行库的安装**，不是自包含包。运行库包括匹配的 GCC 16 libstdc++、
Boost 1.92.0、OpenSSL，以及服务器使用的 libpq/libpng/libjpeg；Qt 客户端还需要与
构建版本兼容的 Qt Core/Gui/Widgets/Svg/DBus、XCB 平台插件和其图形系统依赖。
Qt SDK 只提供库和插件时，不要求运行机器重新安装编译器或开发头文件。
系统有较旧 Qt 库并不代表满足这些要求。

安装目标保留 CMake 发现的外部链接目录作为 RUNPATH；不引用项目源码/构建产物。
使用非系统 Qt SDK 构建时，它的运行库和平台插件仍是外部依赖，该 SDK 路径需要继续
存在。整体移动安装前缀时，主题随前缀移动，无需修改路径；外部运行库不会自动
打包或重定位。
如更换运行库位置，按新依赖位置重新配置并安装；部署者也可使用标准
`CMAKE_INSTALL_RPATH` 指定优先路径，自动保留的外部链接目录仍会追加。不要用
指向构建产物的 `LD_LIBRARY_PATH` 掩盖缺少依赖。Linux 可用 `readelf -d`、`ldd`、`ldd -r` 核对每个安装程序。

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

## 数据库与启动

服务端和数据库测试使用 libpq 的连接配置，包括 `PGHOST` / `PGHOSTADDR`、
`PGPORT`、`PGDATABASE`、`PGUSER` 和 `.pgpass` / `PGPASSWORD`。
先建立应用数据库并配置连接；不要把密码写入命令参数或仓库文件。
仅对**新建的空数据库**依序应用迁移：

```sh
(
    set -e
    for migration in sql/[0-9][0-9][0-9]_*.sql; do
        psql -v ON_ERROR_STOP=1 -f "$migration"
    done
)
```

既有数据库只执行尚未应用的迁移。当前迁移编号为 001–027；服务器不会自动迁移。

安装版使用 `"$chat_prefix"/share/chat/sql/[0-9][0-9][0-9]_*.sql` 作为上述循环的输入，
无需源码目录。执行前核对 libpq 的目标地址、端口、数据库和角色；任何脚本失败立即停止。
迁移是人工部署步骤，安装和服务端启动都不会自动应用 SQL。

### 升级、备份与恢复

升级前停止连接此数据库的 Chat 服务端，记录已应用的迁移编号并先备份。
例如已有 025 数据库，只按顺序执行 `026_create_friend_requests.sql` 和
`027_short_group_invite_codes.sql`，每条命令都使用 `psql -v ON_ERROR_STOP=1`；
当前没有自动迁移历史表，已应用编号须由部署记录确认，不能靠重放脚本探测。
不要对已有库重放 001–025。026 保留双向联系人，将单向联系人转为待确认申请；
027 撤销旧群邀请码，升级后由群主或管理员重新生成。其他历史会话与数据应保留。

使用与服务端版本兼容的 PostgreSQL 官方工具。以下 `$PGDATABASE` 必须明确指向
要备份的应用库，`chat_restored` 是新建的独立空恢复库；部署者须核对名称和所有权：

```sh
(
    set -eu
    : "${PGDATABASE:?请先核对并设置待备份数据库}"
    umask 077
    backup_dir=$(mktemp -d "$HOME/chat-backup.XXXXXXXX")
    pg_dump --format=custom --file="$backup_dir/chat.dump" "$PGDATABASE"
    createdb --template=template0 chat_restored
    pg_restore --single-transaction --exit-on-error --no-owner --no-privileges \
        --dbname=chat_restored "$backup_dir/chat.dump"
)
```

备份写入新建的私有目录，含账号密码哈希、消息和附件，应限制读取权限并安全保管。
`--no-owner --no-privileges` 不恢复原所有者和授权，对象归执行恢复的角色所有；
须选用预期的应用角色。
恢复后核对关键 ID、关系、已读位置以及附件字节，再将服务端连接配置指向恢复库启动验证。
同版本恢复包含已有 schema 与数据，**不能再执行全部迁移**；恢复历史版本备份时只在
恢复完成后补尚未应用的迁移。不使用 `--clean` 覆盖现有数据库；业务库的覆盖恢复应另行规划。

```sh
# 三个位置参数依次为端口、连接处理单元数、数据库连接数。
./build/chat_server 18080 32 8
./build/qt/chat_qt ws://127.0.0.1:18080/ws
./build/chat_tui ws://127.0.0.1:18080/ws
```

在客户端登录页注册账号或输入已有账号、密码；TUI 登录后按 `F1` 查看键盘帮助，按 `Ctrl+K` 打开命令面板。
未传客户端 URL 时优先恢复最近成功登录的服务器，否则使用本机 18080；密码不保存。

公网或不受信网络连接可使用 TLS。服务器接受成对的 PEM 完整证书链和未加密私钥，
链中首张证书须为服务端证书，证书 SAN 应包含客户端实际访问的域名或 IP：

```sh
./build/chat_server 18443 32 8 --tls-cert /path/fullchain.pem --tls-key /path/key.pem
./build/qt/chat_qt wss://chat.example:18443/ws
./build/chat_tui wss://chat.example:18443/ws
```

该端口只提供 HTTPS / WSS；无 TLS 参数时提供 HTTP / WS。
客户端验证证书链和主机名，不提供跳过验证或回退明文的选项。
私有 CA 可通过 `SSL_CERT_FILE` / `SSL_CERT_DIR` 指定信任库。
证书更新后需要重启服务器加载；证书签发和续期由部署者管理。
`GET /health` 是健康检查入口。

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

## 验证与依赖声明

[完整验证说明](docs/verification.md) 介绍专用测试数据库、迁移、Qt offscreen
和 normal / ASan / UBSan 全量验证。执行 `tests/verify.sh` 前须准备该环境；
TLS 测试还需要 **OpenSSL 命令行工具**。不要对真实业务数据库运行集成测试。

第三方源码、固定依赖和分发时需保留的原始声明见 [THIRD_PARTY.md](THIRD_PARTY.md)。
当前能力、限制及实际验证范围见 [开发状态](docs/development-status.md)。
