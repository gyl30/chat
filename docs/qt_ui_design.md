# Qt 界面设计与验收规范

本规范描述 Chat 的 Qt Widgets 界面；主题与 UI 实现仍以本文链接的源码为准。设计目标是清晰、自然、适合长期聊天，优先保持键盘效率、用户上下文和业务语义。HTML 设计原型只用于比较方案，不能代替真实 Qt/X11 验收。

## 范围与权威来源

- 颜色与头像色板：[`daylight.json`](../qt/themes/daylight.json)、[`midnight.json`](../qt/themes/midnight.json)。
- 控件外观与状态：[`common.qss`](../qt/themes/common.qss)，追加对应的 `daylight.qss` / `midnight.qss`。
- 加载、校验、QPalette 与主题通知：[`theme_manager.cpp`](../qt/src/theme_manager.cpp)。
- 公用几何常量与确认动作：[`theme.hpp`](../qt/src/theme.hpp)、[`theme.cpp`](../qt/src/theme.cpp)。
- 自绘内容：[`conversation_delegate.cpp`](../qt/src/conversation_delegate.cpp)、[`message_delegate.cpp`](../qt/src/message_delegate.cpp)、[`user_delegate.cpp`](../qt/src/user_delegate.cpp)、[`avatar.cpp`](../qt/src/avatar.cpp)、[`icons.cpp`](../qt/src/icons.cpp)。
- 页面与生命周期：[`chat_widget.cpp`](../qt/src/chat_widget.cpp)、[`settings_dialog.cpp`](../qt/src/settings_dialog.cpp)、[`group_dialog.cpp`](../qt/src/group_dialog.cpp)、[`main_window.cpp`](../qt/src/main_window.cpp)。

不得另建主题管理器、平行颜色表或第二套好友状态。不得为纯视觉问题修改数据库、协议、SDK、服务端或 TUI。视觉建议必须与已实现行为分开说明。

## 颜色及主题加载

### 一个颜色来源

两份主题 JSON 各提供 42 个必需语义颜色及 `avatars` 色板。QSS 用 `${role}` 引用颜色，`${role.rgb}` 用于已有 alpha 表达；QPalette、QPainter 和 SVG 取同一份主题数据。修改配色只改主题文件，不在 C++ 里增加固定 HEX，不把相同颜色复制进 QSS。

| 职责 | 使用的现有 role |
|---|---|
| 背景与表面 | `canvas`、`panel`、`surface` |
| 经过与选中 | `subtle`、`canvas_hover`、`selected` |
| 文字层级 | `text`、`text_secondary`、`text_muted` |
| 主操作 | `accent`、`accent_hover`、`accent_pressed`、`accent_disabled`、`on_accent` |
| 控件边界与焦点 | `input_border`、`input_hover`、`border_strong`、`focus` |
| 装饰分隔线 | `border`；不承担唯一的控件识别职责 |
| 消息 | `bubble_in`、`bubble_out`、`message_sender`、`message_muted`、`message_focus` |
| 回复、反应与日期 | 回复沿用正文；反应用 `reaction_own` / `reaction_text`，日期用 `day_pill` |
| 未读、在线、已读、链接 | `highlight` / `badge_text`、`online`、`read`、`link` |
| 危险动作 | `danger`、`danger_hover`、`danger_soft` |
| 头像 | `avatars` 数组、`avatar_text` |

该表是职责索引，不是另一个运行时 token 系统。保留角色中的 `nav*` 不表示恢复永久导航栏；不能仅因名称与当前页面不同而删除必需角色。

### 加载与切换

1. 开发构建读取可执行文件旁的 `themes/`；安装读取 CMake 约定的安装主题目录，不依赖当前工作目录或个人源码绝对路径。
2. 用户 `QStandardPaths::AppConfigLocation/themes/` 可覆盖同名 Daylight/Midnight JSON 与 QSS；共享 `common.qss` 仍来自程序包。
3. JSON、角色、头像色板、占位符和完整 QSS 校验成功后整体应用。用户覆盖无法加载时清楚报告，并重新加载同名完整打包主题；不保留损坏覆盖中已读取的部分数据。两次加载都失败时保留原有有效主题，不改变 appearance 或保存设置；启动时若没有任何有效主题，明确报错并退出。
4. `theme_manager::changed` 刷新 QSS、QPalette、自绘视图与 SVG。现有临时 SVG 路径用于避免指示器沿用旧颜色，不另外增加图标缓存体系。
5. 设置通过 `appearance/mode` 保存 `system` / `light` / `dark`；跟随系统继续使用 Qt 的颜色方案通知。重启读取外部主题修改，无文件监控线程。

普通文字以 4.5:1 为验收目标；承担识别职责的图标、边界和焦点以 3:1 为目标。装饰线、禁用控件和非必要装饰不可机械套用同一阈值。根据实际前景/背景组合计算，不仅比较两个 palette 单值。

## QSS、自绘与图标的职责

