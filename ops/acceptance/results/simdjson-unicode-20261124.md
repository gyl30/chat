# SDK JSON 与 Unicode 依赖验收（2026-11-24）

基线：`9e398ae415480a99d93e7eb46c34177b1edc7560`。
环境：Ubuntu 22.04.5 x86_64，GCC 16.0.1，CMake 3.31.10，Boost 1.92.0，
Qt 6.5.3，Ninja。使用仓库固定的 simdjson 5.0.1（`8c512a3`），不升级工具链。

- 请求由 simdjson Builder 序列化；发送和 Outbox 预检共用同一编码和字节限制。可选字段
  不主动写成 null；空参数仍为 `{}`，Outbox 继续按 19 位请求 ID 计算上界。
- SDK 每帧在自己的串行执行器内解析一次，DOM 句柄短暂借用 parser，所有会离开响应回调的
  正文、令牌及业务对象均已复制或转换成自有数据。允许回调提交后续请求，不依赖字段顺序。
  服务端 On-Demand、JSON-RPC 协议、日志处理和数据库结构保持不变。
- 清除 SDK 的 Boost.JSON 和 Boost.Describe 映射；Chat 构建排除 Boost.HTTP 未使用的 JSON
  body 中间件而不修改子模块。实际 Ninja 命令无 `json_body.cpp`、`libboost_json`；
  `readelf` 和 `ldd` 均不显示 Boost.JSON。生产仍使用其他原有 Boost 组件。
- utf8proc 2.12.0／Unicode 18（`79cdd5a`）和 Google emoji-segmenter 0.4.0（`72bdc08`）
  原样放到 `third/`，官方归档 SHA-256 与原固定值相同，六个子模块均未改变。
  空构建目录配置和重复配置均未发起 IPv4／IPv6 socket 或 connect 调用（`strace -f -e trace=network`）。

| Release 同机产物 | 基线 | 改动后 |
|---|---:|---:|
| `libchat_client.a`（不含第三方归档） | 2,417,038 B | 2,305,236 B |
| `chat_server` | 8,612,224 B | 8,612,224 B |
| `chat_qt` | 8,427,648 B | 8,850,840 B |
| SDK JSON 改动涉及的第一方文件行数 | 4,051 | 4,230 |

Qt 改为静态链接 simdjson，最终 Qt ELF 增长；旧 Qt 需要独立的动态 Boost.JSON。

Debug、Release 均构建三个正式目标。运行现有 `ops/acceptance/linux.sh` 完整 Release
集成入口，另跑 SDK JSON、Outbox 草稿、收藏、多选、阅读导航、真实静默网络停顿及 S3
定向回归。验收使用回环套接字、独立 Xvfb／D-Bus、临时 SQLite、隔离 Silo 和测试账号。
验证了混合 Unicode、大小写、ZWJ、肤色、国旗、VS、组合字符与字素截断，以及最近更新、
响应乱序、回调中发请求、断线后清理挂起请求和 Outbox 超限拒绝。没操作登录桌面，
没运行 Wayland／Portal 和生产环境测试。此轮不重复 S3 的 4 GiB 专项。
