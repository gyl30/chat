# 稳态审计记录

2026-10-01，从实际 `origin/main = 27b2a885c0eef66950410fcb0bc737dc7bcb4502` 开始，审计 `16fd4be..27b2a88`。开始时重新 fetch，工作树 clean，HEAD 与远端一致。范围包含 SQL 008–014、服务端会话和消息路径、client library、Qt 状态与现有测试；本阶段不增加产品功能或数据库字段。

## 发现与修复

需求核对：五处测试连接配置写死 PostgreSQL 主机、数据库或用户。现改用空 libpq conninfo，Qt 子进程继承环境，统一读取 `PGHOSTADDR/PGPORT/PGDATABASE/PGUSER/PGPASSWORD`。错误密码及不存在数据库的负例只覆盖被测字段，不覆盖其他环境配置；连接成功与否由 libpq 决定。

生命周期核对：`client_bridge::connect_to_server` 原来只推进会话列表和历史的 generation，旧搜索、上传、下载结果已经排队时可能进入新连接的界面。新增回归先复现失败，再让新连接同时推进已有五个计数；没有增加计数或新的状态框架。

构建核对：GCC 16 的静态反射在 ASan/Debug 下为群管理四个同名局部解析类型生成了重复汇编符号。改为四个独立类型名后编译通过，解析规则和协议未变化。

仓库原则核对：保留有实际职责的状态，不机械拆文件。没有新增兼容路径、配置框架、一次性包装或未来功能预留字段。

## 状态与职责

| 状态 | 当前用途与审计结论 |
|---|---|
| bridge 五个 generation | 分别丢弃过期的会话分页、历史分页、搜索、上传、下载回调；新连接和 close 均使旧请求失效 |
| `messages_loading/messages_loaded/history_exhausted` | 正在请求、已有历史基线、向前翻页到尽头；首次加载和恢复不能只用一个标志表达 |
| typing 本地空闲、刷新、远端过期及清理计时 | 分别结束本地输入、限制刷新频率、让远端提示失效、清理显示；切换和断线清理，重连不恢复旧输入 |
| attachment upload/download job | 一个请求链保存内容、偏移和完成回调；传输失败结束 pending 请求，服务端上传随 session 销毁或退出当前群清理 |
| `attachment_sending` | 禁用重复文件发送，完成或连接/会话清理时恢复控件 |
| reconnect 状态及计时器 | 区分重试间隔、倒计时、延迟提示及恢复提示；历史从已加载范围起点重新获取，包含编辑和删除 |
| group `pending/available/contacts_ready` | 操作待响应、成员快照有效、联系人可用于邀请；断线禁用操作，恢复后重新打开获取快照 |

没有发现可以安全删除的 shadow state。client 与 server 的单连接请求处理保持顺序，未依据假设增加 edit/delete generation 或通用版本框架。

## 事务、读位与附件

发送和邀请都在事务内锁住同一个 conversation；锁取得后再检查当前成员/角色和最新消息水位。任免管理员、改名、邀请、退出的权限也在锁后查询。群管理事务正常 COMMIT/ROLLBACK，SQL 错误关闭连接再归还 lease；pool 下一次 acquire 重连。新增测试验证事务失败后的重连和显式 rollback 后连接复用，没有给 pool 增加自动事务层。

`joined_message_id` 只用于未读基线，`last_read_message_id` 只由真实 `mark_read` 推进。群双勾取当前其他成员中至少一人的真实读位；成员快照会移除退出者的读位。已有 model 测试覆盖仅自己、部分其他成员、成员变更和单聊；没有永久 per-message 回执表。

附件仍为单文件 10 MiB、32 KiB chunk、数据库持久化。新增真实 WebSocket 测试在 begin/chunk 后断开连接，重连不能 finish 旧上传且可以重新上传；退出群后旧上传失效，另一个会话可以开始新上传。现有测试继续覆盖大小、分块偏移、取消、删除、下载权限以及 Qt 下载断线。权限变更能力由下一产品阶段实现，本阶段不提前增加移除成员路径。

## 小群规模

`get_members` 聚合当前成员，历史返回所有当前成员的读位，`publish_conversation` 查询和遍历所有当前其他成员：这些路径随成员数 M 增长。历史消息仍按会话/消息索引取 50 条；会话列表按成员索引及活动 cursor 分页。没有新增人数上限或大群基础设施。

真实 client/server/数据库测试分别建立 3、10、50、200 人群，核对完整成员数和读位数，每种规模各测量三个 get_members/history/send 样本。只有发送者在线，其他成员离线；因此包含发布的成员查询和遍历，不代表 200 个在线连接的 fanout 吞吐。测量打印在 server 测试的 `GROUP_SCALE` 输出中，含网络和数据库延迟，不作性能阈值断言。

该测试证明当前模型在已测 3–200 人范围内形成完整闭环；200 不是协议上限或容量承诺。实际扩大在线群规模前，应测在线 fanout、消息频率和慢消费者；当前没有证据需要对象存储、Redis、事件总线或永久消息回执表。

## 验证范围

完整 CTest 的 13 个测试包含 migration、client、PostgreSQL、HTTP/WebSocket、server、Qt model/delegate 和真实多窗口 UI。已有测试覆盖 concurrent send、leave/rejoin、typing/disconnect、edit/delete/reconnect；本次增强真实 send/invite 竞争、事务连接恢复、附件断线/退出及过期 bridge 回调，复用现有公共边界。

最终验证结果记录在 [开发状态](development-status.md)。构建目录均位于被忽略的 `build/` 内，测试环境通过调用进程提供；没有修改或补写 SQL 001–014。
