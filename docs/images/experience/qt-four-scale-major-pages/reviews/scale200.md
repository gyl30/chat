# Qt 200% 原生主要页面子集独立复审

原 scope：`/tmp/chat-daily-native-qt-scale200-r2-2y82nyss`。只读复制、亲看全部 000–023 的完整 original PNG，未用裁图替代原图；核 AT-SPI tree、X11 xwininfo、operations、freeze、cleanup。未操作产品、SQL、仓库或构建。

结论：本子集未发现高置信 material 裁切、重叠或不可达按钮。此结论不是完整页面/状态矩阵、全部键盘路径、Orca、业务9phase或目标92分验收。

## 实际计数与身份

- 24 captures、24 不同 PNG SHA；逐文件 SHA 对 captures.json 24/24 MATCH。辅助 tui-image.png 不计 capture。
- 70 operations：capture21、click20、field4、finish1、keys14、qt-text1、resize3、wait-qt6；没有 failed-op。20次 click 全部记录 scale2 与 physical_click。
- a11y-errors.json 为 []；24 captures 的 a11y_error_count 均0。这仅是 observer 本次收集错误0，不代表全桥接契约/通知/所有AT内容无缺口。
- freeze head `7375815f70e3d9d6f456c3d9a0d173411800f294`，expected_status_lines=[]；24 captures 一致 PID3017629/ticks556353206、exe `/home/gyl/example/chat/build/qt/chat_qt`、SHA `370a68ae9448916c86e3085f1a459e9b1ea0f2d292cf18113c842de96a1b24a4`。
- driver SHA `0a79f50ab2aec968d0c42b0eb32003a740d33a2e5fcaea076f7ee412de383d30` 与 exec90463 exit0 为 root 运行记录提供的 provenance；本地复制材料没有独立 driver.exit。finish 原文为 EXECUTOR_HANDOFF_NOT_PRODUCT_PASS，不将该标签冒称产品完整PASS。
- 当前 normal23/23 exit0/112.59s 记录在 freeze；current69=false，未新跑原verify三模式，旧69输入不外推当前二进制。

## 亲看原图与交叉核验

| 原图 | 实际可确认范围 |
|---|---|
| 000–002 | 紧凑登录、默认聊天及980×640聊天：品牌/gear/字段/导航/消息/附件发送控件完整，无可确认裁切。未打开服务器设置、注册或错误登录。 |
| 003–005 | 真实联系人、新朋友入/出申请及1920×1080大窗口，不是误点后的重复Chat图。002树logical980×640对应X111960×1280；005对应3840×2160，在4096×2304原图内完整。 |
| 006–007 | 添加好友空状态和实际D搜索命中均有完整字段、行和按钮。没有发送新申请。 |
| 008 | 群资料标题在200%实际换成两行；两行、人数、公告、预览及底部链接/关闭仍分离且完整。换行本身不是裁切缺陷。 |
| 009–011 | 四成员全部显示；End后S005成员行、list及页签有focus状态与原图轮廓；一条pending申请按钮完整；管理页标题/公告/审批/邀请操作分离、关闭可见。邀请只读字段水平显示有限，有复制按钮，不据此判产品缺陷。没有提交、更改权限或压缩群弹窗。 |
| 012–013 | 搜索空状态及一个真实结果；结果focus轮廓可见。没有更早分页或结果复制操作。 |
| 014–015 | 自己资料含关闭/退出按钮；退出确认覆盖资料中段是正常模态遮挡，确认和取消均可见。操作记录随后Escape，后续仍为同一Qt客户端聊天状态。没有真正logout或重登录。 |
| 016 | 实际三行菜单完整可见；public tree 中menu/menu item为0。这是已知公共桥接菜单观察边界，不能将a11y-errors0理解为菜单可访问性完整PASS。Down/Down/Return后实际进入创建群聊；不依赖Home定位菜单首项。 |
| 017–018 | 0选中时Next disabled；C行实际checked=true、selected=true、focused=true、showing/ancestor_showing=true，Next enabled=true，与图一致。只有两个候选、一个选中，不据此证明三人建群确认或大列表。 |
| 019–020 | 命名modal logical520×300对应X111040×600；单成员行完整，Tab焦点轮廓可见。返回/再Next后群名称 ROLE_TEXT 同字节 `  页面矩阵 中文 é 🙂  `，两侧各两空格和decomposed e+acute均保留；qt-text实际equals同字节。未点击创建。 |
| 021–023 | 加入群聊空字段、输入invalid后的字段/加入/取消完整；022仅输入invalid，没有点击加入，不能称错误反馈已验。最后取消后回聊天。 |

焦点限制：树同时保留背景会话行或页签/list的focused标记，不能据此宣称全树唯一focus或所有focus契约完美；上述结论联合实际键盘操作、当前模态、ancestor_showing、rect及原图轮廓。

20次点击保留了logical→physical*2换算。例如联系人rect[9,136,64,64]→physical[82,336]，003真实进入联系人，避开先前125-r2未换算造成误点的driver缺陷。

## 清理与失败分类

此scope cleanup.json历史snapshot：status HANDOFF_NOT_PASS，10 owned及5 activation descendants全部gone=true，protected_unchanged=true，errors=[]；数据库chat_daily_6fa7e32094c2fdf0当时明确保留。该snapshot不覆盖为最终drop。

已独立只读复制并完整读取远端 `/tmp/chat-qt-matrix-driver.2y82nyss/final-database-cleanup.json`：8个exact scopes，其中7DB均connections_before_drop=0、dropdb_actual_exit=0、database_remaining=0；首轮200明确database_created=false。200-r2对应原retained DB同名。private_port_listener_remaining=0、protected_ticks_unchanged=true。此报告将该后续状态与scope历史snapshot分开，不将retained误称泄漏；这是读权威原记录，不是复跑SQL/drop。

首轮200 EADDRINUSE属于private bind preflight，在DB/client记录之前终止；保留失败证据，不计生产测试失败。125-r2坐标未按scale换算和初始Home菜单误定位同样是driver失败，不能用于产品RED。

下一验收缺口仍是未实际执行状态：完整服务器展开/注册/认证错误、ordinary群/权限加载失权、private tabs重授权、邀请/成员实际动作、3/20成员完整创建、实际非法/合法加入、direct/reply/edit/reaction/read/attachment chooser、实际logout/reconnect。不能用本次主要页面视觉可用子集代替这些业务及状态证据。
