# Linux 发布包来源与许可工程审查

审查日期：2026-10-14。源码基线 `5e195498d5b1e62f2c59d890761c1d4810912d0e`。
本报告核查已验收产物的来源和分发材料，不替代适用许可的最终法律判断。
没有重编 Qt、修改业务代码、重新生成发布包或公开上传二进制。

## 结论与适用范围

| 决策维度 | 结论 | 依据与边界 |
| --- | --- | --- |
| 技术发布就绪性 | **CONDITIONAL GO** | 保留 Phase 3 指定 Ubuntu 平台的历史运行结论；本轮增加 ELF 与库替换检查，不冒充新一轮完整通信验收。原生 Fcitx5 / 物理桌面仍未验收。 |
| 分发工程证据完整性 | **INCOMPLETE** | 已取得实际 Qt/ICU 二进制的官方归档逐字节对应证据、源码范围和声明清单；完整厂商源码/补丁/构建关联及最终分发材料充分性仍待确认。 |
| 公开发布许可状态 | **存在明确待补充材料** | 维护者应确认采用的授权路径、对应源码和必要构建安装材料的充分性、Chat 自身的发行条款。不得据本报告公开上传当前包。 |

首选路线 **B：保留现有 SDK，补充具体来源/构建关联及许可确认**。
没有证据要求立即替换 SDK。二进制来源不明这一问题已得到直接证据解释；
缺少完整历史构建记录不等于证明存在修改，也不意味着法律普遍要求字节级可重复构建。
最终许可判断应由实际发行主体在适用许可与发行方式下确认。

## 精确调查对象

| 项目 | 值 |
| --- | --- |
| 归档 | `chat-7dc53d7cf572-linux-x86_64.tar.gz` |
| 字节数 | `231177386` |
| SHA-256 | `06a8d3e01ab4d8b3f2e7435751676fc38cb33677f1e2e25258a296f9bd819234` |
| manifest HEAD | `7dc53d7cf5728f0f46a514cad2dfe7c0ff9be1fa` |
| manifest 打包工具 SHA-256 | `4ad8b28c3e932a451d6693ce3c37cec2ca457e5c1e6089bb65dcfe82913e75f0` |
| 实际 ELF | 34：3 个应用、22 个私有库、9 个 Qt 插件 |
| manifest 检查 | 237 个列出文件全部 SHA 匹配；仅 manifest 自身不在其文件清单中 |

调查的是 Phase 3 `review-fix/package-02` 原包，不是当前 HEAD 的新构建。
当前源码 HEAD 与产物 HEAD 不同有明确历史原因，不能混写。
完整逐文件 SHA、SONAME、Build ID、DT_NEEDED、RPATH 和依赖位置在外部证据中。

## Qt SDK 的正向来源证据

实际 SDK 为独立 Qt **6.5.3 / gcc_64**；路径由现有构建缓存、qmake 和 ELF 来源记录共同确认。
SDK 位置只是取证输入，不作为安装版运行要求。

[Qt 官方 Updates.xml](https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/qt6_653/Updates.xml)
列出 `qt.qt6.653.gcc_64`，版本 `6.5.3-0-202309260341`，RHEL 8.4 / x86_64 原始二进制组件。
本轮只获取相关 Qtbase、QtSvg、ICU 三份归档，共 **28,488,407 字节**；
从官方 HTTPS 入口解析到镜像，记录实际 URL，核对官方提供的 SHA-1，并另算 SHA-256。
这不是厂商数字签名验证；可信度来自官方入口、校验元数据和实际文件的联合证据。

| 官方组件归档 | SHA-256 |
| --- | --- |
| `qtbase-Linux-RHEL_8_4-GCC-Linux-RHEL_8_4-X86_64.7z` | `f4b466016ddd643aa42018247d2a3d1d15b4574f3146735d8be821f5c183a6e6` |
| `qtsvg-Linux-RHEL_8_4-GCC-Linux-RHEL_8_4-X86_64.7z` | `637f05a40375f87e99f10da9c61bd0c650c9ce6f36b6d5edba47cb803decc99c` |
| `icu-linux-Rhel7.2-x64.7z` | `3b2efeeb3d94d3f65cb80bbcddc1a1f1468d310c06d5f810f0d6f833d5731bf4` |

