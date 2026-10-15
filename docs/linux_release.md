# Linux 可搬运发布包

此流程在标准 `cmake --install` 之上收集私有运行库，并重定位 ELF 和 Qt 插件路径。
它不是新的业务安装器，不创建数据库、不执行迁移、不安装系统软件。
验收范围是 Ubuntu 22.04.5 / x86_64 / glibc 2.35；不能据此声称所有 Linux 都兼容。

## 构建和许可输入

先按 [构建说明](../README.md#构建) 启用 Server、Qt、TUI 并完成
[Normal/ASan/UBSan 验证](verification.md)。发布脚本只接受干净、已提交的源码；
核对构建缓存所属仓库，并刷新构建后才赋予发布包 Git 版本。构建机需要 Python 3.10+、
CMake、Git、GNU tar/gzip、ldd、readelf 和 patchelf；目标机不需要开发 SDK。

本轮实际使用 GCC **16.2.0**、Boost **1.92.0** 和 Qt **6.5.3**。
`/usr/bin/g++-16` 的另一个 experimental 版本不是本轮工具链；不要仅凭名字替换编译器。

在仓库外准备以下输入，不能把预编译库或大归档提交到 Git：

- `notices/`：完整第三方版权和许可文本、Qt 第三方 attribution、ICU 56.1 原始许可、
  GCC COPYING3/COPYING.RUNTIME 和实际构建来源、Boost 和静态链接依赖的许可。
  私有 Ubuntu 库使用对应安装包的 copyright 及其引用的完整许可文本。
  `README.md` 必须说明来源和分发边界；不能只放上游链接。
- `sources/`：本工具当前对应的三个完整源码归档：
  `qtbase-everywhere-src-6.5.3.tar.xz`、`qtsvg-everywhere-src-6.5.3.tar.xz`、
  `gcc-16.2.0.tar.xz`。它们一起放入发布包 `licenses/sources/`。
- 核对实际 Qt SDK 的版本、来源和许可证。后续[来源与许可工程审查](release_compliance.md)
  已取得本包所用 SDK 中 7 个 Qt 库、9 个插件、3 个 ICU 库与官方 6.5.3 归档
  逐字节对应证据；Qt 打包后的 RPATH 重定位另行核对。
  SDK 元数据变更符合官方开源安装脚本。完整厂商源码/补丁/构建关联及分发材料充分性
  仍待确认，不能仅凭 edition、版本号或运行通过宣称许可闭合；不要求普遍的字节级重建。

本轮源码归档 SHA-256：

| 归档 | SHA-256 |
| --- | --- |
| qtbase 6.5.3 | `df2f4a230be4ea04f9798f2c19ab1413a3b8ec6a80bef359f50284235307b546` |
| qtsvg 6.5.3 | `fc41c47b69ca1f80473cd4b11996394155781105345392961d064d04f95d5bef` |
| gcc 16.2.0 | `e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e` |

## 打包

以下输入路径由部署者替换，输出目录必须尚不存在：

```sh
python3 -B tools/package_linux.py \
  --source "$PWD" --build "$PWD/build" \
  --qt-prefix /path/to/qt-6.5.3/gcc_64 \
  --notices /path/to/release-inputs/notices \
  --sources /path/to/release-inputs/sources \
  --patchelf /path/to/patchelf \
  --output /path/to/new-release
python3 -B tests/package_linux_test.py -v
```

脚本只针对当前已验证的依赖集合，不是任意程序通用依赖打包器。
构建前要求 `qmake -query QT_VERSION` 为 **6.5.3**，并校验上表三份源码归档的
固定 SHA-256；不同版本或归档内容不一致时停止，不仅检查文件存在。
源码 `sql/` 是迁移集合的权威来源：编号必须连续为 001–028、无重复；安装的
`share/chat/sql/` 必须具有完全相同的文件名集合和逐文件 SHA-256，不能只数 28 个文件。
它还检查 Git 状态、私有库冲突和全部私有 ELF 的 `ldd -r`。两个依赖检查阶段
共用解析器，保留绝对路径中的空格；无法解析的依赖记录明确失败。vdso、动态加载器和
无动态依赖 ELF 的 `statically linked` 输出不当作普通依赖；这些 ELF 仍检查 RPATH 和文件 SHA。
发现未解析符号、开发机 RPATH、绝对 DT_NEEDED 或私有库意外从包外加载即失败。
检查时清除 `LD_LIBRARY_PATH`、`LD_PRELOAD`、`LD_AUDIT` 和 Qt 插件路径覆盖。
失败保留本次输出用于调查，下一次必须选择新目录，不覆盖已有发布产物。

输出包括 `chat-<12位GitSHA>-linux-x86_64.tar.gz`、`SHA256SUMS` 和安装树 `chat/`。
`manifest.json` 保存准确 HEAD、dirty 状态、gitlink、编译器、构建缓存/参数哈希、
打包脚本哈希、patchelf 版本、Qt 版本、库来源与原始/打包 SHA、外部系统 SONAME、
ELF 检查和每个文件的 SHA。manifest 不包含其自身哈希；完整归档由外部 SHA 清单校验。

## 内容与运行边界

包内布局：

```text
chat/
  bin/chat_server, chat_qt, chat_tui, qt.conf
  lib/                      私有运行库
  plugins/                  Qt 平台、图像、SVG、XCB GL 插件
  share/chat/themes/        5 个主题文件
  share/chat/sql/            SQL 001–028
  licenses/                 完整声明及 Qt/GCC 源码
  README.md, manifest.json
```

本轮实际提供 22 个库：Qt 6.5.3 的 Core/Gui/Widgets/Svg/DBus/OpenGL/XcbQpa，
ICU 56.1 的 data/uc/i18n，Boost 1.92 的 json/container/url，GCC 16.2 的
libstdc++/libgcc_s，OpenSSL ssl/crypto，libpq、png、jpeg、zlib 和 xcb-cursor。
不复制 glibc、系统动态加载器、整个 SDK 或系统字体。

图形驱动 GL/EGL/GLX、X11/XCB（除 cursor 外）、xkbcommon、fontconfig/freetype、
字体、GLib/DBus 等由目标系统提供；libpq 的 Kerberos、LDAP/SASL、GnuTLS 闭包也由
本轮目标 Ubuntu 系统提供。完整 68 个系统 SONAME 以实际包的 manifest 为准。
部署到新平台必须重新检查，不能仅看发行版名字或 glibc 版本。

程序使用 `DT_RPATH=$ORIGIN/../lib`，私有库使用 `$ORIGIN`，插件使用
`$ORIGIN/../../lib`。选择 RPATH 是为了覆盖系统 FreeType 等库的间接依赖，
避免先加载系统 PNG 后再将其冒充包内版本；没有开发机绝对运行路径。
`bin/qt.conf` 将插件定位到包内 `plugins/`。可替换兼容的修改版共享 Qt 库/插件并
调试其修改；包内保留 LGPL/GPL 文本和对应源码，不限制适用许可允许的替换和调试。

**可选 Fcitx5 Qt6 插件不随本轮包分发。** 上游 5.0.10 的 plugin-only 构建包含
GPL-2.0-or-later 的 `font.cpp/font.h`，不能仅标 BSD/LGPL；详见
[第三方索引](../THIRD_PARTY.md)。需要原生输入法时另行核对分发许可、Qt 私有 API ABI、
插件路径和真实 Fcitx 服务。本轮未用 IBus/Compose 代替原生 Fcitx 验收。
Noto Sans CJK SC 在两台机器上实际匹配；没有复制字体。

## 部署和启动

复制至新的用户目录，不覆盖已有程序。先在构建机、再在目标机校验归档：

```sh
cd /path/to/new-release
archive=chat-4d378d1380c0-linux-x86_64.tar.gz # 改为本次实际归档名
sha256sum --check SHA256SUMS
scp "$archive" SHA256SUMS gyl@172.20.54.83:/path/to/new-target-directory/
# 以下在目标机执行；目录须事先新建。
cd /path/to/new-target-directory
archive=chat-4d378d1380c0-linux-x86_64.tar.gz # 与构建机一致
sha256sum --check SHA256SUMS
tar -xzf "$archive"
for app in chat/bin/chat_server chat/bin/chat_qt chat/bin/chat_tui; do
  readelf -d "$app"
  ldd -r "$app"
done
```

发布包包含 libpq，不包含 PostgreSQL 服务或 psql。用已有、授权的管理客户端为
此部署建立独立数据库；只对新空库按顺序执行包内 SQL 001–028，设置 `ON_ERROR_STOP`。
不要连接业务库做迁移或测试。目标机没有 psql 时，可以从其他已有管理机连接测试库。
凭据保存到仓库外权限 0600 的 `.pgpass`，不要写入参数、报告或 Git。

```sh
chat_prefix=/absolute/path/to/chat
export PGHOSTADDR=172.20.54.83 PGPORT=5432 PGDATABASE=chat_release_test PGUSER=chat
export PGPASSFILE=/private/path/pgpass
cd /tmp
"$chat_prefix/bin/chat_server" 18080 16 16
# 其他终端：
"$chat_prefix/bin/chat_tui" ws://127.0.0.1:18080/ws
"$chat_prefix/bin/chat_qt" ws://127.0.0.1:18080/ws
```

WSS 服务端追加 `--tls-cert <fullchain.pem> --tls-key <key.pem>`。
客户端使用 `wss://<证书中的主机名>:<端口>/ws`，通过正常信任库或专用
`SSL_CERT_FILE=<测试CA>` 信任证书；不关闭证书或主机名验证。
Qt 需要可访问的 X11 会话，TUI 需要真实终端；不设开发 SDK 的 LD_LIBRARY_PATH。
停止自己启动的 Server 用其已核对 PID 的 SIGTERM；TUI 正常退出使用键盘 Ctrl+C。

## 验收与可重复性

解压到仓库之外，清除开发路径覆盖，从无关工作目录启动三个程序，再整体移动目录重测。
记录真实 `/proc/PID/maps` 和 Qt 插件加载日志；`ldd` 不能证明动态加载的图形插件已运行。
Qt/Xvfb 验证不得写成物理桌面或原生 Fcitx 验收。Chat 协议 SDK/测试辅助脚本可建立前置数据和读取最终事实，
关键聊天及文件操作必须来自发布版 Qt/TUI。

离线守卫测试现有 **27 项**，保留输出保护、输入缺失、dirty 源码、错误构建归属和
环境变量隔离，并覆盖普通/空格依赖路径、包外私有库、未解析依赖、SQL 缺失/重复/额外/
内容改变、真实仓库的最新迁移集合、Qt 版本与三份源码归档哈希。
外部命令及大归档身份使用隔离 mock，SQL 和
普通文件的哈希用真实小文件；这些离线结果不能替代实际新包或原生客户端验收。
历史验收与本轮有界复验分别记录，详细结果见 [Phase 3 验收记录](release_phase3_acceptance.md)，原始日志、截图、
归档和数据库凭据仅保存在受限证据目录，不提交。

GNU tar 固定文件顺序、Git 提交时间、owner/group，gzip 使用 `-n`。
原始 Phase 3 验收的 package-07/08 在同输入、同工具链和 umask 下，两个输出路径
生成的归档 SHA 完全相同；本次审查修复后的新包没有重复进行这一实验。
历史结果只证明同环境重复打包；没有声称跨工具链重新编译必然字节一致。
