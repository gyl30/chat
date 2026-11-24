# Shared Unicode dependencies (Qt and SQLite)

The server's SQLite Unicode functions and Qt desktop client use vendored
[utf8proc 2.12.0 / Unicode 18](../third/utf8proc/README.chat.md). CMake builds one static library.
The service does not depend on Qt.

When `CHAT_BUILD_CLIENT=ON`, CMake derives Qt's emoji property header from the same pinned
`cmake/unicode/emoji-data-18.0.0.txt`. Qt includes the unmodified generated scanner from
[Google emoji-segmenter 0.4.0](../third/emoji-segmenter/README.chat.md).
Neither dependency downloads at configure time. Ragel, Python and runtime data files are not needed.
Preserve the vendored licenses and `cmake/unicode/UNICODE-LICENSE.txt` when distributing.

Qt 6.5+ Svg and Widgets are required for the desktop client (on Linux, DBus is required as well).
Supply `CMAKE_PREFIX_PATH` or `Qt6_DIR` if Qt is not in a default location.

## JSON-RPC

服务端和 SDK 使用现有 simdjson 5.0.1 子模块，保持 `SIMDJSON_EXCEPTIONS=OFF`。
服务端的 On-Demand 不变；SDK 用 Builder 写请求，单次 DOM 解析每个响应或通知。
Chat 只使用 Boost.HTTP 的路由和 Upgrade，不构建它的可选 JSON body 中间件；固定的
Boost.HTTP 子模块源码保持原样，不要求系统提供 Boost.JSON。

## 头像图片依赖

仅服务端静态链接仓库固定的 [LodePNG 20261102](../third/lodepng/README.chat.md)。
Chat 不再要求 libpng、libjpeg 或 zlib 开发包。Boost.HTTP 可选压缩模块未被 Chat
链接，仓库内 curl 的 HTTP 压缩支持仍关闭。Qt 运行时及其格式插件仍可能自行依赖
系统 libpng、libjpeg 和 zlib，不卸载或替换这些库。

## 附件传输和 S3

[libcurl 8.23.0](../third/curl/README.chat.md) 和 [Yxml](../third/yxml/README.chat.md)
直接由仓库内源码静态构建，无系统 libcurl 回退、预编译前缀或配置阶段下载。libcurl 仅保留
HTTP/1.1 和 HTTPS，使用 OpenSSL、线程 DNS、IPv4/IPv6 与 AWS SigV4；Yxml 只链接服务端。

服务端使用现有 S3 工作器执行带 SigV4 请求头的 S3 请求；分片 PUT 和下载 GET 的查询签名
由服务端 OpenSSL 单独生成。SDK 传输这些 URL 时不重新编码。HTTPS 不关闭证书或主机名校验。

### 历史记录：miniocpp 与 AWS SDK 对照（Linux Release）

以下是替换前的历史选型证据，不代表当前构建。比较使用 miniocpp v1.0.1（`5d39de0`）。测试为同一台 Ubuntu 22.04、GCC 16.0.1、Release、Ninja `-j5`
的独立对照。AWS 固定 [1.11.935（1d07b20）](https://github.com/aws/aws-sdk-cpp/releases/tag/1.11.935)，
仅构建 `BUILD_ONLY=s3`、静态、关闭 SDK 测试、保持默认 unity 并用系统 libcurl；
miniocpp 使用上述固定版本与同一批本地依赖。构建计时从已取得源码开始，不含下载。
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
错误映射，以及 SDK 客户端仍用到的 HTTP URL/MD5 辅助接口。此次对照未引入 AWS SDK，也不维护第二套。