官方归档 → 当前 SDK：**7 个 Qt 库、9 个插件、3 个 ICU 库的全文件 SHA 均相同**；
额外核对 qmake 也相同。这证明这 19 个发布来源文件属于官方发行二进制，
并证明它们在当前 SDK 中没有相对该归档的本地字节修改；没有穷举整个已安装 SDK。
SDK → 发布包：这些 Qt 文件 `.text`/`.rodata` 与 SDK 一致，打包脚本记录了 RPATH 重定位；
不能因此将打包后全文件 SHA 写成原始归档 SHA。ICU 三库全文件仍匹配。

### 安装元数据与构建信息

官方 `meta.7z` SHA-256 为 `33bef9c67b6bc1596e9c6028540171a5ed13143381056ca64dfb027501c57734`，
其 SHA-1 与 Updates.xml 相同。实际安装脚本 `isOpenSource=true`，明确执行
`QT_EDITION=Enterprise → OpenSource`、清空 `QT_LICHECK`、初始化安装前缀和 qt.conf。
当前 qconfig、`.pc` 前缀与额外 qt.conf 的差异符合这些官方安装操作。
原归档的 Enterprise 字段和本地的 OpenSource 字段都不能单独决定授权。

| 信息 | 证据与结论 |
| --- | --- |
| 编译器 / ABI | 原始 qmodule / CMake 元数据：GNU 10.3.1、Linux x86_64、非交叉编译；与 Chat 编译器 GCC 16.2.0 区分。 |
| 共享/优化方式 | qconfig：shared、release、separate debug info、rpath；配置快照保存。 |
| 本地代码修改 | 本轮涉及的 19 个来源运行库/插件与官方归档全字节一致。其他 SDK 文件未做穷举。 |
| 历史安装工具/命令/日期 | **UNKNOWN**。限定安装日志没有该 SDK 记录；不读取私人 shell 历史。符合 aqt 操作不等于证明历史上使用了 aqt。 |
| 完整 configure / CI / CMake cache | **UNKNOWN**。SDK 不含对应 config.summary/config.opt/config.log/CMakeCache.txt；未编造参数。 |
| 厂商构建前补丁及准确源码输入 | **UNKNOWN**。公开版本/tag 与发行组件有版本关联，但二进制 Updates 没有源 commit、补丁清单或完整 recipe。 |

## 实际 Qt 组件、插件和源码范围

版本均为 **6.5.3**。每行二进制来源均经过上面的官方归档 → SDK → 发布包核对；
Qt 源文件采用相应文件 SPDX 的`LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only` 选项，
具体选项按该文件核对；本包说明声明使用 LGPLv3 路径。
这不是说源码树所有文件都只有 LGPL，也不是将各选项同时强制套给应用。
第三方编入代码保留自己的许可。

| 分发文件/组件 | 对应完整源码归档 | 许可与声明 |
| --- | --- | --- |
| Core、DBus、Gui、Widgets、OpenGL、XcbQpa 六个共享库 | qtbase 6.5.3 | Qt 源文件 SPDX、Qt LICENSES 与第三方 attribution |
| Svg 共享库 | qtsvg 6.5.3 | Qt 源文件 SPDX、Qt LICENSES 与第三方 attribution |
| `platforms/libqxcb.so`、`libqoffscreen.so` | qtbase 6.5.3 | 对应平台插件源文件及其第三方声明 |
| `imageformats/libqgif.so`、`libqico.so`、`libqjpeg.so` | qtbase 6.5.3 | 对应源文件；JPEG 另有 IJG/libjpeg-turbo 声明 |
| `xcbglintegrations/libqxcb-glx-integration.so`、`libqxcb-egl-integration.so` | qtbase 6.5.3 | 对应 GL 集成源文件及声明 |
| `imageformats/libqsvg.so`、`iconengines/libqsvgicon.so` | qtsvg 6.5.3 | 对应 Svg 插件源文件及声明 |

