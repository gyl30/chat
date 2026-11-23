# Shared Unicode dependencies (Qt and SQLite)

The server's SQLite Unicode functions and the Qt desktop client use the pinned
utf8proc 2.12.0 / Unicode 18 source. CMake verifies the upstream archive,
its checked-in source hashes and the pinned Unicode data hashes; the source is
built only once. The server does not depend on Qt.

When `CHAT_BUILD_QT_CLIENT=ON`, Qt additionally reads the pinned Unicode 18
`cmake/unicode/emoji-data-18.0.0.txt` and derives its emoji property header
in the build directory. The data's complete Unicode License V3 is retained
in `cmake/unicode/UNICODE-LICENSE.txt`. The utf8proc license is retained in
`cmake/utf8proc-LICENSE.md`. These notices must accompany any distribution
containing the statically linked code or generated property tables.

Qt 6.5+ Svg and Widgets are required for the desktop client (on Linux, DBus
is required as well). CMake uses the installed Qt package; supply
`CMAKE_PREFIX_PATH` or `Qt6_DIR` if Qt is not in a default location.

Qt also uses the official Google emoji-segmenter 0.4.0 at commit
`72bdc08c02be6cccdfc8cb1055fea4822a6494c1`:

- Archive SHA256: `6561ffc3fe83df9f9bf07da1d02522c9b0c6878aeaf56282116b92288367e51c`.
- Unmodified generated scanner SHA256: `e41071624cf91187ae26396b3d2b9adda648c5afb4519ff562e7aeb9415c157d`.
- Upstream Apache-2.0 LICENSE SHA256: `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`.

The archive's complete LICENSE and scanner copyright notice must accompany
distributions. Normal builds need neither Ragel nor scanner regeneration.
`FETCHCONTENT_SOURCE_DIR_CHAT_EMOJI_SEGMENTER` permits offline configuration
while preserving the source hash checks. The Unicode/emoji data inputs are
not loaded into the server's runtime process.


## MinIO C++ SDK

`third/miniocpp` 以子模块固定官方 v1.0.1（`5d39de018b81ac45fa0645cfc61b275d869ceea1`）。Chat 构建其 C++17 头文件接口和静态库，不启用测试／示例、文档、RDMA 或 C++20 模块；Chat 自身仍用 C++26 Reflection。TL expected 已在 SDK 中。

OpenSSL、zlib、nlohmann_json、pugixml 由系统开发包提供。SDK 先查合适的 cpp-httplib（0.51+）和 INIReader 包；没有时，按上游逻辑克隆到构建目录下的 `third/miniocpp/_deps/`，使用 cpp-httplib v0.53.1（`b8fe69e4e892ec394ca8de27ab2fe219008690b6`）和 inih r58（`5cc5e2c24642513aaa5b19126aad42d0e4e0923e`）。

cpp-httplib 走源码回退时，上游每次重新配置都会 `git fetch --tags origin`，**即使该目录已经存在**。inih 回退在本地源码存在时只 checkout，不重新下载。系统同时有合适的 cpp-httplib 和 INIReader 包时，这两条 Git 回退均不走；`FETCHCONTENT_FULLY_DISCONNECTED` 不会禁止上游自己执行的 Git。已有的 utf8proc/emoji-segmenter 首次下载由 FetchContent 完成，在原构建目录重用；离线可提供校验匹配的源码，使用 `FETCHCONTENT_SOURCE_DIR_UTF8PROC` 或 `FETCHCONTENT_SOURCE_DIR_CHAT_EMOJI_SEGMENTER`。

### S3 SDK 取舍（Linux Release）

继续使用 miniocpp。以下是同一台 Ubuntu 22.04、GCC 16.0.1、Release、Ninja `-j5`
的独立对照。AWS 固定 [1.11.935（1d07b20）](https://github.com/aws/aws-sdk-cpp/releases/tag/1.11.935)，
仅构建 `BUILD_ONLY=s3`、静态、关闭 SDK 测试、保持默认 unity 并用系统 libcurl；
miniocpp 使用本节的固定版本与同一批本地依赖。构建计时从已取得源码开始，不含下载。
程序体积为两个独立对照程序去除符号后的大小，不是 Chat 服务端整体体积。

| 指标 | miniocpp | AWS SDK for C++ |
|---|---:|---:|
| 干净构建（仅 SDK 及其构建依赖，单次） | 18.91 s | 34.92 s |
| 无改动再构建 | 0.04 s | 0.06 s |
| 对照程序体积 | 2,238,984 B | 10,837,360 B |
| 对照程序峰值 RSS 中位数（各 5 次） | 10,944 KiB | 20,928 KiB |
| `ldd` 中解析出的共享库（含传递依赖） | 12 | 34 |

两边在同一隔离 Silo 上用小文件完成相同流程：Initiate、按精确 key 查未完成上传、预签名
上传、ListParts、Complete、HEAD 核对元数据与长度、预签名下载、Delete、Abort。对象字节
继续走预签名 HTTP，不使用 TransferManager。AWS 原型显式选 path-style 与回环 endpoint，
验证了预签名 Host、分片参数、带 Content-MD5 的 PUT 及下载内容；默认校验设置在这条
路径可用，不代表 SDK 直接流式 PUT、所有 checksum 组合或所有 S3 兼容后端均已验证。

AWS 的 typed List/Abort/HEAD 可以减少适配层自解析 XML，但 S3-only 仍需 SDK Core、CRT 及其
静态组件，当前 libcurl 配置也带来更多动态传递依赖。消费端使用 Outcome，不需要第一方
异常，测试程序加 `-fno-exceptions` 亦成功编译、链接；这不代表 SDK 自身在该开关下构建通过。
迁移还涉及全局 Init/Shutdown 生命周期、现有工作器上的重试和超时边界、分片签名参数、
错误映射，以及 SDK 客户端仍用到的 HTTP URL/MD5 辅助接口。在没有实测收益前不维护第二套。
