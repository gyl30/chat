# 第三方依赖

此索引记录项目使用的第三方来源与原始许可文件，不指定 Chat 自身的许可证。
构建实际版本以仓库 gitlink、固定下载哈希和本地链接的软件包为准；
本索引不替代各依赖完整的版权和许可文本。

## 仓库子模块

初始化方式：`git submodule update --init --recursive`。
来源地址见 [.gitmodules](.gitmodules)，版本由对应 gitlink 固定。

| 依赖 | 原始许可文件 |
| --- | --- |
| Boost.Capy | [Boost Software License 1.0](third/capy/LICENSE_1_0.txt) |
| Boost.Corosio | [Boost Software License 1.0](third/corosio/LICENSE_1_0.txt) |
| Boost.Http | [Boost Software License 1.0](third/http/LICENSE) |
| wslay | [MIT](third/wslay/COPYING) |
| simdjson | [Apache-2.0](third/simdjson/LICENSE)、[MIT](third/simdjson/LICENSE-MIT)，上游提供双许可 |
| spdlog | [MIT](third/spdlog/LICENSE)；其内含组件的声明也应保留 |
| FTXUI | [MIT](third/ftxui/LICENSE) |

FTXUI 的项目补丁在构建目录应用，未改子模块原始源码；
补丁、版本核验与 Unicode 来源见 [TUI 构建说明](tui/cmake/README.md)。

## 构建下载与 Unicode 数据

| 依赖 | 固定版本与声明 |
| --- | --- |
| utf8proc | 2.12.0 / Unicode 18；[保留的上游许可与数据声明](tui/cmake/utf8proc-LICENSE.md) |
| Google emoji-segmenter | 0.4.0，提交 `72bdc08c02be6cccdfc8cb1055fea4822a6494c1`，Apache-2.0；下载包完整 `LICENSE` 与生成 scanner 版权声明须保留 |
| Unicode 属性数据及派生表 | Unicode 18；[Unicode License V3](tui/cmake/unicode/UNICODE-LICENSE.txt) |
| Unicode 字素测试数据 | Unicode 18；[随测试数据保留的声明](tui/tests/data/UNICODE-LICENSE.txt) |

utf8proc 与 emoji-segmenter 的下载来源、哈希和离线源码核验由
[chat_unicode.cmake](cmake/chat_unicode.cmake) 与
[chat_emoji_scanner.cmake](cmake/chat_emoji_scanner.cmake) 固定。
emoji-segmenter 下载目录中的完整 `LICENSE` 也由 CMake 核验；
详见 [共享依赖说明](cmake/README.md)。
分发包含静态链接客户端或生成 scanner/Unicode 表的产物时，应同时带上这些完整声明。

## 外部软件包

CMake 还链接本机安装的 **Boost 1.92.0、OpenSSL、PostgreSQL/libpq、libpng、libjpeg**，
启用 Qt 客户端时链接 **Qt 6.5+**。它们不是上述子模块或固定下载包；
分发产物时需记录实际软件包版本，并保留所用发行包附带的许可及版权文件。
Qt 的具体模块、构建和许可选择取决于所用 Qt 发行包；本项目不替使用者指定许可选项。
TLS 证书测试额外调用 OpenSSL 命令行程序。

## 可选 Fcitx5 Qt6 输入法插件

Linux 原生输入法集成可单独部署 [fcitx5-qt](https://github.com/fcitx/fcitx5-qt)
的 Qt6 platform input context plugin；它不属于 Chat 源码、子模块或编译依赖，
预编译插件不提交到本仓库。Qt 6.5.3 已验证的来源为 tag `5.0.10`，提交
`814bc3980fa4f1e0451242c723b437459f82f8cb`，`BUILD_ONLY_PLUGIN=ON`。

上游按源文件 SPDX 标记许可；该构建包含 BSD-3-Clause、LGPL-2.1-or-later，
以及通过 Qt6 目录符号链接编入的 Qt5 `platforminputcontext/font.cpp`、`font.h`
中的 GPL-2.0-or-later 声明。不能将整个插件仅标为 BSD 或 BSD/LGPL。
分发前必须核对全部编入组件的版权、适用许可及源代码分发义务，
不能仅凭两份许可文件就认为插件的分发条件已经闭合。
Phase 3 可搬运发布包暂不捆绑该可选插件；原生 Fcitx5 分发及验收另行确认。
plugin-only 构建静态整合上游 DBusAddons，声明也覆盖这些编入组件。
Fcitx5 daemon 是系统提供的独立运行服务，并未被 Chat 安装或打包。
Qt 自身和动态运行库的许可义务仍按上文所用发行包处理。
