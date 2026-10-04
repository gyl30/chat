# Qt / TUI 体验品质审查

本轮 BASE_HEAD：`75fb9a0292a3891fcec84982579e0fe1c346d999`。
目标覆盖桌面 Qt Widgets 和 FTXUI 终端端，不改变好友、群权限或消息语义。
内部评分是本项目的审查工具，不代表任何奖项的官方评分或获奖承诺。
本文件持续记录真实证据、取舍和审查结果；当前尚未达到退出品质循环的条件。

## 参考审查（2026-10-04）

### 证据强度

| 参考 | 实际核验的材料 | 边界 |
| --- | --- | --- |
| Telegram Desktop | 官方翻译平台的桌面截图：登录、聊天列表、私聊、群和资料；下载并查看了 29 张图片 | 图库混有历史和近期界面，不能将每张图片都标成当前版本；不是本机登录运行 |
| QQ Windows / NT | 官方下载页的桌面宣传合成图；2025-09 的 9.9.22 模式对比图 | 对比图本身也是展示合成图；未获得足以证明当前最新构建的完整运行截图 |
| 微信 Windows | 官方 4.1.15 下载和更新页；作者发布的 4.1.8 新旧运行界面对比 | 4.1.8 图片是非官方旁证，不能代表 4.1.15 的所有状态 |
| Webby | 官方评审标准和获奖作品记录 | 区分网站、软件与沉浸式作品；本项目自行制定评分权重 |
| Awwwards | 官方指南、作品评审页面及只读参考调查 | 移动网页建议不机械套用到桌面窗口或终端；本轮重新下载 PDF 遇到网络失败 |
| FWA | 官方作品平台与公开材料 | 动态网页未提供可核验的逐项评分公式，不编造官方权重 |