QSS 管理原生控件的字体、边框、表面及交互状态；Qt Layout 管理组件排列，不用 QSS 宽度约束覆盖原生确认框的文本度量。Delegate 的 QPainter 负责头像、会话行、消息气泡及其内容几何，通过 `themed()` 取色；修改字号必须核对绘制与 `sizeHint()`，不能只改变外部字体值。

图标复用 `qt/resources/icons/` 的 SVG 与 `svg_icon()`，颜色取已有语义 role，订阅现有主题刷新。不得用 Unicode 字符替换操作图标，也不增加独立图标色表、缓存或绘制线程。对象名及动态属性仅用于定位实际控件样式，不承担新的业务状态。

## 独立设计交付物

工作区外的 `chat-design-system/` 交付物包含 `README.md`、颜色/字体/间距规范、组件规范、交互规范及四个 HTML 原型。先用 DS 编号定位审查候选，再对照当前源码和同场景 Qt/X11 截图；HTML 中不同的边距、引用卡片或控件形态并不自动成为产品缺陷。

这些材料是可选的设计讨论参考，不是 Qt 运行时资源或新的颜色权威。本文的关键规则、相对源码链接和验收矩阵可以独立阅读，不依赖个人机器路径或原型归档。

## 字体与几何

尺寸为 Qt 逻辑像素；不按缩放比例手工重复乘算。字体继承当前 Qt 平台字体和已有 Emoji fallback，不为品牌新增字体依赖或强制全局等宽。

| 层级 | 当前常用尺寸 | 使用边界 |
|---|---:|---|
| 资料身份标题 | 22，600 | 用户名、群名称 |
| 弹窗标题 | 20，600–700 | 设置、联系人、申请标题 |
| 聊天标题 | 17，700 | 当前会话名称 |
| 设置账号 / 分类 | 16，600 / 15 | 不替代普通正文 |
| 正文、列表名称 | 14；列表名称加粗 | 消息和常用控件 |
| 辅助信息 | 常用 13；继承平台字体的普通字重 | 时间、已读、成员数、会话预览、状态说明；发送者与日期等已有加粗保留 |
| 数字徽标 | 11–12，加粗 | 未读与收到申请，仍需真实可读 |

长用户名使用现有省略与字体度量，资料与错误说明按需换行；不得按 UTF-8 字节数计算字符宽度。对消息字体的修改必须同步核对 `sizeHint()`、QTextLayout、时间及已读宽度。

| 区域 | 当前几何 | 约束 |
|---|---|---|
| 登录 | 380×540，头像 88 | 不将服务器配置提高到凭据同等层级 |
| 主窗口 | 默认 1180×760，最小 980×640 | 双栏：332 宽会话列表 + 聊天，无永久导航栏 |
| 会话 / 联系人行 | 62 高，头像 46 | 名称 14、辅助 13，无水平滚动条 |
| 消息 | 气泡最大 430，头像 33，侧边距 16，内边距 11×8 | 不因宽屏无限扩宽；同发送者紧凑连接 |
| 消息形状 | 常规圆角 16，连接圆角 5 | 焦点沿实际气泡路径，不描整行长矩形 |
| 输入区域 | 基础高度 56，工具 44×46，输入最小 36 | 多行不挤掉必要的导航与消息阅读区 |
| 联系人 | 宽 420；首页高度按内容在 240–480 间取值；申请页默认高 480；最小 360×240 | 数据决定首页高度，列表可滚动 |
| 设置 / 群资料 | 默认 520×580，最小 420×400 | 设置 header 控件间距 8，返回按钮与标题分离；少内容不加占位卡片，多内容可访问完整功能 |
| 常用弹窗边距 | 公用 24，间距 12 | 特定页已有更紧凑 header，勿机械统一全部几何 |

有限节奏优先复用 4 / 8 / 12 / 16 / 24 等已有间距；头像、气泡尾部和文字度量可以有明确用途的专用值。不得仅为数字整齐重写所有布局。

## 控件状态与键盘

| 状态 | 视觉职责 | 操作要求 |
|---|---|---|
| Normal | 可分辨的输入区、按钮、列表内容 | 次要动作默认可见，名称与 tooltip 明确 |
| Hover | 较轻的表面反馈 | 不等同当前选中，不改变选中对象 |
| Pressed | 明确的短时按下反馈 | 主操作使用 `accent_pressed`；发送、资料主操作及头像上传均有专属 pressed，避免通用规则被 #id 优先级覆盖；不得令文本或图标消失 |
| Selected | 稳定标识当前对象 | 列表填充与现有强调标记配合；不只依赖颜色 |
| Focus | 可见的键盘操作目标 | 按钮 / 输入有边界，消息沿实际气泡描边；不删除焦点绘制 |
| Disabled | 明确不可操作 | 不仅变色；真实 enabled 状态阻止重复请求 |
| Loading / Error | 清楚说明进度或失败原因 | 失败保留输入、已有数据和草稿，不伪装为空列表 |