没有随包分发 Qt Network、Quick、QML、WebEngine 或 Fcitx5 插件；
不把构建机上其他已安装模块算作分发组件。

| 随包完整源码归档 | SHA-256 |
| --- | --- |
| `qtbase-everywhere-src-6.5.3.tar.xz` | `df2f4a230be4ea04f9798f2c19ab1413a3b8ec6a80bef359f50284235307b546` |
| `qtsvg-everywhere-src-6.5.3.tar.xz` | `fc41c47b69ca1f80473cd4b11996394155781105345392961d064d04f95d5bef` |
| `gcc-16.2.0.tar.xz` | `e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e` |

[官方 6.5.3 源码索引](https://download.qt.io/archive/qt/6.5/6.5.3/submodules/)以及
[qtbase v6.5.3](https://code.qt.io/cgit/qt/qtbase.git/tag/?h=v6.5.3)、
[qtsvg v6.5.3](https://code.qt.io/cgit/qt/qtsvg.git/tag/?h=v6.5.3)可追溯公开发布源码。
两 tag 分别指向 `372eaedc5b8c771c46acc4c96e91bbade4ca3624`、
`220cd8c6261552f0fcf47061c64f862adae29b55`。
完整归档含这些实际目标的源代码、构建脚本及第三方树；这是模块范围覆盖证据，
**不是**从版本号推导完整厂商构建前源码/补丁必然一致。
需要确认是否有实际必需、但归档未包含的厂商构建安装材料。

120 条声明文件比较记录覆盖 113 个独立 Qt 声明文件（含 98 个第三方文件），
全部逐字节匹配源码归档；7 个通用许可文本各与两个归档比较。51 个 attribution 的 LicenseFile 引用全部存在，
另有 GCC 的 COPYING3/COPYING.RUNTIME 两项精确比较通过。
包中 166 个独立声明文件与三份源码归档分别计数。
Qt SDK feature 元数据表明 double-conversion、PCRE2、HarfBuzz、JPEG、PNG 使用内置版本，
FreeType、zlib 使用 system 版本。内置 JPEG/PNG 与包内另行复制的 Ubuntu 库是不同来源。
完整第三方声明是覆盖超集，不能把每个 Android/测试/其他平台 attribution 都宣称已编入 Linux 产物。

| 6.5.3 源码中的相关第三方项 | 归档记载版本 / 许可 | SDK feature 与覆盖边界 |
| --- | --- | --- |
| double-conversion | 3.3.0 / BSD-3-Clause | 内置；归档源码与声明均提供 |
| PCRE2 / SLJIT | 10.42 / BSD 类许可与具体 Binary-Like-Packages Exception、BSD-2-Clause | 内置；保留完整许可而不简化为一个 BSD 标签 |
| HarfBuzz | 8.2.0 / MIT | 内置；归档源码及版权提供 |
| libjpeg-turbo | 3.0.0 / IJG、BSD-3-Clause、Zlib | 内置；与独立 Ubuntu 2.1.2 私有库区分 |
| libpng | 1.6.40 / Libpng、libpng-2.0 | 内置；与独立 Ubuntu 1.6.37 私有库区分 |
| FreeType / zlib | 源码树分别为 2.13.2 / 1.3，FTL 或 GPL-2.0、Zlib | SDK 使用系统库，不能把源码树版本当作所加载宿主版本 |
| Qt 绘图的 FreeType gray rasterizer | attribution 未标版本 / FTL 或 GPL-2.0 | 单独编入的绘图来源声明仍需保留，不能因 system-freetype 就忽略 |

表中版本来自实际归档 attribution；feature 是原 SDK 配置证据。
厂商构建前是否替换了这些源文件尚无完整补丁记录，不能据表声称逐个嵌入组件已从二进制独立还原。


## 其他私有库、静态组件和宿主边界

| 实际随包组件 | 实际版本/来源 | 许可材料与剩余边界 |
| --- | --- | --- |
| Boost container/json/url | 1.92.0；manifest 原始文件 SHA 与现有来源一致 | BSL-1.0 全文；未附完整源码，按适用许可确认声明保留。 |
| ICU data/uc/i18n | 56.1；三库与 Qt 官方 ICU 归档相同 | ICU/Unicode 及其第三方完整 license.html；不是只有一个 MIT 标签。 |
| libstdc++、libgcc_s | GCC 16.2.0；本地构建配置和两个安装位置原始文件 SHA 对应 | GPLv3 + GCC Runtime Library Exception 3.1、完整源码及记录的 configure；未执行源码重建。 |
| libssl、libcrypto | OpenSSL 3.0.2；升级日志指向 libssl3 3.0.2-0ubuntu1.26，但旧 deb 精确字节对应 UNKNOWN | Apache-2.0 及 Ubuntu copyright；不得将当前升级后的 .30 版本冒充原包版本。 |
| libpng16 | 1.6.37；升级日志指向 1.6.37-3ubuntu0.5，但旧 deb 精确字节对应 UNKNOWN | libpng 及关联版权声明；不得将当前 .7 版本冒充原包版本。 |
| libjpeg.so.8 | libjpeg-turbo8 2.1.2-0ubuntu1；现有来源 SHA 一致 | BSD/IJG/zlib 相关声明；包 README 有 IJG attribution。 |
| libpq.so.5 | libpq5 14.24-0ubuntu0.22.04.1；现有来源 SHA 一致 | PostgreSQL 许可及包内版权。 |
| libxcb-cursor.so.0 | 0.1.1-4ubuntu1；现有来源 SHA 一致 | MIT/X11 包内版权。 |
| libz.so.1 | zlib1g 1:1.2.11.dfsg-2ubuntu9.2；现有来源 SHA 一致 | zlib 及包内版权。 |

当前主机在原包生成之后升级了 libssl3 `.26 → .30`、libpng16 `.5 → .7`。
原包字节仍完全符合 manifest；本轮没有安装或升级系统软件。
历史版本不能仅凭今天的 dpkg 输出决定；升级日志和旧包获取结果单独保留。
限定旧 deb 获取失败：两个旧归档 URL 返回 404；Launchpad 官方 metadata 可确认历史
publication/build，但未取得对应 deb，旧包字节、section 和 copyright 比较 **NOT_RUN**。
包内七项 Ubuntu copyright 与当前主机相同是另一项事实，不代替旧 deb 对照。

应用静态编入组件另行核对：Capy/Corosio/Http（BSL）、wslay（MIT）、simdjson
（上游 Apache/MIT 双许可）、spdlog/fmt（MIT）、FTXUI（MIT）、utf8proc/Unicode 声明、
emoji-segmenter（Apache-2.0）。Gitlink、CMake 固定源码/哈希、声明复制与符号证据见外部清单。
符号只能佐证编入，不能代替完整构建输入推导。上游多许可证文件保留，不替发行者随意选择许可。

68 个宿主 SONAME（另加动态加载器）未复制进包，包含 glibc、图形驱动、X11/XCB、
xkbcommon、FreeType/fontconfig、DBus/GLib，以及 libpq 的 Kerberos/LDAP/SASL/GnuTLS 闭包。
这是工程运行边界，不能不经适用条款分析就统称 GPL 定义的 System Libraries。
许可目录中额外的宿主版权文件不意味着分发了这些二进制。

## 分发义务检查与证据边界

按[Qt 官方 LGPL 义务说明](https://www.qt.io/development/open-source-lgpl-obligations)、
[Qt 6.5.3 LGPL 原文](https://raw.githubusercontent.com/qt/qtbase/v6.5.3/LICENSES/LGPL-3.0-only.txt)
以及[GPLv3 原文](https://www.gnu.org/licenses/gpl-3.0.en.html)检查；
6.5 在线文档可能更新到后续补丁版，不能用其当前组件表覆盖实际 6.5.3 源文件声明。

| 项目 | 当前事实 | 尚需确认 |
| --- | --- | --- |
| Qt 使用声明与完整许可 | 发布包 README 明示 Qt/LGPLv3；包内 GPL/LGPL 文本与官方 6.5.3 文本 SHA 相同。 | 实际发行条款不得限制适用许可允许的修改及调试；维护者确认所采用授权路径。 |
| 库的对应源码 | 同包提供完整 qtbase/qtsvg 源码，覆盖实际目标；没有已发现的 SDK 运行库本地改动。 | timestamped 官方组件与实际源码/厂商补丁、必要构建安装材料之间的关联，及所选源码交付方式充分性。 |
| 动态替换与调试 | README 明确允许兼容修改库/插件替换及调试；无 manifest 哈希启动锁。 | 动态链接不免除所分发库自己的源码义务。 |
| LGPL Combined Work | 使用共享库机制；未用静态 Qt。 | 按 LGPL §4 的具体路线核对应用条款、通知、源码与替换条件；共享机制与 relink 对象方案不是无条件同时要求。 |
| Installation Information | 运行路径、库/插件位置可解释；标准源码内有构建安装脚本。 | LGPL §4(e)/GPL §6 在 User Product 等适用条件下的义务，由发行主体确认；不能把所有安装信息义务一概视为无条件。 |
| 运行时版权告知 | 当前关于页不显示版权告知；不能仅因显示应用名/版本就机械套用 LGPL §4(c)。 | 若未来展示运行时版权告知，重新检查该条件。 |
| GCC runtime | 完整源码、配置快照及 GPL/Exception 文本已有。 | [GCC runtime 说明](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html)：例外对符合条件的应用链接有作用，不取消单独分发运行库自身的许可义务。 |
| 第三方 attribution | 全文和来源 SHA 清单已有；Qt 复制声明与归档对应。 | 对各实际组件按其具体许可确认版权、通知及适用源码要求，不用一个 Qt 标签覆盖全部。 |
| Chat 自身发行许可 | 仓库目前没有声明 Chat 的 LICENSE，已有第三方索引也不指定 Chat 许可。 | 维护者确认自身版权授权及公开发行条款；不在本轮自行指定开源许可证。 |

### 实际共享库替换验证

在独立副本，仅以原 SDK 中兼容 QtCore 替换包内 QtCore：

- 原包 QtCore SHA：`5de296a51b2a381420c706c864e19263ca8f3439d445725c13c3c97b67fa5334`。
- 替换文件 SHA：`ac391204135f1435ca1c97a7f92e6f94d9d369f6b12fb823923496360a272bd8`。
- manifest 未改；清除 LD_LIBRARY_PATH/PRELOAD，实际 `/proc/PID/maps` 加载替换文件。
- 独立 Xvfb/X11 下两次真实 Qt/XCB 登录窗口正常显示并正常退出，前后截图已查看。
- 应用 `$ORIGIN/../lib`、库 `$ORIGIN`、插件 `$ORIGIN/../../lib`；qt.conf 指向包内插件。
- 原包与 SDK 未改，副本、临时配置和本轮进程已清理；两次外部驱动失败也保留分类日志。

这是替换机制实际可用的证据，**没有**从源码编译修改版 Qt，也没有验证任意 ABI 不兼容库。
不将此小实验当作全部 LGPL 条件或物理桌面/Fcitx5 的法律或运行证明。

## 待补充材料与最小路线

1. **Qt 发行来源/源码关联**：保存本轮官方元数据、归档及对比；向 Qt 官方发行资料或授权主体
   取得 `6.5.3-0-202309260341` 与准确源码/补丁的关联说明，以及确属必要的构建安装材料。
   已恢复的 feature/编译器元数据不能伪造为完整 configure recipe。
2. **源码交付充分性和发行条款**：维护者结合法律专业意见确认 LGPLv3 路径、对应源码材料、
   Combined Work 条件、可能适用的安装信息及 Chat 自身条款。不能只提供普通下载链接
   就一概认定所有源码义务完成，也不能仅因没有字节级重建就断定违规。
3. **历史 Ubuntu 库身份**：按升级前版本核对旧二进制包和版权；失败时保持证据缺口，
   不用新系统库替换原包来掩盖取证问题。
4. **后续产物材料**：许可确认后若补充发行目录材料，应生成新包并给出新身份；
   本报告没有变更旧包，不能声称新证据已经在旧归档内部。

只有必要材料无法恢复或发现真实来源不符时才考虑路线 C。
最小替换计划是：在仓库外用明确的官方 qtbase/qtsvg 6.5.3 源码和显式配置构建所需
共享模块/插件，保存依赖来源、补丁、脚本、缓存和许可；不升级版本或重建不需要的模块。
保留当前 SDK/包。替换将产生新 Qt Build ID/文件 SHA，必须重打包并重新建立
ELF/主题/XCB/安装重定位及有代表性的客户端通信回归，旧 Phase 3 不能直接继承为新二进制 PASS。
未打包的可选 Fcitx5 Qt6 插件涉及 GuiPrivate ABI，须在另行部署时重新核对/重建和真实测试，
不把它加入本轮范围。成本在真实配置和机器评估后估算；本轮没有启动 Qt 编译。

## 本轮执行与未执行

| 检查 | 结果 |
| --- | --- |
| 归档 SHA、237 个 manifest 文件 | PASS |
| 34 ELF 的当前 ldd -r | PASS；无 missing/undefined；仍依赖已声明宿主库 |
| 官方归档 → SDK 的 19 个来源文件 | PASS；额外 qmake 也匹配 |
| SDK 配置快照及官方安装脚本差异解释 | PASS；完整历史构建/补丁关联 UNKNOWN |
| 三份完整源码归档 SHA | PASS |
| Qt 120 条比较记录 / 113 个独立声明文件、51 个 LicenseFile、GCC 两项文本 | PASS |
| Qt LGPL/GPL 文本与官方 6.5.3 原文 | PASS |
| 独立副本共享库替换 / 两次实际 Qt/XCB 启动 | PASS；仅登录页，不作完整业务验收 |
| Qt 源码重建、完整厂商 recipe 复原 | NOT_RUN / UNKNOWN；没有证据要求自动重建 |
| Normal / ASan / UBSan / 打包离线测试 | NOT_RUN；仅文档变化，无工具或 C++ 变化 |
| Phase 3 完整跨机聊天矩阵 | NOT_RUN；保留历史结果，不重复 85 分钟矩阵 |
| 公开许可最终确认 | BLOCKED；待具体材料及发行主体确认 |
| 原生 Fcitx5 / 物理桌面 | NOT_RUN；本轮范围外且插件不分发 |

## 外部证据索引

敏感记录和归档不进 Git。本轮证据根为开发机 `chat-review/release-compliance/`，
定位时可由审查执行者提供绝对位置；关键规则和结论不依赖个人路径才能理解。

- `package-identity.json`、`initial-git-state.json`：归档与源码身份。
- `sdk-provenance/report.md`、`result.json`、`official-archive-downloads.json`、
  `sdk-vendor-comparison.json`：官方来源、逐文件 SHA/section、实际构建元数据。
- `sdk-provenance/official-installscript.qs`、`installation-rules-sources.json`、
  `source-reference-downloads.json`：安装操作和官方源码/tag 资料，含实际 URL 与 SHA。
- `components/report.md`、`components.json`：全部 ELF、私有/宿主边界、源码/版权及历史 Ubuntu 取证。
- `legal/sources.json`、`license-text-comparison.json`：官方许可原文来源与文件校验。
- `replacement/result.json`、`review.json`、`cleanup.json`：实际替换、截图检查和清理结果。

原始 Phase 3 数据见 [历史验收记录](release_phase3_acceptance.md)，
运行及打包边界见 [Linux 发布说明](linux_release.md)，许可索引见 [THIRD_PARTY](../THIRD_PARTY.md)。
