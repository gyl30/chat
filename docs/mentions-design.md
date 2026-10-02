# 群成员提及

SQL 019 增加 `message_mentions(message_id,user_id)`，主键保证同一消息的目标不重复。消息与用户外键级联清理；退出群不删除历史目标。旧消息不根据当前成员回填提及。

只解析群聊文字正文中的 `@username`。现有用户名没有字符限制，因而使用当前成员完整用户名的字面匹配，区分大小写，并排除词内、邮箱和 `@@`。空格、Unicode 和正则符号仍是用户名的一部分，不引入另一套用户名规则。正文中存在前缀相同的姓名时，PostgreSQL ARE 使用最长完整匹配；所有姓名先转义为字面模式。依据见 [PostgreSQL 17 正则规则](https://www.postgresql.org/docs/17/functions-matching.html)。单聊文字和附件文件名不产生群提及。

发送和编辑在既有 conversation 行锁、事务内解析当前成员并保存目标，与正文一起提交。编辑替换目标，删除清空目标。加入、退出、移除与发送共享锁边界，不能提及已经退出的成员；历史已保存的目标仍表示当时的事实。新增目标使完整 notification 超过现有 64 KiB 边界时，整个修改回滚。

不新增 RPC 或 notification。发送确认、历史、搜索、会话最新消息及 `message/message_updated` 都携带 `mentions:[{user,username}]`，客户端不能在发送参数中指定目标。username 从当前 users 查询，持久化身份使用真实 user ID。SDK 检查目标 ID、姓名、重复项和删除消息的空目标约束。

Qt 按权威目标高亮正文，目标包含当前用户时显示“提及你”；搜索、分页和重连复用相同模型。多行文字使用 [QTextLayout](https://doc.qt.io/qt-6/qtextlayout.html) 绘制。编辑和历史按现有 edited_at 合并，旧结果不能恢复已经清空的目标。发送、编辑和删除回调在 Qt 线程检查现有 connection generation，不新增提及缓存、计数器或生命周期状态。

静音仍抑制所有桌面通知，包括提及；消息接收、未读和实际已读语义不变。没有提及通知等级、独立未读计数、用户自动补全或提及历史表。