Tab / Shift+Tab 顺序应自然；Space / Enter 能激活可操作按钮；方向键在列表中选择；Esc 返回或关闭当前层级。确认危险动作默认取消，Esc 取消，文案完整可读。自定义按钮不能依靠鼠标 hover 才可识别。

消息发送快捷键保持设置的既有语义：默认 Enter 发送、Shift+Enter 换行；Ctrl+Enter 模式中 Enter 换行。中文预编辑交给 Qt 提交，不误发送消息。不得使回复、编辑、附件、复制和消息键盘导航回归。

## 页面组件约束

### 聊天与消息

主菜单负责全局入口，双栏聊天始终保留会话与草稿。会话 hover 弱于 selected，未读数字独立于 presence。消息正文为主，时间与已读为辅助；incoming/outgoing 一眼可分，无大阴影、玻璃或装饰动画。当前 Delegate 对 incoming 消息组首条显示发送者名、组末条显示头像，并没有按 direct / group 区分这两项绘制；本规范记录现状，不在本轮提出新的消息排版变更。当前回复是在正文前显示引用文字，不把 HTML 引用卡片误写为已实现能力。

主题切换、打开联系人 / 设置 / 资料和取消动作不得清空消息、滚动位置、当前会话、消息选择或输入。只在用户明确选择另一个联系人时切换聊天。

### 联系人、添加联系人和申请

联系人是独立弹窗，只来自 accepted contacts；本地搜索不改变申请入口。行点击 / Enter 进入聊天，头像打开资料，两者不能重复触发打开会话。

申请与联系人数据分开：无 incoming / outgoing 时不显示申请入口、不保留空行；仅 incoming 计入红色提醒，outgoing 显示等待确认。申请详情 Esc 先回联系人首页，首页 Esc 关闭。接受 / 拒绝 / 撤回失败保留申请与错误；刷新保留合理选择、焦点和滚动。

pending 不获得 direct 或 presence 权限；shared group 不自动成为好友。非好友 direct 从 Chats 隐藏，历史数据仍保留，再次接受恢复原会话。样式不能为上述规则建立客户端替代权限模型。

### 设置、资料与确认

设置集中管理应用偏好；我的账号只负责用户名、头像与资料。设置当前使用 Qt QCheckBox 与外观 QComboBox，不为参考产品改成额外开关组件体系。子页返回按钮与标题保留 8px 间距，键盘 focus 不紧贴标题；返回和 Esc 有确定含义。联系人与设置现有标题字重保持，不仅为“统一”调整整套字体。

群资料按身份、信息、成员、管理区组织，多成员列表独立可滚动；危险动作与普通管理分组。保留权限判断，不填充无意义卡片。

确认正文允许长中文和换行，按钮文字不得截断；使用 QMessageBox 的内容度量与换行，不给 `qt_msgbox_label` 叠加互相制约的固定 min/max 宽度。无法读取完整文案是功能可用性缺陷，不是可接受的审美差异。通知开关和预览隐私、托盘退出 / 隐藏 / 退出登录继续使用已有实现。

## 变更验收矩阵

测试名称以 `ctest -N` 为准；完整构建与数据库安全要求见 [`verification.md`](verification.md)。每项结果区分当前实际执行、历史记录与未执行。原型、offscreen 绘制或截图文件存在均不能单独作为真实桌面验收。

| 维度 | 必须核对的场景 | 可复核依据 |
|---|---|---|
| 主题与状态 | Daylight / Midnight；normal、hover、pressed、focus、selected、disabled | JSON 对比度计算 + 控件测试 + 原生截图 |
| 窗口 / DPI | 980×640、1180×760、125%；涉及布局时连续 resize | 真实 X11 / Xvfb Qt 窗口，不能低于最小尺寸 |
| 消息 | 短长文本、中文、Emoji、reply、edit、reaction、附件、时间 / 已读 | Delegate / UI 回归 + 实际发送接收 |
| 联系人 / 申请 | 无申请、incoming、outgoing、两者都有、最后一条处理、长列表滚动 | 真实好友状态与 UI 操作，不由 SQL 写消息冒充操作 |
| 弹窗 / 焦点 | 键盘进入、返回 / 关闭、长确认文案、取消、反复打开 | Qt UI + X11；取消不发送业务请求 |
| 上下文 | 当前会话、草稿、消息选择、滚动；主题来回切换 | 确定性状态测试 + 原生操作 |
| 外部主题 | 修改 QSS / JSON，重启生效；二进制 SHA256 不变；损坏文件 | 只改隔离的用户覆盖目录，记录错误及完整有效主题 |
| 可靠性 | 弹窗关闭后回调、重复点击、断线 / 恢复 | 受影响 Qt ASan / UBSan、定向测试 |
| 集成 | 主菜单、联系人、申请、设置、群资料互通 | 完整 Normal CTest + 小规模原生 Qt/X11 smoke |

视觉证据与运行日志留在 Git 工作区外。源码只提交明确相关文件，禁止连带提交截图、数据库、敏感设置、构建产物或无关历史 docs。验收必须报告尚未执行的项目；不放宽断言、跳过失败或使用 suppression 制造 PASS。