Telegram 的原始图片来自官方
[登录](https://translations.telegram.org/en/tdesktop/login/?mode=screenshots)、
[会话列表](https://translations.telegram.org/en/tdesktop/chat_list/?mode=screenshots)、
[聊天](https://translations.telegram.org/en/tdesktop/private_chats/?mode=screenshots)、
[群](https://translations.telegram.org/en/tdesktop/groups_and_channels/?mode=screenshots)和
[资料](https://translations.telegram.org/en/tdesktop/profile/?mode=screenshots)图库。
登录的高级入口低于主要身份操作，聊天区把列表、正文、输入明确分区，
资料把身份、常用操作和详细信息分层；这些是对实际图片的观察，不移植其业务规则。

QQ 材料来自[官方下载页](https://im.qq.com/download)和
[9.9.22 介绍](https://www.vgover.com/news/181045)。
微信的当前版本依据[官方下载页](https://pc.weixin.qq.com/)和
[4.1.15 更新](https://weixin.qq.com/updates?platform=windows&version=4.1.15)，
运行界面对比来自[4.1.8 作者文章](https://www.sohu.com/a/996230784_120914897)。
已看到低重量的图标导航、明确的选中态、列表与聊天分区、输入动作集中排列；
没有据此断言其登录、好友申请、重连或错误流程均已被覆盖。

### 状态覆盖

| 状态 | Telegram 已查看的直接证据 | QQ / 微信的现有证据 |
| --- | --- | --- |
| 登录、注册、身份错误 | 574、575、589、12112、19505、19506：登录输入、错误、验证码和替代登录方式 | 当前完整流程缺口 |
| 主窗口、导航、列表、搜索 | 991、1016、1181、19519、17330：列表、搜索、空聊天、窄窗口列表 | QQ 合成图；微信 4.1.8 运行对比 |
| direct / group、header、composer | 2339、18792、18793：消息、输入区、回复条及上下文动作 | 主窗口图可以观察区域分工，未核验完整交互 |
| profile、群资料、成员 | 835、2329、2374、19634、19480：身份、操作分层、成员及右键菜单 | 群侧栏只见于 QQ 展示图，profile 仍有缺口 |
| 创建群、选择成员、确认 | 832、833、2373：成员选择、名称输入和表单弹窗 | 当前直接证据缺口 |
| context menu、reaction、reply、edit | 18792、2329、19624、19625：菜单、回应选择、回复条 | 完整流程缺口，不能从主窗口图推断 |
| 入群申请、拒绝、权限错误 | 19183、2350：拒绝结果及无法发送提示 | 完整流程缺口 |
| empty、loading、offline、reconnect、附件详情 | 图库有空会话；其余尚不足以作完整状态证明 | 未核验完整流程 |

上表是研究覆盖边界，不是本产品的验收豁免。
本产品的每种状态都需要通过自己的真实使用和截图检查。

### 抽象出的设计原则

1. 常用路径稳定：主导航、列表、聊天、返回关系一眼可辨，不把所有功能摊在同一层。
2. 登录先表达产品与身份，服务器设置渐进展开，不抢主要操作的视觉焦点。
3. 空状态说明当前页缺少什么、下一步怎么做；加载、失败与离线有明确文字反馈。
4. 固定区域采用有限尺寸和间距体系，长文本、窄窗口和 HiDPI 仍能使用。
5. 选中、焦点、hover 和 disabled 有一致表达；关键含义不能只由颜色承担。
6. 消息正文先于时间、已编辑、已读等元数据；输入区默认轻量，多行时有界增长。
7. Qt 保留 cream / dark green 身份；TUI 按终端格宽和键盘习惯设计，不照搬 GUI。

[Webby 官方标准](https://www.webbyawards.com/judging-criteria/)把结构、导航、
视觉、功能、交互和整体体验一起审查；本项目据此把可操作性与截图品质共同作为门槛。
[How We Feel](https://winners.webbyawards.com/2023/apps-dapps-and-software/app-features/best-user-experience/243330/how-we-feel)
提供功能型产品参考，
[Reuters 的官方设计说明](https://www.thomsonreuters.com/en/press-releases/2018/july/reuters-puts-utility-at-heart-of-latest-news-app)
强调信息效用和使用场景。
[Awwwards 指南](https://www.awwwards.com/mobile-excellence-guidelines.pdf)和
[FWA](https://thefwa.com/)作为 craft 参考；本项目不为评审感堆动画或新增功能。

## 原始界面与运行基线

真实 Qt/X11：两个客户端、SDK 建立真实账号与群、隔离 PostgreSQL。
100 人群的资料、成员、好友请求、双向聊天、创建群和退出流程 4/4 通过。
另采集了 980×640、1180×760、1280×800、1440×900、1920×1080 的登录与主窗口，
以及 125%、150%、200% 的真实 xcb 登录截图。
这些尺寸已采集，不代表所有页面在所有尺寸下都已验收。

真实 TUI/tmux：两个及更多客户端的导航回归 11/11 通过，
保存 ANSI 样式 capture；额外采集了 60、70、80、100、120、160 列的登录、列表和聊天。
重新采集登录时通过可见的 Tab / Shift+Tab 焦点变化确认新帧，
弃用仅 resize 后立即截取的旧帧；ANSI 是原始证据，仓库以 gzip 无损保存，包括末尾空行和样式控制码；PNG 是便于评审的格宽渲染预览。

| 页面 | 保留的 before 证据 |
| --- | --- |
| Qt 登录 | [X11 截图](images/experience/before-qt-login.png) |
| Qt 注册错误 | [X11 截图](images/experience/before-qt-registration.png) |
| Qt 聊天 | [X11 截图](images/experience/before-qt-main.png) |
| Qt 消息菜单 | [X11 截图](images/experience/before-qt-menu.png) |
| TUI 登录 | [样式原文](images/experience/before-tui-login.ansi.gz)、[预览](images/experience/before-tui-login.png) |
| TUI 消息 | [样式原文](images/experience/before-tui-messages.ansi.gz)、[预览](images/experience/before-tui-messages.png) |

完整本地运行证据：

- `/tmp/chat-quality-before-qt-20261004`：100 人真实 Qt 流程。
- `/tmp/chat-quality-before-extra-qt-tabs-20261004`：Qt 页面、尺寸和 HiDPI 原图。
- `/tmp/chat-quality-before-complete-qt-20261004`：正文、图片和原始黑底菜单；后续坐标步骤失败，不能称整次运行通过。
- `/tmp/chat-quality-before-tui-20261004`：真实 TUI 11 项回归及样式 capture。
- `/tmp/chat-quality-before-login-tui-20261004`：确认新帧后的六档登录尺寸。
- `/tmp/chat-quality-before-extra-tui-retry-20261004`：六档列表/聊天和群、成员、申请、Help、Account。

## 初始问题清单

| 编号 | 严重度 | 已观察到的问题 | 处理状态 |
| --- | --- | --- | --- |
| Q01 | P1 | X11 消息右键菜单黑底深色字，关键操作无法清楚辨认 | 已修复并通过完整验证 |
| Q02 | P2 | Qt 登录的服务器配置先于身份；初始焦点也落在 URL | 已修复并通过完整验证 |
| Q03 | P2 | Qt 输入框仍是单行 QLineEdit，不能提供目标要求的多行有界增长 | 已修复并通过完整验证 |
| Q04 | P2 | Qt 新朋友列表仍有默认控件选中样式和大片分隔空白，与联系人行节奏不一致 | 未处理 |
| Q05 | P2 | Qt 群资料/管理界面表单和等宽文字按钮密集，信息和操作缺少分层 | 未处理 |
| T01 | P2 | TUI 登录 URL 为首要焦点，按钮各自带框，用户名规则长期占一整行 | 已修复并通过完整验证 |
| T02 | P2 | TUI 空消息、空申请等不同页面均出现泛化的 No items | 未处理 |
| T03 | P2 | TUI 长页眉和操作提示在窄屏缺少清楚的摘要层级 | 未处理 |

键盘打开列表、Qt 多行编辑、长公告、长 username、dialog 滚动、selection/focus、
terminal light/dark、combining、SSH 和 suspend/restore 仍需进一步实际核验。
没有把尚未复现的风险写成确定 bug，也没有把未知项记为 PASS。

## 初始内部评分

这只是基线审查，不计入要求的连续两轮 fresh review。

| 维度 | 满分 | Qt | TUI |
| --- | ---: | ---: | ---: |
| Visual hierarchy & craft | 20 | 10 | 9 |
| Usability | 15 | 10 | 9 |
| Structure & navigation | 10 | 7 | 7 |
| Interaction & feedback | 10 | 5 | 6 |
| Functional execution | 10 | 9 | 9 |
| Consistency & system coherence | 10 | 6 | 6 |
| Responsive / adaptability | 10 | 6 | 6 |
| Accessibility / readability | 5 | 2 | 3 |
| Originality / product identity | 5 | 4 | 2 |
| Overall experience | 5 | 3 | 3 |
| 合计 | 100 | 62 | 60 |

已有功能和真实通信稳定，但菜单可读性、认证层级、跨页一致性与待验证的适配项
明显未达到 92 分门槛。后续不能仅凭 CTest 全绿提高评分。

## 第一轮修复：弹出菜单

根因是全局 QWidget 透明背景延伸到顶层 QMenu。
为菜单和子菜单提供明确的 cream 表面、边框、深色正文及绿色选中态；
不改消息动作或权限，不增加组件包装层。

回归覆盖未选中菜单背景、键盘选中态和 reaction 子菜单。
修复前 `--widgets-only` 在透明背景断言失败，修复后通过。
真实 X11 [after](images/experience/after-qt-menu.png)确认回复、回应、已读、编辑和删除可读。
真实 Qt 导航 4/4 通过。
完整 `tests/verify.sh`：normal 20/20（97.91 秒）、ASan 20/20（131.87 秒）、
UBSan 20/20（123.57 秒），无 suppression、跳过或 timeout 放宽。
额外 X11 使用覆盖正文、图片、回复条、编辑窗口、reaction 子菜单、附件查看、
搜索、资料、群页、重连和 HiDPI 登录，完成运行保存在
`/tmp/chat-quality-menu-full-after-20261004`。
本阶段只关闭 Q01；其余问题和连续两轮最终自审仍未完成。

## 第二轮修复：Qt 登录与注册

登录以 Chat 标记、简短说明、用户名和密码、全宽主操作、创建账号组织层级。
服务器地址仍绑定原来的连接字段，移入默认收起的“服务器设置”；
展开聚焦地址，收起回到用户名，不清除账号或连接地址。
注册与登录共用 420 像素宽、32 像素内边距、12 像素节奏，
表单标签位于字段上方，主操作和返回区分权重。
焦点、hover、pressed、disabled 和字段 accessible name 有明确表达。
没有增加新的登录状态、协议或业务规则，注册成功仍保留原有确认流程。

真实 X11 对比：
[登录 before](images/experience/before-qt-login.png) /
[after](images/experience/after-qt-login.png)，
[注册 before](images/experience/before-qt-registration.png) /
[after](images/experience/after-qt-registration.png)。
[980×640 展开服务器设置](images/experience/after-qt-login-settings.png)无裁切。
实际采集 980×640、1180×760、1280×800、1440×900、1920×1080，
125%、150%、200% 的登录和注册均可用。

回归先在旧实现的“用户名应获得初始焦点”断言失败，再在新实现通过。
覆盖 Tab、设置展开/收起、地址和身份保留、空参数内联错误与五档布局。
真实键盘注册覆盖非法用户名拒绝、合法账号写入隔离数据库及成功返回登录。
X11 导航的好友处理、返回、建群、退出 4/4 通过；
证据在 `/tmp/chat-quality-auth-after-qt-complete-20261004`。
两次早期运行因驱动未处理已有注册成功确认弹窗而失败，未作为整次 PASS；
对应自有进程和隔离数据库均已清理。

`tests/verify.sh`：normal 20/20（96.68 秒）、ASan 20/20（129.13 秒）、
UBSan 20/20（123.90 秒），无编译警告、suppression、跳过或 timeout 放宽。
基于现有全界面基线和本轮认证改进，Qt 暂评 71/100：
12/20、11/15、7/10、7/10、9/10、7/10、7/10、3/5、4/5、4/5；TUI 仍为 60/100。
这是阶段复核，不能替代最终连续两轮完整 fresh review。
本阶段只关闭 Q02，聊天输入、联系人/群页一致性、终端认证和跨页状态仍需继续。

## 第三轮修复：终端登录层级

TUI 登录默认进入 Username，Password 后依次是 Log in、Create account 和 Server settings。
用默认终端前景、bold、dim、反色和 `>` 焦点标记区分层级与选择，移除每个按钮的独立边框。
用户名规则只在验证失败时显示，不长期占据首屏。
服务器设置使用现有 FTXUI Maybe，仅保存 UI 的展开布尔状态；
地址继续绑定 `app.server_url`，展开聚焦地址，Esc 收起并返回用户名。
未改注册、登录、重连、登出或 bracketed paste 的业务路径。
高度不足 20 行时减少说明与空白，错误反馈优先于辅助提示。

真实 tmux 样式对比：
[before](images/experience/before-tui-login.ansi.gz) /
[after](images/experience/after-tui-login.ansi.gz)，
对应 [before 预览](images/experience/before-tui-login.png) /
[after 预览](images/experience/after-tui-login.png)。
另保留 [60×20 展开设置](images/experience/after-tui-login-settings.ansi.gz)及
[40×12 错误](images/experience/after-tui-login-error.ansi.gz)；
PNG 为 ANSI 格宽预览，不是终端模拟器的原生像素截图。

先确认旧焦点和 URL 首屏在组件回归中 RED，再验证 Unicode、密码遮蔽、
展开编辑、Esc 收起及值保留 GREEN；编辑位置通过实际 End 键明确，不依赖输入框初始光标。
真实 tmux 在 40×12、60×12、60×20、70×24、80×24、100×30、120×40、160×45
采集收起和展开两种状态，用 Tab / Shift+Tab 的可见焦点变化确认 resize 后新帧。
注册通过原生键盘拒绝首尾 Unicode 空白，并创建中文、内部空格用户名。
后续日常导航、好友、消息、粘贴、重连、群和退出共 13/13 通过，
证据在 `/tmp/chat-quality-auth-after-tui-20261004`，自有进程与隔离数据库已清理。

`tests/verify.sh`：normal 20/20（95.70 秒）、ASan 20/20（129.81 秒）、
UBSan 20/20（123.16 秒），无编译警告、suppression、跳过或 timeout 放宽。
基于基线和本阶段认证改进，TUI 暂评 70/100：
12/20、11/15、7/10、8/10、9/10、7/10、7/10、3/5、2/5、4/5；Qt 仍为 71/100。
本阶段只关闭 T01，T02/T03、Qt 输入和跨页一致性、完整两轮 fresh review 仍未完成。


## 第四轮修复：桌面多行消息输入

输入区使用 QPlainTextEdit 保留纯文本与换行，默认单行高度，
按文档视觉行数和当前字体行高有界增长，更多内容在框内滚动。
文件和发送按钮保持在底部，宽度变化后重新采用原生文档布局结果。
Enter 发送，Shift+Enter 换行；空草稿禁用发送，输入与发送有 accessible name 和键盘提示。
中文输入法仍有 preedit 时，Enter 先确认输入法，不提前发送已提交的半段正文。
未修改 RPC、server、数据库、附件或已确认的权限规则。

回归先在旧单行控件失败，再验证多行粘贴、发送内容完整性、Shift+Enter、
高度上限、滚动、软换行 resize、离线草稿保留及 QInputMethodEvent 的确认边界。
初次实现的窄窗高度更新和输入法确认均由失败回归指出并修复。
原有重连用例以发送按钮充当连接探针；空草稿禁用发送后，
改为等待输入框可用和权威历史恢复，仍保留离线、权限快照和重连断言。
没有放宽 timeout 或删除业务断言。

真实 X11 两个 Qt 客户端用键盘发送、接收和粘贴多行中文/emoji，
PostgreSQL 只读核验完整 UTF-8 内容，导航、好友、建群与退出共 7/7 通过。
实际检查 980×640、1180×760、1280×800、1440×900、1920×1080 与连续 resize；
125%、150%、200% 缩放下输入区可用。原图位于
`/tmp/chat-quality-composer-after-qt-20261004`。
[单行 before](images/experience/before-qt-main.png) /
[多行 after](images/experience/after-qt-composer-multiline.png)，
[最小窗口长草稿](images/experience/after-qt-composer-minimum.png)，
[200% 输入区](images/experience/after-qt-composer-hidpi.png)。
TUI 未修改，重新运行真实 tmux 导航 11/11 通过，证据在
`/tmp/chat-quality-composer-tui-regression-20261004`。

完整 `tests/verify.sh`：normal 20/20（100.30 秒）、ASan 20/20（127.75 秒）、
UBSan 20/20（119.27 秒），无编译警告、suppression、跳过或 timeout 放宽。
Qt 暂评 74/100：12/20、12/15、7/10、8/10、9/10、7/10、8/10、3/5、4/5、4/5；
TUI 保持 70/100。这是阶段复核，未计入最终连续两轮 fresh review。
本阶段关闭 Q03，跨会话草稿和发送反馈继续实际核验，
联系人/群页一致性、终端空态、图标 HiDPI 与全流程最终审查仍未完成。
