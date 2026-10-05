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
| QQ Windows / NT | 官方配置确认 9.9.36（2026-09-24）；官方宣传合成图及 2025-09 的 9.9.22 对比图 | 版本事实不等于运行证据；仍缺当前构建的完整操作截图 |
| 微信 Windows | 官方 4.1.15（2026-09-15）下载和更新页；作者发布的 4.1.8 运行对比 | 4.1.8 图片是非官方旁证；当前 profile、群和错误流程仍有直接证据缺口 |
| Webby | 官方评审标准和获奖作品记录 | 区分网站、软件与沉浸式作品；本项目自行制定评分权重 |
| Awwwards | 官方指南、作品评审页面及只读参考调查 | 移动网页建议不机械套用到桌面窗口或终端；本轮重新下载 PDF 遇到网络失败 |
| FWA | 官方 case API、Ceramic Beats 实际运行与 CLEAR 前后截图 | Every Neuron 加载未完成；未提供可核验的逐项评分公式，不编造官方权重 |

Telegram 的原始图片来自官方
[登录](https://translations.telegram.org/en/tdesktop/login/?mode=screenshots)、
[会话列表](https://translations.telegram.org/en/tdesktop/chat_list/?mode=screenshots)、
[聊天](https://translations.telegram.org/en/tdesktop/private_chats/?mode=screenshots)、
[群](https://translations.telegram.org/en/tdesktop/groups_and_channels/?mode=screenshots)和
[资料](https://translations.telegram.org/en/tdesktop/profile/?mode=screenshots)图库。
登录的高级入口低于主要身份操作，聊天区把列表、正文、输入明确分区，
资料把身份、常用操作和详细信息分层；这些是对实际图片的观察，不移植其业务规则。

QQ 材料来自[官方下载页](https://im.qq.com/download)、
[官方当前配置](https://qq-web.cdn-go.cn/im.qq.com_new/latest/rainbow/pcConfig.json)和
[9.9.22 介绍](https://www.vgover.com/news/181045)。
2026-10-05 补查确认当前 Windows 9.9.36 于 2026-09-24 发布；
QQ 与微信的版本字段经实际官方响应核验，没有把发布说明或宣传合成图记作当前桌面操作验收。
微信的当前版本依据[官方下载页](https://pc.weixin.qq.com/)和
[4.1.15 更新](https://weixin.qq.com/updates?platform=windows&version=4.1.15)，
运行界面对比来自[4.1.8 作者文章](https://www.sohu.com/a/996230784_120914897)。
已看到低重量的图标导航、明确的选中态、列表与聊天分区、输入动作集中排列；
没有据此断言其登录、好友申请、重连或错误流程均已被覆盖。

### FWA 功能参考补证据（2026-10-05）

官方 case API 确认 [Ceramic Beats](https://thefwa.com/api/cases/ceramicbeats)
为 2026-09-22 FWA of the Day；[Every Neuron](https://thefwa.com/api/cases/every-neuron)
为 2026-09-20 FWA of the Day。这是作品记录，不代表本项目获奖或使用其官方评分。

实际 [Ceramic Beats](https://ceramic-beats.zui.ooo/) 用 Chrome 153.0.8010.52、
Playwright 1.61.1、1440×900 headless 浏览器加载，主代理亲自复核原始截图。
真实点击 CLEAR 的 [before](images/experience/reference-fwa-ceramicbeats-before-clear.png) /
[after](images/experience/reference-fwa-ceramicbeats-after-clear.png) 保留页面布局，
样本网格清空，名称从 Kiln Floor 变为 untitled，底部计数从 21 变为 0。
可直接观察到常用操作、材料、网格、持续状态的分层，以及内容改变后仍稳定的操作位置。
借鉴清楚反馈和稳定层级，不移植材质装饰、播放语义或疏密比例到聊天列表。
本次仅完成 CLEAR，不宣称播放、听音或保存流程已验收。

[Every Neuron](https://everyneuron.com/) 返回 200，但仍停在白色页面和 0 加载进度，
字体就绪等待也出现超时；后续 DOM 中出现导航文字不足以证明真实导航完成。
其交互不记为通过。运行证据在 `/tmp/chat-quality-fwa-runtime-20261005`。
QQ/微信当前完整运行状态、Awwwards PDF 重试等原有缺口仍存在。

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
| Q04 | P2 | Qt 新朋友列表仍有默认控件选中样式和大片分隔空白，与联系人行节奏不一致 | 已修复并通过完整验证 |
| Q05 | P2 | Qt 群资料/管理界面表单和等宽文字按钮密集，信息和操作缺少分层 | 已修复；第七轮真实 X11 与键盘复核 |
| T01 | P2 | TUI 登录 URL 为首要焦点，按钮各自带框，用户名规则长期占一整行 | 已修复并通过完整验证 |
| T02 | P2 | TUI 空消息、空申请等不同页面均出现泛化的 No items | 第八轮修复五处已复现空态 |
| T03 | P2 | TUI 长页眉和操作提示在窄屏缺少清楚的摘要层级 | 第八、十一轮修复页眉与长身份，六档宽度/两种背景原生复核；其他页面继续审查 |

后续真实使用新增的确定问题：

| 编号 | 严重度 | 已观察到的问题 | 处理状态 |
| --- | --- | --- | --- |
| Q06 | P1 | Qt 切换会话时沿用同一输入框文字，草稿可能带到另一接收方 | 已修复并通过完整验证 |
| Q07 | P1 | Qt 提交请求即清空草稿，服务端拒绝或断线时丢失尚未确认的正文 | 已修复并通过完整验证 |
| Q08 | P2 | Qt composer 按字体指标估算行高，中文多行恢复后发生不必要滚动并遮住前行 | 已修复并通过完整验证 |
| Q09 | P2 | 200% 下预渲染首字头像、真实头像和 SVG 图标密度不足，边缘锯齿或细节模糊 | 第九轮修复并通过完整验证及四档真实截图 |
| Q10 | P2 | 点击部分头像会连续打开两次资料，或关闭资料后继续打开原行聊天 | 第九轮实际鼠标事件回归修复并通过完整验证 |
| Q11 | P2 | 用户/账号资料重复显示身份，主要/复制/退出操作权重接近，按钮块和宽度偏大 | 第十轮修复并通过完整验证和真实四档缩放/导航 |
| Q12 | P1 | 消息搜索输入框 Enter 触发默认关闭，聚焦分页也无法用 Enter 请求更早结果 | 第十二轮修复默认/自动默认按钮和键盘回归，真实四档缩放搜索通过 |
| Q13 | P2 | 图片 QLabel 的源尺寸阻止预览缩小；强制缩窗后裁掉边缘 | 第十二轮复用解码图片等比例适配显示区域，缩小/放大及真实保存通过 |
| Q14 | P2 | 创建/加入群和确认框仍有默认平台图标、英文按钮及不一致的操作权重 | 第十三轮收口布局、动作、键盘默认与选人标记；验证见下文 |
| Q15 | P2 | 消息编辑框过窄，默认不换行、英文等权按钮；Tab 替换选中正文 | 第十五轮修复；七场景回归、四档原图与完整验证收口 |
| Q16 | P1 | 编辑菜单或弹窗等待期间插入旧历史，普通行索引会漂移到另一条消息 | 第十五轮修复编辑身份/正文捕获并完整验证；其他菜单路径继续逐条核验 |
| T04 | P2 | FTXUI 对 ZWJ、宽字组合符和部分 emoji presentation 的格宽/输出不一致，真实终端可能错位或丢组合符 | 真实 DSR 与依赖源码确认；未修复，不计 Unicode 全矩阵通过 |
| T05 | P2 | Help 的命令列表横向裁掉，窄屏快捷键不换行；滚到底仍无法看到末尾命令 | 第十四轮按词换行与实际内容滚动；六档宽度、两类高度的原生证据收口 |

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

## 第五轮修复：桌面草稿归属与发送确认

本阶段从 `772f1cdadd929ec6333e5239ba8fc5d315e99830` 开始，工作区干净且与远端一致。
真实 X11 复现切换到历史只读会话后仍显示群草稿，以及超出传输上限后输入区被清空。
现有 Qt 组件回归确认跨会话草稿 RED；真实 server 的参数拒绝回归也确认原文丢失 RED。

只把非当前会话的正文保存到内存映射，当前草稿仍以编辑框为准。
切换时恢复对应正文并抑制程序赋值产生的 typing；关闭会话、权威成员移除和账号切换清理对应草稿。
每个会话记录正在等待的发送，保持输入可编辑并阻止重复提交。
成功确认只清除仍匹配本次正文和回复选择的当前草稿；后台确认只处理所属会话。
失败保留正文和当前回复选择，显示内联反馈；断线清理等待状态并保留正文，
若仍在等待结果则提示检查历史，重连不自动重发。
沿用 client bridge 的连接 generation，没有新增服务端草稿、协议、migration 或通用资源层。

真实截图进一步发现高度数字正确不等于正文完整可见：字体指标行高低于实际中文 fallback 排版，
且 QPlainTextEdit 的滚动范围还预留一个像素。
回归先确认短中文/emoji 草稿存在不必要的滚动 RED，再按 QTextLine 的实际行高和文档边距计算，
最多六行；使用原有文档布局和 updateRequest 信号排队更新，没有新增布局状态字段。
恢复后的两行正文和 200% 下的六行输入均重新查看实际 X11 原图。

真实证据：

- 草稿带错会话 [before](images/experience/before-qt-draft-cross-conversation.png) /
  恢复原会话 [after](images/experience/after-qt-draft-restored.png)。
- 断线丢稿 [before](images/experience/before-qt-draft-lost.png) /
  RPC 拒绝后保留正文 [after](images/experience/after-qt-draft-failure.png)。
- [迟到确认保留后来文字](images/experience/after-qt-draft-new-text.png)、
  [修正行高后的 200% 输入](images/experience/after-qt-composer-line-height-hidpi.png)。

两个真实 Qt 窗口的六个草稿场景通过，包括手动修正、切页、重复 Enter、
等待期间改写、断线/重连和手动发送。
竞争场景用真实 PostgreSQL conversation 行锁及 pg_stat_activity 确认请求已经等待，
然后切页或修改正文再释放，不靠 sleep 猜竞争顺序。
首次驱动误用 SDK 的单个 actor 参数调用批量 disconnect；修正后完整重跑六项，
该次部分结果没有被记为完整 PASS，自有进程与数据库已清理。
另重跑多行输入、五档窗口、连续 resize、三档缩放及原生导航七项通过。
TUI 未修改，真实 tmux 导航 11/11 通过。
原始证据分别位于 `/tmp/chat-quality-drafts-final-qt-20261005`、
`/tmp/chat-quality-drafts-composer-final-qt-20261005` 和
`/tmp/chat-quality-drafts-tui-regression-20261005`。

最终完整 `tests/verify.sh`：normal 20/20（98.19 秒）、ASan 20/20（129.67 秒）、
UBSan 20/20（124.03 秒），Qt/TUI 均启用；没有编译警告、suppression、跳过或 timeout 放宽。
`git diff --check` PASS；server、client library、SQL 和依赖未修改。
阶段评分暂维持 Qt 74/100、TUI 70/100，新增问题修复不替代完整全流程评审。
Q04/Q05、T02/T03、图标 HiDPI 和最终连续两轮 fresh review 仍未完成。


## 第六轮修复：桌面新的朋友

本阶段从 `66dac9cd7c77dcbed9eac16cd7ec824c9e7bbbd1` 开始，工作区干净且与远端一致。
真实 X11 复现两条申请各占半屏、默认蓝色选中行和 Enter 无法打开资料。
请求列表直接复用联系人 delegate 的 62 像素行、46 像素头像、姓名/状态层级与浅绿色选中态；
用 Qt 标准显示、状态和图片 role 传递数据，保留原有申请用户 ID 与资料动作。
收到/发出标题显示数量，空区隐藏；短列表按内容收口，长列表保持各自滚动。
仅添加两个实际标题控件引用，没有新增 model、manager、权限或加载状态。

头像缓存变化现在刷新对应请求行。权威申请刷新按用户 ID 保留选择，
保留正在浏览的滚动位置；可见选择继续可见，不把浏览中的用户强拉回旧选择。
键盘 Enter 打开原有资料流程，焦点使用轮廓，不只靠颜色。
真实长列表截图还发现缩小窗口会裁切键盘选择；viewport resize 后仅让有焦点的列表滚入当前行。
没有捕获已删除 item 指针或维护额外 resize 状态。

组件回归确认行节奏、头像刷新、选择丢失和焦点轮廓 RED 后修复。
现有 Qt UI 测试覆盖 80 条列表、五档窗口、刷新/错误/空态、Enter 和有焦点的 resize。
新增 resize 断言曾把 offscreen 资料窗口关闭后的未激活主窗口当作有焦点窗口；
明确恢复窗口与列表焦点后通过。没有放宽 timeout 或删除断言，临时诊断输出已删除。

真实证据：

- 请求布局 [before](images/experience/before-qt-friend-requests.png) /
  [after](images/experience/after-qt-friend-requests.png)。
- [下载完成后的真实头像](images/experience/after-qt-friend-request-avatar.png)、
  [200% 缩放](images/experience/after-qt-friend-requests-hidpi.png)。
- resize 裁切选择 [before](images/experience/before-qt-friend-requests-resize.png) /
  [保持选中行可见](images/experience/after-qt-friend-requests-resize.png)。

两个真实 Qt 窗口、申请实时刷新/头像/清除、五档尺寸、连续 resize、三档缩放/重新登录，
以及好友接受/移除/取消、建群和退出共 8/8 通过，证据在 `/tmp/chat-quality-friends-final-qt-20261005`。
另用 SDK 建立 20 条收到、5 条发出的真实申请，核验 Tab、End、Enter、资料身份与窗口缩小，
25 条申请仍保持 pending，证据在 `/tmp/chat-quality-friends-many-resize-final-qt-20261005`。
原生脚本的头像证据查询和 fixture 数量/连接假设先纠正再完整重跑，失败部分结果没有记为 PASS；
自有进程和失败的隔离数据库已清理，没有改测试账号的身份数据。
现有 100 人群原生流程 4/4、真实 TUI/tmux 11/11 也通过。

最终完整 `tests/verify.sh`：normal 20/20（97.06 秒）、ASan 20/20（130.31 秒）、
UBSan 20/20（123.44 秒），Qt/TUI 均启用，无编译警告、suppression、跳过或 timeout 放宽。
随后全量 clean build、诊断 fixture 和 client 严格警告检查均通过，警告/错误为零；
重建后的 CTest 20/20（101.00 秒）与 `git diff --check` PASS。
server、client library、SQL 和依赖未修改。

Qt 暂评 76/100：13/20、12/15、7/10、8/10、9/10、8/10、8/10、3/5、4/5、4/5；
TUI 保持 70/100。本阶段改善列表 craft 和跨页一致性，仍不计入最终连续两轮 fresh review。
Q05、T02/T03、图标 HiDPI、完整参考证据与全流程审查仍未完成。


## 第七轮修复：桌面群资料与管理

本阶段从 `f257deb93f7efabce4bb241ba7e34825ec15828a` 开始，工作区干净且与远端一致。
真实 X11 复核群概览、成员、申请和管理：旧表单在高窗口中拉开，大块留白与等权按钮削弱操作层级；
申请空页缺少说明，预览头像下载后不刷新，申请刷新会丢失正在查看的用户。

群身份采用头像、名称和人数层级，概览只显示最多三位成员并保留全部成员入口，不限制群人数。
群资料和管理采用原生 QScrollArea，小窗口可滚动到操作，不强制扩大窗口。
管理表单按名称、公告、邀请链接分组，主操作、次操作和危险操作明确区分；
概览只说明链接状态，完整 opaque token 仍在原有管理页，普通成员不可见。
未改权限、管理员上限、公告、邀请、审批、退出、移除或转让语义。

三处用户列表复用联系人 delegate 的行节奏、真实头像、姓名/角色和焦点轮廓。
Enter 或头像打开原有资料流程；头像缓存变化刷新对应用户行。
申请按 ID 保留正在查看的用户与浏览位置，空态明确说明且隐藏无效操作。
没有新增列表模型、状态框架、权限快照或通用资源层，新增成员字段都是实际控件引用。

组件与真实窗口复核继续发现：滚到成员末尾后缩窗会裁切键盘选择；
公告全文的只读正文默认不能进入键盘焦点，Tab 与 Ctrl+End 无法读到末尾。
两项均先补回归确认 RED，再分别使用 viewport resize 后滚入当前项、
只读正文的鼠标/键盘选择修复。全文正文保持只读，不改变公告内容。

真实证据：

- 群概览 [before](images/experience/before-qt-group-overview.png) /
  [after](images/experience/after-qt-group-overview.png)。
- 管理表单 [before](images/experience/before-qt-group-management.png) /
  [after](images/experience/after-qt-group-management.png)，
  [420×480 可滚动表单](images/experience/after-qt-group-management-minimum.png)。
- [末尾成员缩窗仍可见](images/experience/after-qt-group-members-resize.png)、
  [申请实时刷新保留身份](images/experience/after-qt-group-request-selection.png)。
- [键盘读取公告末尾](images/experience/after-qt-group-announcement-keyboard.png)、
  [200% 完成加载后的概览](images/experience/after-qt-group-hidpi.png)。

两个真实 Qt 客户端的七项流程通过：百人成员列表 End/缩窗/资料身份、
新增申请后的选择和资料身份、键盘通过/拒绝与真实 membership、头像更新/清除、
长中文/emoji 公告全文、420×480/720×820/连续 resize、普通成员与断线/重连、
125%/150%/200% 重新登录后恢复。
原图在 `/tmp/chat-quality-group-keyboard-final-qt-20261005`；
现有百人群原生流程 4/4、未修改的 TUI/tmux 回归 11/11 通过。
若干临时原生驱动先修正 fixture 身份、焦点、排序和 readiness 假设再完整重跑；
未把部分成功或加载中截图记作完整验收，失败的自有进程和隔离数据库均已清理。

最终完整 `tests/verify.sh`：normal 20/20（94.53 秒）、ASan 20/20（135.70 秒）、
UBSan 20/20（118.17 秒），Qt/TUI 均启用；无编译警告、suppression、跳过或 timeout 放宽。
`git diff --check` PASS。
server、client library、SQL 和依赖未修改，无 migration。
Qt 暂评 78/100：14/20、12/15、7/10、8/10、9/10、8/10、9/10、3/5、4/5、4/5；
TUI 保持 70/100。Q05 关闭；T02/T03、其他资料/弹窗一致性、图标 HiDPI、
完整参考证据与最终连续两轮 fresh review 仍未完成，本阶段不等同于整个 Goal 完成。


## 第八轮修复：终端空态与页眉层级

本阶段从 `870c9ab5bfbf42193e6e818d85257943c099e004` 开始，保留已完成的桌面群资料工作。
两个真实 TUI 和隔离 PostgreSQL 复现空会话、空历史、空入群申请、空用户搜索、空消息搜索；
五页原先均显示泛化的 `No items`。现在分别说明当前页缺少的内容，
空会话提供现有 `N` 入口，空消息搜索提供现有 `/` 入口，不新增动作。
文字说明当前显示内容，没有新增 loading 状态或把短暂空快照解释成永远没有历史。

搜索、好友申请、成员、联系人选择和复制页把页面标题与次级键盘提示分开。
64 字符搜索词在 60–160 列下不再挤掉复制入口；无入群申请时不提示通过/拒绝。
成员管理提示置于固定页眉，不随长名单滚出视野。
沿用原有 `preview_text`、滚动、选中和键盘模型，没有新增组件、权限状态或包装层。

真实样式证据以 ANSI 原文为准，gzip 无损保留；PNG 只是格宽评审预览，
其临时渲染字体未覆盖所有 emoji，不能据此宣称真实终端字体或浅色背景已完整验收。

| 页面 | before 预览 | after 预览 | 原始样式 |
| --- | --- | --- | --- |
| 空会话 | [before](images/experience/before-tui-empty-chats.png) | [after](images/experience/after-tui-empty-chats.png) | [before](images/experience/before-tui-empty-chats.ansi.gz) / [after](images/experience/after-tui-empty-chats.ansi.gz) |
| 空历史 | [before](images/experience/before-tui-empty-history.png) | [after](images/experience/after-tui-empty-history.png) | [before](images/experience/before-tui-empty-history.ansi.gz) / [after](images/experience/after-tui-empty-history.ansi.gz) |
| 空入群申请 | [before](images/experience/before-tui-empty-join-requests.png) | [after](images/experience/after-tui-empty-join-requests.png) | [before](images/experience/before-tui-empty-join-requests.ansi.gz) / [after](images/experience/after-tui-empty-join-requests.ansi.gz) |
| 空用户搜索 | [before](images/experience/before-tui-empty-user-search.png) | [after](images/experience/after-tui-empty-user-search.png) | [before](images/experience/before-tui-empty-user-search.ansi.gz) / [after](images/experience/after-tui-empty-user-search.ansi.gz) |
| 空消息搜索 | [before](images/experience/before-tui-empty-message-search.png) | [after](images/experience/after-tui-empty-message-search.png) | [before](images/experience/before-tui-empty-message-search.ansi.gz) / [after](images/experience/after-tui-empty-message-search.ansi.gz) |

[60 列真实搜索结果](images/experience/after-tui-search-narrow.png)及其
[原始样式](images/experience/after-tui-search-narrow.ansi.gz)显示长查询、操作提示与正文独立。
现有 render 测试逐项确认 RED 后 GREEN，没有新增测试 executable 或删除原业务断言。
TUI 定向 CTest 5/5，通过已有导航的真实 tmux 11/11、未修改 Qt 的 X11 导航 4/4。
新增原生流程 7/7：五处空态、真实发送后六档搜索/复制/返回、空会话的新建菜单/返回。
两次驱动把跨行单词当作缺失，修正为按正文格内容判断后完整重跑；
失败的部分结果没有计为 PASS，自有进程和隔离数据库已清理。
本阶段证据在 `/tmp/chat-quality-tui-empty-verified-after-20261005`、
`/tmp/chat-quality-tui-states-navigation-20261005` 与 `/tmp/chat-quality-tui-states-qt-regression-20261005`。

最终完整 `tests/verify.sh`：normal 20/20（97.76 秒）、ASan 20/20（132.49 秒）、
UBSan 20/20（122.70 秒），Qt/TUI 均启用；无编译警告、suppression、跳过或 timeout 放宽。
`git diff --check` PASS；server、client library、Qt、SQL 和依赖未修改，无 migration。
TUI 暂评 74/100：13/20、12/15、7/10、8/10、9/10、8/10、8/10、3/5、2/5、4/5；
Qt 保持 78/100。五处空态已修复，标题与操作层级的明确剪裁问题已修复；
长身份摘要、其他资料与弹窗、图标 HiDPI、完整参考证据、浅色/色深/终端字体矩阵
及最终连续两轮 fresh review 仍需继续，不把局部通过记为整个 Goal 完成。


## 第九轮修复：头像密度与资料点击

本阶段从 `a60c78b41e110b3e1ab3da50399d9342e369dddc` 开始。
两个真实 Qt 客户端在 100%、125%、150%、200% 下查看百人群概览、成员资料和自己的账号。
200% 原图确认预渲染首字头像与 SVG 图标锯齿，真实头像因 128 像素缓存丢失细节；
原有 delegate 直接绘制的 fallback 不需要改动。

头像及 SVG 各保留一个 DPR=2 的资源，由 Qt 按目标尺寸缩小，不引入多版本管理或自定义 icon engine。
真实头像缓存先按原有圆形绘制语义取中央方形，再缩至 208×208，
满足最大 104 逻辑像素头像在 200% 下的密度。
不是上传裁剪器；原始头像、传输、revision、重复下载和重连规则不变。
每个已缓存头像约 169 KiB RGBA，100 个约 16.5 MiB；不保留原图和多档位缓存。

依据 [Qt QIcon](https://doc.qt.io/qt-6/qicon.html) 和
[QPixmap DPR](https://doc.qt.io/qt-6/qpixmap.html#setDevicePixelRatio)，
测试检查足够的物理密度与不变的逻辑尺寸，不错误要求返回 DPR 必须等于请求 DPR。
本项目 Qt 6.2.4 的
[实际选择实现](https://raw.githubusercontent.com/qt/qtbase/v6.2.4/src/gui/image/qicon.cpp)
也说明为什么同时提供 1x/2x 会在 125% 选择密度不足的 1x；最终只保留一份 2x 资源。
公共绘制/缓存测试逐项确认 RED 后 GREEN；宽幅图片同时覆盖中心裁切后的细节。

真实点击还发现：群概览、收到/发出的申请、搜索头像绑定了两条打开资料路径；
关闭第一次资料会再次弹出。联系人与会话头像关闭后，还会继续执行原行聊天动作。
移除重复的整行/头像绑定；需要两个不同动作的联系人和会话，由现有 delegate 的命中区域
分别发出头像与正文事件。没有增加 dialog-open、busy、generation 等状态。
组件测试使用实际 viewport 的按下/释放，验证一次资料、关闭后不打开聊天、正文仍单击打开聊天；
集成测试也改用真实鼠标事件，保留好友确认、群权限、历史与生命周期断言。

原始截图仅截取实际 X11 窗口，没有重排或修饰：

- 200% 真实头像 [before](images/experience/before-qt-profile-hidpi-200.png) /
  [after](images/experience/after-qt-profile-hidpi-200.png)。
- 200% 首字头像 [before](images/experience/before-qt-account-hidpi-200.png) /
  [after](images/experience/after-qt-account-hidpi-200.png)。
- 200% 群概览 [before](images/experience/before-qt-group-hidpi-200.png) /
  [after](images/experience/after-qt-group-hidpi-200.png)。
- 真实资料 [100%](images/experience/after-qt-profile-hidpi-100.png)、
  [125%](images/experience/after-qt-profile-hidpi-125.png)、
  [150%](images/experience/after-qt-profile-hidpi-150.png)。
- 自己的账号 [125%](images/experience/after-qt-account-hidpi-125.png)、
  [150%](images/experience/after-qt-account-hidpi-150.png) 与
  [200% 主窗口](images/experience/after-qt-main-hidpi-200.png)。

本阶段四档原生流程通过，完成加载后查看资料并关闭，确认没有再次弹窗；
原有 Qt/X11 导航 4/4、未修改 TUI 的 tmux 导航 11/11 通过。
证据在 `/tmp/chat-quality-hidpi-avatar-after-qt-20261005`、
`/tmp/chat-quality-avatar-nav-after-qt-20261005`、`/tmp/chat-quality-avatar-tui-regression-20261005`。
此前失败采集既有驱动 readiness/焦点假设问题，也真实暴露了重复资料交互；
没有把失败的部分结果记作 PASS。所有本阶段自有进程和隔离数据库已清理。
本机缺少 at-spi bus 和两个 GTK 模块，原生日志如实保留；不是编译警告，也不代表无障碍已经完整验收。

最终完整 `tests/verify.sh`：normal 20/20（93.23 秒）、ASan 20/20（131.17 秒）、
UBSan 20/20（123.32 秒），Qt/TUI 均启用，无编译警告、suppression、跳过或 timeout 放宽。
`git diff --check` PASS。
Qt 暂评 80/100：15/20、12/15、7/10、9/10、9/10、8/10、9/10、3/5、4/5、4/5；
TUI 保持 74/100。
截图仍显示资料身份重复、操作权重接近；其他弹窗与资料整体语言还需继续真实评审。
长身份摘要、完整参考、终端字体/色深/浅色矩阵和连续两轮 fresh review 仍未完成。
本阶段不改 server、client library、SQL 或依赖，无 migration。


## 第十轮修复：用户与账号资料层级

本阶段从 `ab36058f312c35c4b47cb5288feb14a1514ff922` 开始。
真实 100–200% 窗口显示同一用户名在头像下和第二块信息区重复出现，
复制、消息和退出缺少明确权重；原宽度 590、132×78 操作块也比群资料更重。
移除重复身份和不用的样式；保留标题的鼠标/键盘选择、复制用户名及原业务操作。

资料宽度收口为 520，内边距 24、间距 12，头像仍为 104；
操作改为 160×40、20 像素 SVG 的图文按钮。
消息/好友确认和更换头像使用主要操作，复制为次级，移除/拒绝/取消/退出为安静的危险操作。
图标关闭有明确 accessible name 和 tooltip，主要/次级操作保留 hover、disabled 和焦点区别。
身份下显示账号、好友关系或现有 presence；已打开资料随现有 model/好友事件更新，
失去好友关系立即不显示 private presence。不增加请求、DTO、缓存或权限状态。

既有 Qt UI 入口依次补回归确认 RED，再修 identity、尺寸、操作权重、图标名称及关系/presence。
64 UTF-8 字节中文/emoji 身份在 980×640 父窗口中完整换行，资料不超 520×600；
头像传输、好友确认、历史、群权限和生命周期的旧断言保留。

- 用户资料 [before](images/experience/before-qt-profile-layout.png) /
  [after](images/experience/after-qt-profile-layout-normal.png)。
- 自己账号 [before](images/experience/before-qt-account-layout.png) /
  [after](images/experience/after-qt-account-layout-normal.png)。
- 资料 [125%](images/experience/after-qt-profile-layout-125.png)、
  [150%](images/experience/after-qt-profile-layout-150.png)、
  [200%](images/experience/after-qt-profile-layout-200.png)。
- 账号 [125%](images/experience/after-qt-account-layout-125.png)、
  [150%](images/experience/after-qt-account-layout-150.png)、
  [200%](images/experience/after-qt-account-layout-200.png)。

两个真实 Qt 四档加载、资料打开/关闭与真实 clipboard 复制通过。
原有 X11 导航 4/4 包含好友接受/移除、申请取消、返回、新建群以及退出取消/确认。
旧导航脚本仍点击旧版绝对坐标而失败；改为从实际窗口可见操作定位，原业务结果断言不变，
重新完整通过。没有用生产兜底或测试后门保留旧坐标。失败的自有数据库和进程已清理。
证据在 `/tmp/chat-quality-profile-layout-final-qt-20261005`、
`/tmp/chat-quality-profile-layout-nav-final-qt-20261005`。

最终完整 `tests/verify.sh`：normal 20/20（98.19 秒）、ASan 20/20（131.03 秒）、
UBSan 20/20（121.52 秒），Qt/TUI 均启用，无编译警告、suppression、跳过或 timeout 放宽。
未修改 TUI 的真实 tmux 导航 11/11 通过，证据在
`/tmp/chat-quality-profile-layout-tui-regression-20261005`。`git diff --check` PASS。
server、client library、TUI、SQL 和依赖不变，无 migration。
Qt 暂评 83/100：16/20、12/15、7/10、9/10、9/10、9/10、9/10、4/5、4/5、4/5；
TUI 保持 74/100。这不是最终两轮 fresh review；其余弹窗、完整键盘/终端矩阵和参考缺口继续保留。


## 第十一轮修复：终端长身份与摘要

本阶段从 `e2661351cc6082f7fa0a7d869e9ddb10305c28b5` 开始。
真实 xterm 中，合法的长中文群名会剪掉人数，宽字可能覆盖右边框；
64 字节身份在资料中被截断，成员角色被用户名挤掉，账号名与连接状态粘连。

复用已有 FTXUI glyph/cell 接口，在单行摘要处按格宽保留省略号；
群人数、成员角色和连接状态独立保留，会话侧栏为原滚动指示留一格。
资料页完整身份换行，正文、复制、数据库身份和传输内容不变。
没有新增解析器、状态、缓存或框架，也未改 Qt、server、client library、SQL 和依赖。

既有 render 入口逐项确认 RED 后 GREEN，覆盖六档群名、长资料、角色和滚动指示。
资料测试检查公共 Screen 中完整身份的字序，允许正常跨行；
早期只查找 `_END` 的测试误将跨行当作丢字，已纠正，没有为错误观察方式改变产品。

原生证据是实际 X11 xterm 窗口原图，同时保存 tmux styled ANSI，无重排或修饰：

- 长群名 [60 列 before](images/experience/before-tui-long-title-60.png) /
  [after](images/experience/after-tui-long-title-60.png)，
  [100 列 before](images/experience/before-tui-long-title-100.png) /
  [after](images/experience/after-tui-long-title-100.png)。
- 其余聊天尺寸：[浅色 70](images/experience/after-tui-long-title-light-70.png)、
  [80](images/experience/after-tui-long-title-80.png)、
  [浅色 120](images/experience/after-tui-long-title-light-120.png)、
  [160](images/experience/after-tui-long-title-160.png)。
- 完整资料 [before](images/experience/before-tui-long-profile-60.png) /
  [浅色 after](images/experience/after-tui-long-profile-light-60.png)。
- 成员角色 [before](images/experience/before-tui-long-members-60.png) /
  [after](images/experience/after-tui-long-members-60.png)。
- 账号身份 [before](images/experience/before-tui-long-account-60.png) /
  [after](images/experience/after-tui-long-account-60.png)。

群名 before 来自未修复的原生运行；资料、成员和账号 before 来自本轮首次迭代、
尚未修复完整身份/角色时保存的二进制。最终运行又通过真实注册和邀请链接加入一个
64 字节账号，因此群人数由早期的 4 变为 5；不是 UI 修复改变了成员语义。
两真实 TUI 双向聊天，第三个真实 TUI 注册、加入、查看账号，权威身份和 membership
经实际 SDK 核验。最后采集 31 张窗口，六档 60/70/80/100/120/160 列、24/30/40/45 行，
浅色/深色分别运行；60/100/160 的资料、成员和账号完成加载后检查。
xterm 372 使用 DejaVu Sans Mono 与 Lily Han Sans HW SC；tmux 3.6a 终端声明
256 色与 RGB 能力，应用仍用默认前景/背景和 dim/bold/inverted。
这不是所有终端字体、独立色深配置及 SSH/suspend 的完整验收。

基本 CJK、混合 ASCII、普通 emoji 和 Latin combining 在本轮可见。
另外真实 DSR 光标查询确认：FTXUI 7.0.3 把 `👩‍💻` 算作 5 格，
原始 xterm 算 4 格，tmux 中算 2 格；宽字后的组合符也可能在 FTXUI 输出时丢失。
[上游修复](https://github.com/ArthurSonzogni/FTXUI/commit/71c036681659f9ef964b17fc48e99bb2e85074fc)
处理宽字组合符及 U+FE0F，尚不能证明 ZWJ 或全部终端一致。
未更新依赖、未自造 Unicode 框架，T04 继续保留为确定问题。

最终完整 `tests/verify.sh`：normal 20/20（95.66 秒）、ASan 20/20（138.23 秒）、
UBSan 20/20（121.19 秒），Qt/TUI 均启用；无编译警告、suppression、跳过或 timeout 放宽。
原有真实 TUI 导航 11/11、Qt/X11 导航 4/4 通过。`git diff --check` PASS。
证据在 `/tmp/chat-quality-tui-title-final-20261005`、
`/tmp/chat-quality-tui-identity-nav-20261005`、`/tmp/chat-quality-tui-identity-qt-regression-20261005`。
所有本轮自有进程和隔离数据库已清理；失败驱动的加载等待、窗口定位和临时端口问题
也已核实，没有记作产品通过。无 migration。

TUI 暂评 77/100：14/20、12/15、7/10、8/10、9/10、9/10、9/10、3/5、2/5、4/5；
Qt 保持 83/100。复合 emoji、其他弹窗和全键盘/终端矩阵、完整参考证据与最终两轮
fresh review 仍未完成；本阶段通过不代表整个品质 Goal 完成。


## 第十二轮修复：消息搜索与附件弹窗

本阶段从 `c4bd20c9310de5feac77f645314bc9fb1ea2d859` 开始。
真实 X11 搜索框按 Enter 后窗口消失，窗口树确认它已关闭。
既有 Qt 入口先复现 RED，再将“搜索”设为主默认按钮，移除重复的 returnPressed 路径；
保留分页和关闭的自动默认行为，使聚焦按钮仍可 Enter，返回输入框又恢复搜索。
公共事件回归核对请求次数与 cursor，不为键盘另建 handler 或状态。
最初分页 fixture 漏填 conversation，被既有模型正确忽略；
修正 fixture 后重新确认原分页 Enter 行为 RED，再完成修复。

原预览依赖 QLabel 固定 pixmap 大小，正常 Qt resize 被源图尺寸限制；
原生强制缩至 420×360 后只能看到中间部分。
现让显示 QLabel 可收缩，既有 dialog 保留解码源 QPixmap，
数据就绪和显示区 resize 时平滑等比例缩放，不放大源图、不反复解码、paint 不做 IO。
源 pixmap 与缓存通过 Qt 隐式共享；窗口放大仍从源图缩放，保存继续使用原始 bytes。
没有资源管理器、多尺寸 cache 或额外生命周期标志。

两类大弹窗仍为 700×600；普通附件仍为 500×180。
复用既有 24 像素内边距和 12 像素间距，把分页/保存与关闭放入同一紧凑操作行。
搜索/保存明确为主要操作，关闭本地化并移除平台图标；hover、focus、disabled 复用现有配色。
消息搜索结果去掉默认列表边框，状态保持次级文字；业务和网络查询不变。

原图仅截取实际 X11 窗口，无重排、装饰或合成：

- 消息搜索 [before](images/experience/before-qt-message-search.png) /
  [after](images/experience/after-qt-message-search.png)。
- 空结果 [125%](images/experience/after-qt-message-search-empty-125.png)，
  匹配结果 [100%](images/experience/after-qt-message-search-results.png)、
  [150%](images/experience/after-qt-message-search-results-150.png)、
  [200%](images/experience/after-qt-message-search-results-200.png)。
- 图片预览 [before](images/experience/before-qt-message-preview.png) /
  [after](images/experience/after-qt-message-preview.png)，
  窄预览 [before](images/experience/before-qt-message-preview-small.png) /
  [after](images/experience/after-qt-message-preview-small.png)，
  [200%](images/experience/after-qt-message-preview-200.png)。
- [实际保存后的状态](images/experience/after-qt-message-preview-saved.png)。

before 几何采集使用本轮保存的二进制，搜索默认关闭的单行初步修复已在其中，
其他布局和预览尚未修改；Enter 错误来自此前单独的真实失败运行和公共回归，
不把 before 几何截图伪称为全部旧行为。
最终两个真实 Qt 完成链接入群、双向群聊、连续 Enter 空/匹配搜索，
四档 100/125/150/200% 与原文件保存，共保留 50 张实际窗口采集。
这不是 50 个独立测试，也不是全部页面尺寸组合验收。
GTK 保存框只选中了文件名主干，早期驱动留下 `.png.png` 后缀；
实际文件 bytes 完全相同，修正全选后完整重跑通过，未改产品保存逻辑。
原生文件选择器继续使用系统组件，没有为其另建界面。

现有 Qt UI 完整定向测试通过（37.47 秒），原有 Qt/X11 导航 4/4、TUI/tmux 11/11 通过。
最终原生证据在 `/tmp/chat-quality-dialog-final-qt-20261005`、
`/tmp/chat-quality-message-dialog-qt-nav-20261005`、
`/tmp/chat-quality-message-dialog-tui-regression-20261005`；自有进程和隔离数据库已清理。
完整 `tests/verify.sh`：normal 20/20（97.29 秒）、ASan 20/20（133.72 秒）、
UBSan 20/20（120.43 秒），Qt/TUI 均启用，无编译警告、suppression、跳过或 timeout 放宽。
`git diff --check` PASS。
Qt 暂评 86/100：17/20、13/15、7/10、10/10、9/10、9/10、9/10、4/5、4/5、4/5；
TUI 保持 77/100。创建/加入群、确认框、复合 emoji 和全键盘/终端矩阵、参考缺口
及最终连续两轮 fresh review 继续保留。本阶段不改 server、client library、TUI、SQL 或依赖，无 migration。

## 第十三轮：建群、加入与操作确认

延续第十二轮的真实原图，处理 Q14，不更改群创建、邀请审批、权限或联系人规则。
普通弹窗宽度采用 520，短表单采用 480，复用内容边距 24、间距 12。
选人页为 520×540，保留搜索、选择 chips、联系人和两步流程；命名页按内容收为 520×300，
以有界成员列表取代大片空白和可能撑大的姓名段落。列表保留完整文本、tooltip 和键盘滚动，
多选 20 人时可由 Tab 到达列表并用 End 查看尾部；焦点有边框，不把可滚动内容设为 NoFocus。
选中标记使用同族 SVG check，未选为空心边框，不能只靠颜色识别选择。

加入群继续使用真实 QInputDialog、完整邀请链接和原有错误处理；明确“加入 / 取消”、
字段 accessible name 与可读宽度，取消默认平台图标。六处危险操作共同使用具体动作名称，
保留 QMessageBox 和原有 Yes/No 决策，但用户看到的是“删除 / 移除联系人 / 退出登录 /
转让群主 / 移除成员 / 退出群聊”与“取消”。初始 Enter、Esc、关闭窗口均取消，
只有显式确认才执行业务；正文按 PlainText 显示，不把用户名当作富文本。
注册成功、错误通知及已有关闭条也使用明确中文动作，未增添新的确认步骤。

标准按钮设置对象名后原先仍保留已缓存的次级样式，真实截图和新增显示像素断言发现主动作不绿；
在完成按钮配置后刷新其样式。间距、主操作像素、多选键盘滚动均先复现 RED，再验证 GREEN。
六种确认的四种决策共 24 个公共事件回归；这不是 24 个独立 CTest target。

- 选人 [before](images/experience/before-qt-create-choice.png) / [after](images/experience/after-qt-create-choice.png)。
- 命名 [before](images/experience/before-qt-create-name.png) / [after](images/experience/after-qt-create-name.png)，
  [200%](images/experience/after-qt-create-name-200.png)，
  [20 人键盘滚到末尾](images/experience/after-qt-create-members-keyboard.png)。
- 加入 [before](images/experience/before-qt-join.png) / [after](images/experience/after-qt-join.png)。
- 退出确认 [before](images/experience/before-qt-logout-confirm.png) / [after](images/experience/after-qt-logout-confirm.png)。

本轮两个真实 Qt 在隔离库完成链接入群、群聊，100/125/150/200% 保留 44 张实际窗口采集，
包括选人、命名、加入、搜索及确认；不是全部页面或全部尺寸的验收。
原生证据在 `/tmp/chat-quality-create-keyboard-final-20261005`。
最初临时驱动假定 fixture 免审批，但 SDK fixture 已开启审批，因此入群返回 pending；
明确用 SDK 设定免审批场景后重跑，不改变产品规则。另两次旧坐标分别点到取消和错过图片；
先查看原图再调整驱动，图片保存的前一轮证据不冒充本轮重测结果。

独立 TUI 复审发现 T05：60/80/160 列和 24/45 行下，Help 的命令后半段不可见，
滚到底仍不能访问 `avatar-clear, logout, quit`；实际 xterm 原图与 tmux ANSI 在
`/tmp/chat-quality-help-audit-1r4t6svt`。FTXUI 的横向 frame 和未约束换行是直接原因，
将单独修复。T04 的宽字组合符落在保留格、ZWJ 与 FE0F 问题仍未解决，不能只改 UI 截断函数
就宣称端到端 Unicode 完成。本阶段不修改 TUI、server、client library、SQL 或 third，无 migration。

最终 Qt/X11 导航 4/4（`/tmp/chat-quality-create-qt-navigation-final-20261005`），
TUI/tmux 导航 11/11（`/tmp/chat-quality-create-tui-regression-20261005`）。
建群驱动旧的全体绿色像素聚合把次级按钮文字计入范围，点中了上一步；
改为实际窗口内连续主按钮填充区域，保留权威 SDK 群创建和成员断言后完整通过。
退出以默认 Enter 取消、Tab 明确选择确认、权威 presence 变化验证，不再猜绝对屏幕坐标。
完整 `tests/verify.sh`：normal 20/20（102.42 秒）、ASan 20/20（129.69 秒）、
UBSan 20/20（122.08 秒），Qt/TUI 均启用；无编译警告、sanitizer 报告、跳过、suppression 或 timeout 放宽。
最终日志 `/tmp/chat-q14-keyboard-verify-20261005.log`；自有进程和成功隔离库已清理，
失败运行证据保留用于说明驱动问题。`git diff --check` PASS。
Qt 暂评 87/100：18/20、13/15、7/10、10/10、9/10、9/10、9/10、4/5、4/5、4/5；
TUI 保持 77/100。这不是最终两轮 fresh review。
Help、复合字符、消息编辑弹窗及未闭环的全页面/键盘/参考/终端矩阵继续处理，不宣布达到退出门槛。

## 第十四轮：终端 Help 的完整阅读与滚动

以第十三轮独立复审的 T05 为依据，不更改命令、导航父子关系或业务规则。
旧 Help 的命令 paragraph 位于同时允许横向滚动的 frame 内，长段落没有按实际宽度换行；
快捷键也按单行 text 输出。固定的滚动项数又没有包括窄屏换行后的高度，
所以到达旧上限仍看不到后半段命令。最初按字硬换行能显示文字，但会把命令名拆在两行，
六档宽度断言仍有三档失败，不能算作完整可读。

现在 Help 的静态说明按完整单词和 FTXUI terminal cell width 换行，
同一 `help_content()` 同时供渲染与滚动范围计算，不保存另一份行数或滚动状态。
Help 与 Copy 的视口只沿纵向 frame，标题与操作提示固定，正文可用 j/k 阅读。
Copy 原有按 glyph 换行和消息字节处理不变；没有更换依赖或自造 Unicode 解析器。
这不是 T04 的修复，也不把 ZWJ、FE0F 和宽字组合符记作已通过。

- 短屏命令末尾 [before 60×24](images/experience/before-tui-help-60x24.png) /
  [after 60×24](images/experience/after-tui-help-60x24.png)。
- 宽屏 [before 160×45](images/experience/before-tui-help-160x45.png) /
  [after 160×45](images/experience/after-tui-help-160x45.png)。
- 六档真实宽度的其余原图：[60×45](images/experience/after-tui-help-60x45.png)、
  [70×45](images/experience/after-tui-help-70x45.png)、
  [80×45](images/experience/after-tui-help-80x45.png)、
  [100×45](images/experience/after-tui-help-100x45.png)、
  [120×45](images/experience/after-tui-help-120x45.png)。
- [80×24 键盘返回顶部](images/experience/after-tui-help-returned-top.png)。

自动回归包含六档宽度乘 24/45 行的 12 组真实 component 事件：
从 Contacts 进入 Help，200 次 j 到底，在 Commands 到 Clipboard 之间检查全部 33 个完整命令，
短屏首项确实滚出，再用 200 次 k 恢复原视口并用 Esc 返回 Contacts。
断言保留物理换行，不能把拆开的命令名拼接后算通过；这些是既有 tui_render target 的断言，
不是 12 个新增 CTest target。原有 Ctrl+C、Copy、Unicode、消息导航等回归保留。

原生 xterm / tmux 测试覆盖 60/70/80/100/120/160×45 及 60/80×24，
每档保存顶部、j 到底、k 返回与 Esc 返回 Chats，共 32 组 PNG、ANSI、文本。
原图没有重排或合成；主代理和独立代理亲自检查短屏、窄长屏与宽屏，
命令不再截断或拆词，正文、滚动条、标题与 footer 没有覆盖。
原生输出 `/tmp/chat-quality-help-after-native-20261005`，result 为 PASS，errors 为空；
旧 before 只有 60/80/160 三档宽度，不伪称已收集六档 before。
测试只使用自有数据库、端口、tmux socket 和进程，结束后数据库已删除。

Qt/X11 导航 4/4、TUI/tmux 导航 11/11 通过，证据分别在
`/tmp/chat-quality-help-qt-navigation-20261005`、
`/tmp/chat-quality-help-tui-regression-20261005`；其自有进程与成功隔离库已清理。
完整 `tests/verify.sh`：normal 20/20（95.52 秒）、ASan 20/20（132.50 秒）、
UBSan 20/20（124.60 秒），Qt/TUI 均启用。
补充 Contacts 返回路径后再次重建 normal 定向断言，并完整重跑 normal 20/20（99.09 秒），
ASan 与 UBSan 的构建已包含该最终断言。日志在 `/tmp/chat-help-verify-20261005.log`、
`/tmp/chat-help-normal-final-20261005.log`。无编译警告、sanitizer 报告、suppression、跳过或 timeout 放宽。
`git diff --check` PASS；验证隔离库已删除。
Qt 暂评保持 87；TUI 暂评 78/100：14/20、13/15、7/10、8/10、9/10、9/10、9/10、3/5、2/5、4/5。
Help 可用性改善不替代全产品评审；复合 Unicode、长身份的其余页面、消息编辑弹窗、
完整键盘/终端/参考矩阵和最终连续两轮 fresh review 均继续保留。
本阶段不改 Qt、server、client library、SQL 或 third，无 migration。

## 第十五轮：消息编辑的阅读与目标归属

本阶段从 `14f45b080601f09eb65e60aa6310e6d4c4117dba` 继续。
真实 before 编辑框为 278×281，默认英文按钮和平台图标，正文不换行；
Tab 会把全选的原文替换成制表符。原生驱动先后误选删除菜单项和只读会话，
经实际窗口和权威 SDK fixture 核对后修正，没有为了旧坐标修改产品。

编辑继续使用局部的真实 QInputDialog，复用 520 宽度、24 内容边距和 12 间距，
多行正文按显示宽度换行，Tab 到保存，保存/取消中文且无平台图标。
空字符串仍不提交，非空空白原样保存，不以 trim 改写既有业务规则。
正文、会话和消息 ID 在菜单打开前捕获；保存时检查仍是原会话且可发送。
不增加临时状态成员、协议、服务端快照或兼容包装。

既有 Qt UI 入口新增七个公共事件场景：取消、空串、非空空白、多行 Unicode、
弹窗期间插入历史、切换会话、菜单期间插入历史。
两种历史插入分别先复现编辑目标错误和原文错误 RED，再验证捕获身份/正文后的 GREEN。
定向驱动最初的焦点和菜单事件次序问题先修正，不记作产品失败或放宽断言。

首轮四档缩放、双 Qt 完成 49 张真实窗口采集，行为通过，
但独立原图复审发现输入区缺失主题边框和焦点轮廓，主代理再次亲看确认。
按钮样式正确不等于输入区样式正确，该版不记为视觉完整 PASS。
输入区聚焦/Tab 后左边中点的像素断言先复现 RED，再在完成对象命名和配置后刷新样式。
七个场景均验证焦点色 `#547C68`、非焦点色 `#DDD9D0`，没有新增状态或样式框架。

- 普通编辑 [before](images/experience/before-qt-message-edit.png) /
  [after](images/experience/after-qt-message-edit.png)，[200%](images/experience/after-qt-message-edit-200.png)。
- 长正文 [before](images/experience/before-qt-message-edit-long.png) /
  [150% 首部](images/experience/after-qt-message-edit-long-150.png)、
  [200% 末尾](images/experience/after-qt-message-edit-long-200.png)。
- [125% Tab 到保存](images/experience/after-qt-message-edit-tab-125.png)、
  [另一真实 Qt 收到更新](images/experience/after-qt-message-edit-peer.png)。

最终原图来自 `/tmp/chat-quality-message-edit-polished-final-qt-20261005`，
不是重排或合成；100/125/150/200% 实际窗口为 520×329、650×411、780×494、1040×658。
五次进程启动均记录实际二进制 SHA `9763a11f50a02cf833f8f33e121f633ad1a5d361d1ca211557aa7b5169d074f5`，
工作区 source hash 是同期观察，不冒称 HEAD 构建身份。
正文包含中文、ASCII、emoji 和组合符；真实 16 行、2815 字节保存、另一客户端更新、
三种编辑状态 Esc 保留、空串不提交和非空空白原样均逐项核对。
主代理和独立代理实际复看聚焦、Tab、窄正文及 200% 长文末尾，没有新增本专项 material issue。
49 张采集不等于 49 个独立测试，也不是全部页面或全部可访问性验收。
首轮与中间版的截图和驱动失败证据保留，不混作最终二进制结果。
最终完整 `tests/verify.sh`：normal 20/20（97.32 秒）、ASan 20/20（135.61 秒）、
UBSan 20/20（122.28 秒），Qt/TUI 均启用；无编译警告、sanitizer 报告、suppression、跳过或 timeout 放宽。
日志 `/tmp/chat-edit-final-verify-20261005.log`，对应验证隔离库已删除。
最终 Qt/X11 导航 4/4、TUI/tmux 导航 11/11，证据在
`/tmp/chat-edit-polished-qt-navigation-20261005`、`/tmp/chat-edit-polished-tui-navigation-20261005`。
原生采集自有数据库已删除、进程与端口已释放；长期服务与用户库未动。
`git diff --check` PASS。
同类回复、删除、附件与已读菜单尚需逐条核验，不由编辑专项推断为正确。
Qt 暂评仍为 87，TUI 78；全页面矩阵、复合字符、参考缺口和最终连续两轮
fresh review 尚未完成。本阶段不改 server、client library、TUI、SQL 或 third，无 migration。

### 终端复合字符的下一步证据

临时探针 `/tmp/chat-unicode-combining-wMg22F` 直接链接当前 FTXUI 7.0.3，
按 text → Screen → ANSI 核对字节。Latin 一/两个组合符通过，
CJK 一/两个组合符及连续宽字三项均丢失组合符（exit 1）。
只在 `/tmp` 副本中把组合符附加到最后实际 glyph，五项全部保留（exit 0），
原有宽度 2/2/3/3/5 没有改变。
这证明一个局部候选，不代表 ZWJ、FE0F 或实际 Input 光标、删除、Backspace 已正确。
没有修改依赖、pin 或 third；T04 继续保留，下一阶段先补端到端矩阵。
