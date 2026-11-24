# CMake 构建目标收口（2026-11-24）

基线 `c9e9d29cf183a43dead2a32336b9bf114930fd04`。Ubuntu 22.04.5，
GCC 16.0.1、Boost 1.92.0、Qt 6.5.3、CMake/Ninja。

| 原目标 | 新目标 | 文件 |
|---|---|---|
| `chat_server` | `server` | `server` |
| `chat_client` | `sdk` | `libsdk.a` |
| `chat_qt` | `client` | `qt/client`，主题在 `qt/themes/` |

取消 `chat_storage_support`、`chat_sqlite_storage` 和 `chat_attachment_storage`。
数据库与 S3 专属源码直接进入 `server`；`sqlite_database.cpp` 与 `blocking_worker.cpp`
从同一列表分别编进 `server` 和 `sdk`，不新增中间目标。仓库自定义第三方目标改为
`sqlite`、`yxml`、`lodepng` 和 `emoji_scanner`。保留 `SQLite::SQLite3` 和上游目标。
第一方 CMake 文件合计 **425 → 425 行**（顶层、Qt、`cmake/*.cmake` 及四个自定义
第三方 CMakeLists）；减少的是三个内部归档目标，显式源码和链接列表保留。

## 已验证

- 空目录 Debug、Release 均构建三个正式目标；`CHAT_BUILD_CLIENT=OFF` 独立构建
  `server` 与 `sdk`，显式禁用 Qt 查找。独立 CMake 消费端只链接 `sdk`，
  使用公开头构造并关闭 SDK 成功，无 Qt。
- 另一个空目录与重复配置经 `strace` 核对均无 IPv4/IPv6 系统调用；
  无改动的增量构建没有重编。Ninja、编译数据库、实际文件及归档成员确认新目标、
  两个共享源码的两份对象均正确，未生成旧产物。
- 完整 `ops/acceptance/linux.sh`；Qt 探针继续重用生产对象和主题，
  S3 探针直接链接签名/XML 对象并通过 AWS 样例。服务端测量工具直接替换三个正式对象，
  在热库、单月和双月各跑深页查询与发送，实际记录了归档读取。
- 老版本探针生成独立测试配置和 SDK 数据。新正式 `qt/client` 在虚拟 Xvfb
  无命令行服务器地址、也无密码时，使用已保存地址与凭证恢复旧账号；确认打开
  `chat/chat_qt.conf`、原 SDK 日志库、原主题覆盖文件以及新产物旁的主题。
- 实际链接命令与 `readelf`、`ldd` 均核对。Release 服务端和 Qt 的直接
  `DT_NEEDED` 列表分别保持 6 项和 13 项；服务端没有 Qt、Boost.JSON、miniocpp、
  libpng、libjpeg 或 libz。Qt 自身原有的图片库传递依赖未变。

Release 服务端 **8,681,864 → 8,677,752 B**，Qt **8,851,192 B 不变**，SDK 归档
**2,305,236 → 3,142,442 B**（现在包含原本在内部归档的两个共享对象。）

所有测试使用回环、隔离 Xvfb/D-Bus 与应用数据、测试 SQLite/Silo。不操作登录桌面、
生产或远端桌面；本轮未重跑 4 GiB。
