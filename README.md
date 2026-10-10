# Chat

C++ 聊天服务器、共享客户端 SDK、Qt Widgets 桌面客户端和 FTXUI 终端客户端。
支持好友确认后的私聊、群聊、消息历史、回复/编辑、附件，以及 WS / WSS 连接。

## 构建

需要 CMake 3.30+、支持 C++26 `-freflection` 的 GCC 16、**Boost 1.92.0**
CMake package，以及 OpenSSL、PostgreSQL/libpq、libpng、libjpeg 开发包。
Qt 客户端需要 Qt 6.5+ Widgets/Svg；TUI 使用仓库固定的 FTXUI 子模块。
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

## 验证与依赖声明

[完整验证说明](docs/verification.md) 介绍专用测试数据库、迁移、Qt offscreen
和 normal / ASan / UBSan 全量验证。执行 `tests/verify.sh` 前须准备该环境；
TLS 测试还需要 **OpenSSL 命令行工具**。不要对真实业务数据库运行集成测试。

第三方源码、固定依赖和分发时需保留的原始声明见 [THIRD_PARTY.md](THIRD_PARTY.md)。
当前能力、限制及实际验证范围见 [开发状态](docs/development-status.md)。
