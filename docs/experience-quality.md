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
| Q17 | P1 | 历史插入后删除/回复/附件指向另一条消息，已读详情显示另一条消息读者 | 第十六轮四项实测 RED 后修复；永久回归、完整三模式验证与真实双 Qt 通过；搜索复制同类缺陷也已修复 |
| Q18 | P2 | 回复取消按钮黑底低对比且无动作名称，已读成员默认浅蓝选中；头像/读者刷新丢失当前选择 | 第十七轮修复；公开事件回归、三模式完整验证、四档双 Qt 原图与独立专项复核完成 |
| Q19 | P2 | 长提及越过气泡右缘；无空格长正文和 URL 无法正确分配换行高度 | 第十八轮统一正文排版，永久 256 组合与完整三模式验证 GREEN；原生专项结果见下文 |
| Q20 | P2 | 消息搜索 Tab/Down 已改变当前/选中行，但无任何可见反馈 | 第二十三阶段修复，正式三模式及真实双Qt四档缩放通过 |
| Q21 | P2 | 搜索正文实时更新后可能不再匹配；删除占位仍计入“找到”数量，与界面说明矛盾 | 第二十四阶段修复，正式三模式、真实双Qt四档与AT-SPI行数/复制/草稿核验通过 |
| Q22 | P2 | 原生无障碍树中的搜索输入/结果、会话/消息列表和个人资料入口没有动作名称 | 第二十五阶段补九个稳定操作名称，原生四档树、资料/联系人/添加好友实际AT-SPI动作通过；不代表全部屏幕阅读器验收 |
| Q23 | P2 | 原生消息记录/搜索结果行只有正文名称，没有发送者/时间的可读取上下文 | 第二十六阶段从唯一消息model提供上下文，永久RED/GREEN及四档真实原生树/live重建/删除隐私通过；全无障碍矩阵仍开放 |
| Q24 | P2 | 群消息可见已读人数包含peer与0，但原生Name仅描述自己消息的非零已读 | 第二十八阶段复用read_count_role，永久RED/GREEN及四档真实SDK/原生树计数一致；可见截图与完整无障碍验收分别记账 |
| Q25 | P2 | 主消息列表可通过键盘移动当前消息，但NoSelection同时隐藏了当前行焦点 | 第二十九阶段复用现有焦点轮廓；永久RED/GREEN、四档真实键盘回复/回应/读者详情及三模式门禁通过，完整键盘矩阵仍开放 |
| Q26 | P2 | 会话、联系人和用户搜索列表有键盘当前行，但Enter不执行对应动作 | 第三十阶段复用原生activated与现有业务动作；永久Return/小键盘Enter、真实双Qt三条Return任务及三模式完整门禁通过，完整键盘矩阵仍开放 |
| T04 | P2 | FTXUI 对 ZWJ、宽字组合符和部分 emoji presentation 的格宽/输出不一致，真实终端可能错位或丢组合符 | 第十九轮字素编辑与宽字组合符附着修复已进入生产；终端 span、裁剪、leading mark 和 shaping 仍未闭环，不计 Unicode 全矩阵通过 |
| T05 | P2 | Help 的命令列表横向裁掉，窄屏快捷键不换行；滚到底仍无法看到末尾命令 | 第十四轮按词换行与实际内容滚动；六档宽度、两类高度的原生证据收口 |
| T06 | P2 | TUI 搜索选中消息之前一项被删除时，只clamp索引，选中目标跳到另一ID | 第二十四阶段修复，永久RED/GREEN、真实双TUI六档选中目标/可复制正文/草稿验证通过 |

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

## 第十六轮：菜单操作的消息归属

从 `89004f58082486a261ca6bc9ed7c05d87549910b` 继续，临时公开 Qt widget 探针
在菜单或确认框的真实嵌套事件循环内插入旧历史，核对模型确实从 row0=7 变为 row0=6、row1=7。
删除和回复实际信号指向 6；附件同时发送错误 ID 6 与原文件名 `original-7.bin`；
读者 2 只读到 6，读者 3 读到 7，已读详情却显示两人而不是仅 3。
这些是实际输出，不是把未执行的代码风险当作 bug。
读详情最初 fixture 漏保留 read positions，空列表不能作证明；
更正后重新得到确定 RED，错误 fixture 日志保留但不记作通过。

操作继续复用菜单打开前的会话与消息 ID，回复预览也捕获原发送者和正文；
菜单返回后检查连接与原会话，删除确认返回后再检查一次，不跨会话执行。
已读详情接收具体会话/消息 ID，初始同步刷新不合法时不进入弹窗，
之后仍按原 ID 随权威读者/模型/头像变化刷新或关闭。
即时 delegate 点击保持原资格检查；置顶和表情回应已有正确身份保护，不为统一而重写。
未增加状态字段、token、通用菜单框架或后端快照。

临时四项探针已全部 GREEN，既有 Qt UI 入口新增十个公共事件场景：
四类历史插入、四类菜单期间切换到就绪的另一会话、删除取消和确认期间切换会话。
核对原 ID、文件名、回复发送者/正文、特定读者、操作次数及没有跨会话信号；
原有编辑、权限、生命周期和其他断言全部保留。这不是十个新增 CTest target。
完整三模式验证与真实双 Qt 使用结果见下；临时四项 RED 并不表示
十个永久场景都在旧版执行过，切会话与取消场景是追加的保护覆盖。

递归检查又实测确认搜索结果复制的同构缺陷：菜单内收到更早分页结果，
原结果 ID 7 移到 row1，实际 clipboard 却是 ID 6 的正文。
临时探针与既有 Qt UI 的永久断言均先 RED；只在菜单前捕获原 QString 后均 GREEN，
保留中文、组合符和 ZWJ 的原文字节，不为复制增添另一个消息模型或状态。
群成员菜单按捕获用户 ID 查找并重查权限，会话列表按捕获会话 ID 操作，不机械修改。
早期真实双 Qt 普通流程已核对唯一读者、回复原 ID/正文、删除取消与显式确认、原附件下载字节，
但菜单 Home、文件按钮 Return 和 SDK 重复登录的三种驱动/fixture 问题先被区分并修正。
失败运行不记为完整通过；原始证据保留，自有失败库经精确核验后删除。
最终包含搜索复制的统一版本完成真实双 Qt 使用，证据在
`/tmp/chat-quality-message-actions-final-copy-qt-20261005`。
两客户端实际进程 SHA 均为 `29ce5dacbb67f6cc817838da8e04ba4289e641be8faec7ac96795e3951605551`，
同期源码 hash、启动时间和 fixture 记录齐全；中间版结果不冒作最终统一版本。
26 张原图与 20 张窗口裁图不是独立测试数。

- [特定读者详情](images/experience/after-qt-message-action-read-details.png)：仅实际读到原消息的用户 3。
- [原消息回复条](images/experience/after-qt-message-action-reply-preview.png)、
  [另一客户端收到的回复](images/experience/after-qt-message-action-peer-reply.png)：引用原 ID 2、发送者和完整正文。
- [显式删除](images/experience/after-qt-message-action-delete.png)：默认取消保留消息，显式确认仅删除 ID 4。
- [附件保存](images/experience/after-qt-message-action-attachment.png)：原始 130 bytes 与下载 SHA 完全相同。
- [搜索复制](images/experience/after-qt-message-action-search-copy.png)：真实系统剪贴板与原正文一致，34 UTF-8 bytes。

主代理和独立代理复看真实原图，核对具体读者、peer 引用、危险动作焦点、附件和复制结果。
最终完整 `tests/verify.sh`：normal 20/20（98.08 秒）、ASan 20/20（134.78 秒）、
UBSan 20/20（122.24 秒），Qt/TUI 均启用；无编译警告、sanitizer 报告、
suppression、跳过或 timeout 放宽。日志 `/tmp/chat-actions-final-verify-20261005.log`。
最终 Qt/X11 导航 4/4、TUI/tmux 导航 11/11，证据分别在
`/tmp/chat-actions-final-qt-navigation-20261005`、`/tmp/chat-actions-final-tui-navigation-20261005`。
完整验证与原生采集隔离库已核验删除；自有进程与端口释放，长期服务和用户数据不变。
原生普通路径与公共 widget 模型重排探针相互补证，不宣称普通截图本身证明并发重排正确。
主代理亲看原图发现 Q18：回复取消黑底与已读列表默认浅蓝选中态，
身份修复不等于这些界面已经达到最终视觉品质。
Qt 暂评 87、TUI 78，新增功能正确性问题的局部修复不替代全产品达标或最终两轮 fresh review。
本阶段不改 server、client library、TUI、SQL 或 third，无 migration。

## 第十七轮：回复取消与读者列表

从 `1924f653657bb9635964a432ea149003a78c19cf` 继续。
第十六轮真实原图的取消回复为 28×24 黑底 ×，无动作 tooltip，accessible name 也只是 ×。
公开 widget 探针核对已读成员行高 38、默认浅蓝选中；选中读者 3 后触发实际头像刷新，
current row 变为 -1、selection 清空，但键盘焦点仍在列表。
这是选择身份丢失，不误称窗口丢焦点。

取消回复复用已有 SVG close，36×36 点击区域、中文动作名称、透明表面和绿色键盘轮廓。
Space 取消后回到原草稿，不清正文，下一条发送不携带旧 reply ID。
读者列表复用现有 user_delegate、62 高行与头像尺度，显示真实“已读”而非虚构在线状态；
姓名显示省略、完整 Unicode 姓名保留在数据与 tooltip。
刷新前仅局部捕获当前读者 ID 和滚动位置，重建后按 ID 恢复；不存在的读者不移交选择。
不增加成员状态、通用列表框架或后端快照。

既有 Qt UI 新增公开控件场景，先在原代码得到点击区域/图标 RED，
修改后全部 widgets-only GREEN；另有实际旧版头像刷新丢选择的独立探针。
覆盖真实 Backtab/Space、草稿和发送 ID、MousePress/Release、Down、23 人滚动列表、
头像/读回执刷新及读者移除。原有消息身份、编辑、权限和生命周期断言仍执行。
离屏菜单返回后未重新激活窗口是驱动问题，先核验再修驱动，不添加产品 Tab 状态。
圆角轮廓的 grab 像素 alpha 为 242，直接与不透明 QColor 比较会假失败；
回归检查明确 RGB 与高 alpha，并对照失焦图无轮廓，不弱化可见焦点要求。
最终完整 `tests/verify.sh`：normal 20/20（96.74 秒）、ASan 20/20（137.27 秒）、
UBSan 20/20（123.14 秒），Qt/TUI 均启用；没有 sanitizer 报告、编译警告、
suppression、跳过或 timeout 放宽。日志 `/tmp/chat-q18-verify-20261005.log`，隔离库已删除。
Qt/X11 导航 4/4、TUI/tmux 导航 11/11，证据在
`/tmp/chat-q18-qt-navigation-20261005`、`/tmp/chat-q18-tui-navigation-20261005`。
独立公开 widget 探针在 100/125/150/200% 跨多次事件循环复核当前读者、滚动和焦点；
移除所选读者后，resize 和新增读者不误交选择；按钮边缘鼠标取消也保留正文并回输入。
这些不是原生视觉证明。随后真实双 Qt 的 100/125/150/200% X11 使用完成，
76 组原始全屏图与对应窗口裁图保留在 `/tmp/chat-quality-q18-final-third-qt-20261005`。
每档 19 组，不把图数当独立测试数或百人容量结果。
五次实际启动的 `/proc/PID/exe` SHA 均为
`1c6f2c28298947bca93daa82a9149564389234d9cd06e8decaf5a5e536bb8a9e`。
每档实际取消保留完整多行 Unicode 草稿，随后发送不带 quote，另一客户端实际收到。
真实 SDK 头像和读回执更新后读者数 51→52，原生选择行与滚动位置保持；
数字读者身份与成员移除行为由永久控件断言补证，不仅凭屏幕行位置推断。
主代理亲看最终原图，独立审查另看四档 48 张状态图和四张全屏 tooltip，
没有发现 Q18 新 material 缺陷；长名提示自然跨过 dialog，不能用窗口裁图误判裁切。
64 UTF-8 bytes 的 Unicode 姓名实际通过 SDK 注册，不用超限假名冒充业务支持。
最终 `result.json` 为专项 PASS，自有数据库已删除、端口 18886 与自有进程释放，长期服务未动。
首两次原生驱动分别没有让背景客户端真实阅读、没有重连预留给 TUI 的夹具账号；
原图与 SDK 输出明确区分，失败不计为完整 PASS，自有数据库和进程均清理。
没有为了驱动失败修改产品或放宽读者数断言。

原图对照与关键状态：

- 旧 [取消回复](images/experience/after-qt-message-action-reply-preview.png) /
  新 [正常](images/experience/after-qt-reply-cancel-normal.png)、
  [125% 悬停](images/experience/after-qt-reply-cancel-hover-125.png)、
  [200% 键盘焦点](images/experience/after-qt-reply-cancel-focus-200.png)。
- 旧 [读者详情](images/experience/after-qt-message-action-read-details.png) /
  新 [键盘选择](images/experience/after-qt-read-members-keyboard.png)、
  [125% 失焦仍保留选择](images/experience/after-qt-read-members-unfocused-125.png)、
  [200% 回执刷新](images/experience/after-qt-read-members-refresh-200.png)。
- [200% 完整长姓名提示原始全屏](images/experience/after-qt-read-members-tooltip-200.png)、
  [另一客户端收到取消引用后的正文](images/experience/after-qt-cancelled-reply-peer.png)。

上述 before 是发现缺陷的前一轮真实使用，不冒称与新夹具人数完全相同。

Qt 暂评 87、TUI 78；此项局部 GREEN 不代表全产品或连续两轮 fresh review 完成。
复合 Unicode 的严格临时探针仍实际 RED：8 项拆字素编辑、9 项渲染丢失。
原探针 exit 0 仅观察事件和粘贴成功，新 strict 退出码计入上述缺陷，旧证据日志未覆盖。
未修改依赖，不把临时候选记为 T04 已解决。

## 第十八轮：长消息与提及排版

从 `07b3ceea7216cf53db619aeea19aeb6c63694bb4` 继续。
第十七轮真实 125% 图中，合法长姓名的蓝色提及越过绿色气泡右缘，延伸到窗口裁切处。
公开 delegate 探针又确认无空格长正文/URL 的旧测量无法为全部换行分配高度。
永久回归先在旧实现得到 `FAIL long message allocates every wrapped line: 320/0/0`，实际 exit 1；
不是把测试编译失败或人为大画布当成产品 RED。

所有消息正文统一用 QTextLayout 的 `WrapAtWordBoundaryOrAnywhere` 测量与绘制，
长 token 无词边界时也可换行；字号、气泡宽度、元数据规则与原始消息内容不改变。
仅显示副本把 LF 换成等长 LineSeparator，保留原始 Unicode 和前导/尾随空行，
提及的 UTF-16 偏移不变，引用正文不误高亮。sizeHint、paint、editorEvent 仍共享实际布局。
没有用 clip 隐藏溢出，也没有增加另一套消息模型或持久布局状态。

既有 `qt_delegate` 新增 256 个组合：四档宽度、双方向、长正文/URL/提及/引用、
两个非零/零起点与四档 DPR。独立 Qt 公开排版 oracle 核对每行高度和全部字形像素，
包含抗锯齿边缘；原有短正文、附件、反应与已读断言全部保留。
不是 256 个新增 CTest target。独立探针另从实际绘制区域点击六种布局：
36 次反应与六次已读点击目标正确，空白处零信号。
短正文像素保持，但真实 fallback 字体可能使完整气泡高度增加；不宣称旧/新整张图完全相同。

最终统一源码完整执行 `tests/verify.sh`，Qt/TUI 均 ON：

| 构建 | 完整 build / CTest | 实际耗时 |
|---|---|---:|
| normal Debug | PASS / 20/20 | 103.42 s |
| ASan | PASS / 20/20 | 141.22 s |
| UBSan | PASS / 20/20 | 134.91 s |

日志为 `/tmp/chat-q19-final-verify.log`；无编译警告、sanitizer 报告、
suppression、跳过或 timeout 放宽，隔离库已删除。
Qt/X11 导航 4/4、TUI/tmux 导航 11/11，证据分别在
`/tmp/chat-q19-qt-navigation-20261005`、`/tmp/chat-q19-tui-navigation-20261005`。

最终双 Qt 原生操作保留在 `/tmp/chat-quality-q19-native-final3-20261005`，驱动实际 exit 0。
主客户端逐档 100/125/150/200%，另一客户端固定 100%；五次实际启动的
`/proc/PID/exe` SHA 均为 `3f45f551d7d28ca8ffec640d1373d1a5286ece67c5ff34749d56fc7288ff692b`。
每档覆盖 980×640、1280×800、1440×900、1680×960 逻辑窗口和连续 resize，
共 44 组全屏原图/窗口裁图，不把图数视为独立测试数。
真实 Qt 输入发送完整 Unicode 正文和 64-byte 合法用户名提及，持久化正文、目标用户均精确一致；
SDK 更新反应和引用，Qt 实际呈现长 quote、末行、时间与读者信息。
引用来自 SDK，不冒称本轮执行了 Qt 回复菜单；客户端聚焦并滚到最新后，真实阅读位置达到最后消息。
同期源码 hash、SDK 事实、实际进程与几何信息齐全，不凭截图推断数据库结果。
最终日志无运行错误标记，隔离数据库已由 SQL 再核不存在，自有 Qt/服务器/端口已清理，长期服务未动。

首两次驱动失败分别为输入点击位置错误、背景读者未激活且未滚到底；
失败图/SDK 记录保留，不改产品已读规则或放宽断言。
第三次已完成所有事实与清理，但 SSH 观察返回 255，不冒称原命令成功；
独立核对其完整产物后又重新执行上述 final3，拿到明确 exit 0，连续 resize 后也实际滚到最新再采图。
公开回归和原生图相互补证，不把 `PASS_DRIVER` 单独当视觉验收。

原图对照（旧/新夹具不同，不冒称内容完全相同）：

- [旧 125% 长提及溢出](images/experience/before-qt-long-mention-125.png) /
  [新 125% 最小窗口](images/experience/after-qt-long-message-minimum-125.png)。
- [200% 长引用与末行](images/experience/after-qt-long-message-reply-200.png)、
  [200% 连续 resize 后](images/experience/after-qt-long-message-resized-200.png)、
  [另一客户端的提及、反应和引用](images/experience/after-qt-long-message-peer.png)。

主代理亲看最终四档原图中的长 token、完整尾标、短行、时间/回执和反应位置，未见 Q19 溢出或重叠。
独立代理已复看同一最终产品源码的前次 24 张 size/reply/peer 图，未见 Q19 新 material 缺陷；
本次 final3 的 SDK 二十条正文/提及/反应/引用与读位另逐项核对，
后续独立复看已覆盖本次四档缩放的十六张 size 图、四张 resize、四张 reply、
四张固定 100% peer-reply，共 28 张，未见本专项溢出或重叠。
normal、peer-normal 和 input-ready 的另外十六张未计入独立视觉 PASS；
审图副本在 `/tmp/chat-q19-final3-independent-review.buhPOD`。
不把本专项当全页面 fresh review。
原生 emoji 的字体 shaping 仍需独立验收，本阶段不以字节保留声称全部显示策略已完成。

### T04 的独立边界研究仍不是生产修复

当前 pinned FTXUI 的扩大严格矩阵仍 RED：248 项 codepoint 模型检查中 25 项失败；
真实 Chat bracketed-paste 事件中的九类原文保留，但光标/覆盖/密码/鼠标与显示列模型未闭环。
真实 xterm/tmux DSR 表明 ZWJ、家庭等宽度不同，tmux 虚拟列数不等于外层终端实际 shaping；
不能统一硬编码 emoji 为两列，也不能仅根据 TERM 假定已适配。

临时维护库候选采用 utf8proc 2.12.0 / Unicode 18 的 stateful UAX29；
独立 fresh 重编证据在 `/tmp/chat-t04-linear-review-BukhBV`。
官方 GraphemeBreakTest 实际为 **853** cases（先前口头 883 包含注释等行，不是 case 数），
逐 byte/正负位移/越界的边界错误为零；候选 512-byte Previous/Count/Iterate 各 decode 512 次，
旧候选为 131839 次。原编辑、合并字素、覆盖/密码和 CRLF 回归 GREEN，旧 Input 对照仍严格 RED。
这些只证明候选单次边界 API 线性，不代表整个 Input 操作线性或生产集成完成。
leading combining 的实际渲染仍丢 bytes；终端 span、Screen continuation、边界裁剪与选择复制未解决。
依赖与子模块未修改，不提交 `/tmp` fork 或用候选结果关闭 T04。

Qt 暂评仍 87、TUI 78；本轮局部修复不替代全页面/参考/无障碍/终端矩阵，
最终两次独立完整 fresh review 尚未完成。

## 第十九轮：终端输入保留完整字素

从 `a04c91e1fc765ac9e0010ff8159b9bd3ef58432d` 继续。此前 pinned FTXUI 会在
移动、删除和覆盖中拆开 ZWJ、区域指示符、肤色、Indic 等扩展字素；宽字后的组合符
还会附着到保留空格而丢失。采用 utf8proc 2.12.0 / Unicode 18 的 stateful UAX29，
不在应用中维护 Unicode 规则表，不以 ASCII 替代原文。

FTXUI 子模块和所有 gitlink 不变。启用 TUI 时才下载官方固定提交 archive，
SHA256 校验见 [维护说明](../tui/cmake/README.md)。构建目录从 pristine 源精确应用
三文件补丁，原 screen/component target 各替换一个 TU，私有头也来自修补目录。
独立核对 normal/ASan/UBSan 的编译数据库、archive/符号、static export 和许可；
没有重复实现、库符号覆盖技巧或 `/tmp` fork 依赖。重复配置不改变最终文件 mtime。

Input 的每个 event/render 共用操作内边界，插删后重新分段，回调之后不读取旧边界。
CRLF 保留原文 byte offset，覆盖不吞换行；password 每个扩展字素显示一个 bullet。
非法 UTF-8 的原 bytes 不改写，每个非法 byte 独立分界。
没有持久化边界缓存，也没有新增应用层 Unicode wrapper。

### 永久回归与独立 RED/GREEN

新增 `tui_unicode`，使用原始官方 853 条 GraphemeBreakTest 作为独立边界 oracle；
逐 byte、正负移动、越界/极值、非法 UTF-8、11 类 Input、合并插删、覆盖/密码、
鼠标/上下、CRLF、Ctrl-word 和真实回调重入均通过。
相同永久测试链接原 pin 的真实 String/Input TU 时 actual exit 1，普通断言失败 6531 次，
极值阶段明确跳过以避免旧实现超长循环；新生产 target actual exit 0。
这个数是重复断言数，不是 6531 个独立产品缺陷。

独立候选计数还验证真实 Input 单次操作共用边界：256 字符 CtrlRight 从 65536 次
UTF8 decode 降到 256 次，鼠标从 130560 降到 256 次；不是 viewer/capacity benchmark。
这些计数不涵盖 DOM 绘制或原生终端 shaping，不用其替代体验验收。

### 完整构建与门禁

最终统一源码串行执行 `tests/verify.sh`，Qt/TUI 均 ON：

| 构建 | build / 全量 CTest | 实际耗时 |
|---|---|---:|
| normal Debug | PASS / 21/21 | 104.86 s |
| ASan | PASS / 21/21 | 136.98 s |
| UBSan | PASS / 21/21 | 132.55 s |

日志 `/tmp/chat-t04-final-verify.log`；编译告警、sanitizer 报告、sanitizer suppression、
跳过和 timeout 放宽均为零，新 C 依赖也有 sanitizer 插桩。
额外全新 Debug `-Werror` build 和全量 21/21 CTest 通过（106.87 s）；
全新 RelWithDebInfo `-O2 -g -DNDEBUG -Werror` 的 Unicode target/回归通过（0.01 s），
但该配置的全项目 build 未通过：未修改的 Boost.Capy 分配路径触发 GCC
`mismatched-new-delete`，既有 PG coroutine 测试触发 `maybe-uninitialized`。
未压告警、未改第三方或将局部 target GREEN 冒称全量 RelWithDebInfo GREEN。

首次完整验证 normal/ASan 已通过，UBSan Qt UI 与另一次独立 CTest 并行时撞到
夹具固定端口 18769，失败日志 `/tmp/chat-t04-production-verify.log` 保留。
核对两个进程均已退出后串行重新执行完整门禁，取得上述最终三套 21/21；不重写失败记录。

### 两真实 TUI 的编辑流程

最终驱动 `/tmp/chat-t04-root-native-editing.py` actual exit 0，证据
`/tmp/chat-t04-root-native-editing-final2-20261005`。两个真实 TUI 实际 `/proc/PID/exe`
SHA 均为 `e4e379ef9920198a8a3f67685c26a6677f56177fa78271983c06ae99ab584963`。
60/80/120 列下，各 11 类字素分别完成 Backspace、Delete、overwrite、Ctrl-word
后发送，共 132 条流程。每条 SDK 正文精确匹配、消息 ID 唯一，另一 TUI 实际收到；
66 份 plain/styled capture 保留。此计数是流程数，不是按键数或独立视觉审查数。
仓库 TUI 导航回归另 actual 11/11，包括多行粘贴、重连草稿、选择目标与退出。

探针首试因用户名 fixture 前缀超过 24 bytes 被拒绝；第二试用旧“消息已发送”状态
立即查询，未等待此次真实接收而过早清理。只修诊断前缀和等待目标：最终每条都等待
对端出现本次唯一 marker，再核 SDK。失败目录和日志保留，未改产品发送语义。
最终及失败的自建库均经 SQL 再核不存在，自有 TUI/服务器/端口均清理；长期服务未动。

### 尚未完成，不能关闭 T04

主代理阅读三档真实 capture：CJK combining bytes 已保留，编辑后正文精确；
80 列 ZWJ 行仍可见后缀/边界残留，不能给其视觉 PASS。
UAX29 分界不决定 terminal columns：当前 width/cell mapping 仍逐 scalar，
leading combining 仍丢 bytes，窄 Text 会裁掉 cluster 内部，Screen 只识别两列 continuation。
下阶段需将列映射、完整 span、原子裁剪和选择复制在真实 xterm/tmux 中统一验证；
不能硬编码全部 emoji 两列或以 tmux 虚拟 DSR 冒称外层字体 shaping 正确。
本轮只交付完整字素编辑基础，保留 T04 和整体品质目标。Qt 暂评 87、TUI 78；
全页面、参考、无障碍及最终两次独立完整 fresh review 尚未完成。

## 阶段 20：终端字素渲染的可见范围与写入契约

生产基线 `d90397aadb014c3cb433e9210fa271367af83276` 未改实现。
本阶段只在隔离 FTXUI 副本验证渲染结构，候选未接入 Chat，不构成 T04 交付。
原 pin、子模块和应用源码保持不变。

### 严格失败与隔离原型

`/tmp/chat-t04-span-contract.cpp` 的 107 条固定断言覆盖 Screen/node/stencil 的整字素
裁剪、半格复制、叠加覆盖、真实 Separator、VText、Canvas、Frame 和零宽 bytes。
探针 SHA256 `6194e96d7a837745b6e69764142d3735950ce9b3f9d0e4c0bf3e52be2c79a0e3`。
当前生产链接 actual exit1（61 条失败）；第一隔离原型
`/tmp/chat-t04-span-candidate-QosU2Q` actual1（4 条失败）；第二原型
`/tmp/chat-t04-span-viewport.EvHoJa` actual1（3 条失败）。断言数不是独立产品缺陷数。
初版肤色 legacy slots 误写成4已更正为实测库值2，并补对照；部分裁剪的 oracle
从保留旧点号加强为清空可见交集。旧失败记录保留，不将修正 fixture 当产品修复。

第二原型将 Selection endpoint 和真正 viewport 分开；root/Frame/容器逐层传递
当前可见交集，Text 仅复制完整可见字素。半格选区仍复制整个字，半格 viewport
则不显示或复制该字。没有上一帧可见性缓存，没有改 child origin。
独立 `/tmp/chat-t04-viewport-independent-Z1xCpm` 冻结 headers/archives 后验证27/0，
涵盖嵌套 x/y、真实焦点滚动、负 origin、HBox/VBox/Flexbox、兄弟范围恢复、反选与 resize。
GetNodeSelectedContent 的 parts 正确；既有叶节点空 return 未被宣称解决。

递归 writer 审查补出原清单漏掉的 Gauge/Graph。第二原型使 Surface SetCell/SetGlyph
共用撤销旧组的边界，迁移 Border/Cell、ClearUnder、ScrollIndicator、全部 Separator、
Gauge/Graph 与 Canvas import/export；style-only 和 box 单列字符合并保留原语义。
主代理 import 探针14/0；独立 sibling writer 旧157 checks/83 FAIL、新95 checks/0 FAIL。
其中检查数量随实际 head/continuation 改变，新版另有9条 SetCell 检查，不能将83说成
独立场景。六 ASCII 被误当一个6列字素的诊断 fixture 已更正，首版日志保留。

### 原生 CJK 对照

`/tmp/chat-t04-cjk-native-c1pExS` 冻结两版 binary/archives 后，在独占 Xvfb/XTerm372
运行；20份真实TTY DSR、原始输出与4张 PNG 保存。主代理和独立代理亲看两版窗口：

| 独立场景 | 生产 | 第一候选 |
|---|---|---|
| 分配1列的中 | 实际2列越界 | 1列空白，整字裁剪 |
| 5列边框 | 中间行右边框丢失 | `│测 │`，完整边框 |
| 续格覆盖X | X被吞 | ` XR`，X/R保留 |

实际运行 `/proc/PID/exe` SHA 与冻结 binary 一致；自有 helper/xterm/Xvfb 全部退出，
未用数据库、服务或 tmux0。这里仅是三个 CJK 场景，不是产品、emoji、tmux或T04 PASS。
另外现存终端原图/DSR只读复审再次确认：tmux虚拟两列不证明outer XTerm完整shaping；
Fontconfig找到Noto Color Emoji也不证明该终端实际绘制正确。

### 隔离门禁与仍未完成的部分

第二原型 normal/fresh ASan/fresh UBSan build actual0，各 sanitizer 82翻译单元均实际
核对插桩。三套重新链接：独立 review8/0、viewport27/0、import14/0、Unicode853/0；
sibling writer 又独立链接fresh ASan/UBSan，均95/0。report/warning/suppression/skip为零，
但严格107断言仍3 FAIL、exit1，sanitizer无报告不能抹掉语义失败。
这是隔离原型门禁，未重复宣称本阶段正式全项目21 CTest或真实双TUI验收。

仍失败：leading/zero-only原bytes显示两项，以及raw空续格clear的不可观察擦除意图。
还需统一显示载体、原文复制和Input列映射；确定真实受测terminal policy；完整复审
custom writer与写入契约，再以TUI专属精确补丁交付并跑正式全量/原生门禁。
旧 scalar width 尚未改，候选ZWJ5/11不能冒称native4/8或tmux2/2正确。
不将任何raw mutation视为自动可观察，不把未合入原型当生产已修复。
详细源码、hash、命令、失败与scope见上述各目录REPORT。
Qt暂评87、TUI78，T04仍P2，全品质目标及两轮完整fresh review继续保持未完成。

## 阶段 21：完整字素显示、原子裁剪与原文选择

本阶段从 `42687959b138aa8de567d8e00533a328a25df3d9` 继续，把前阶段隔离验证的
完整 span、Selection viewport 与递归 writer 修复接入正式 TUI。FTXUI 原 pin 和
所有 submodule 均不修改；维护补丁扩为 21 个文件的 SHA guard、零 fuzz 应用，
screen/dom/component 真实 target 替换对应 TU，patched public header 优先传递给消费者。
不使用额外对象覆盖 archive 符号，也不依赖 `/tmp` 源进行生产构建。
独立重配置核对 21/21 文件与冻结候选一致；当前三套编译数据库各含 76 个 FTXUI TU，
没有重复 TU，ASan/UBSan 的全部 76 TU 和 utf8proc C TU 均实际插桩。

Text/VText/Canvas 按完整 EGC 提交；Surface 用明确 head/continuation span 维护整组。
覆盖任一续格也清理旧组，半字视口只显示空白，不复制隐藏字；半格选择仍复制完整可见字。
Frame 的 viewport 与选择端点分别传递，嵌套容器和 sibling 不泄漏可见范围。
Border/Separator/Gauge/Graph/ScrollIndicator/ClearUnder 同步迁移到 typed 写入。
自定义单格 separator/border 不能把宽字或多个 EGC 塞入一格；Canvas import 尊重源 stencil。

### 显示载体不是原文

完整原字素、编辑和发送保持不变。孤立 nonspacing/enclosing mark 使用 dotted circle，
format-only、非法 bytes 和危险控制字符使用替代标记；这些显示载体不写入消息或复制内容。
原生验证发现孤立 Mc `ः` 加圆圈在 XTerm 实占两列，旧候选只分配一列，严格 RED；
最终孤立 spacing mark 采用单格 replacement，正常有基字的 cluster 不改写。
这是 orphan display policy，不宣称解决普通 Indic 字体 shaping。
`DisplayWidth`、Input 列映射、Text layout 与应用换行/省略共用同一显示列模型；
`string_width` 的旧 scalar 政策未改。`Utf8ToGlyphs` 保留原字素及显示列的空 slots。
Text 的既有 CRLF 行/复制归一化语义保留，不把它宣称为逐字节 CRLF copy；Input/source bytes 保留。

mutable CellAt/at 只保证读取与样式修改，字符修改必须使用 SetGlyph/SetCell 或 Canvas typed API。
空续格 `.clear()` 没有可观察变化，不能被解释成擦除请求；未伪造解决该 raw-mutation 反例。

### 永久回归、失败记录与修正

新增 `tui_display` 的 628 项断言覆盖 raw/display 分离、Text/Input 列与光标、密码、鼠标、
换行、安全序列化、半字裁剪与原文选择；新增 `tui_span` 的 80 项覆盖 typed alias/整组擦除、
Text/VText、嵌套 Frame、custom decorator、Canvas stencil、sibling writer 与 source ownership。
Text/VText 只持有一份正文与边界 offset，不保存悬空 view，短临时正文和 65-byte 单 EGC
在 caller 修改/销毁后仍正确绘制与复制。原官方 853 条字素边界断言保留。
独立旧零载体原型实际 614 checks / 171 FAIL，新版首次永久测试 627/0；
修正真实 Mc 策略后当前为 628/0。旧 span 原型 74/8，新版 74/0，随后补 6 项 ownership 得 80/0。
不同版本的断言数量不能当作同一固定矩阵或独立产品缺陷数。

首次正式门禁 normal 的 tui_render 在 5 秒时限失败；第二次 normal 通过而 ASan 超时。
失败日志 `/tmp/chat-t04-render-production-verify.log` 和 `/tmp/chat-t04-render-final-verify.log` 保留。
实际 GDB 调用栈定位到 Help 选择范围计算反复构建 Text，不放宽 timeout、不减少 200 次双向按键断言。
删除重复 display 字符串、改为 owned source + offsets、单次布局，并为简单 ASCII 保留等价快速路径；
完整 Unicode 分段仍交给 utf8proc，官方 853 条不减。最终直接 normal 0.86 s、ASan 4.06 s，
两者所有原断言通过；不是容量 benchmark 或完整终端宽度政策。

### 原生证据及范围

最终 single-pass 正式库证据 `/tmp/chat-t04-production-native-current-TLI5Jm`：独占 XTerm372、
DejaVu Sans Mono / Lily Han Sans HW SC、110×30，两版本 40 个真实 TTY DSR、4 张 X11 PNG。
10 个输出行 bytes 与独立预期及实际列差全匹配；活进程 SHA、冻结 archives/source 齐全。
主代理和独立代理亲看：carrier 可见，孤立 Mc/ZWJ replacement 各一列，CJK 半字空白、
5 列边框完整、续格覆盖 X 未吞。21 个自有 PID 均退出，没有 DB/服务或 tmux0 操作。
旧 Mc 严格 RED 目录 `75Qk3A` 与非最终首次 GREEN `green-X1y8ll` 均保留，不外推版本。

- [旧 helper 原图](images/experience/before-tui-carrier-span-xterm.png) /
  [最终正式库 helper 原图](images/experience/after-tui-carrier-span-xterm.png)。

这两图是 isolated Screen helper，不冒称整个 Chat 截图或 emoji/full-app PASS。
最终两个真实 TUI 在 60/80/120 列完成 132 条编辑发送流程，SDK 精确正文和唯一消息 ID、对端接收
均一致；另在 60/70/80/100/120/160 列完成 48 条 carrier 原文发送/原子 Backspace 流程。
两个探针的真实 `/proc/PID/exe` 均为
`e94bcff63a8828b822fa5a2c156e230bac12c0dac1f2b626b4cbe1efb4de43b5`；SDK 证明显示载体未写入消息。
证据为 `/tmp/chat-t04-owned-render-native-editing-20261006` 和
`/tmp/chat-t04-owned-carrier-app-final2-20261006`，成功隔离库已删除。
carrier 首试错误等待被一行 composer 滚动隐藏的 marker，失败时实际 carrier 已可见；
只修临时驱动的观察目标，未改产品或删除失败日志。
真实 Qt/X11 导航 4/4、TUI/tmux 导航 11/11，目录分别为
`/tmp/chat-t04-owned-render-qt-navigation-20261006` 与 `/tmp/chat-t04-owned-render-tui-navigation-20261006`。

### 最终统一门禁

最终统一源码执行正式 `tests/verify.sh`，Qt/TUI 均 ON，无测试跳过、sanitizer suppression 或 timeout 放宽：

| 配置 | build / 全量 CTest | 实际测试耗时 |
|---|---|---:|
| normal Debug | PASS / 23/23 | 104.09 s |
| ASan | PASS / 23/23 | 138.44 s |
| UBSan | PASS / 23/23 | 133.91 s |

最终日志 `/tmp/chat-t04-owned-final-verify.log`，外层 runner
`/tmp/chat-t04-owned-final-validation.log` 明确 `VALIDATION_EXIT=0`。
额外本阶段全新独立 Debug `-Werror` 目录重新构建最终源码，23/23 CTest 通过（107.63 s），
日志 `/tmp/chat-t04-owned-fresh-werror-{build,ctest}.log`；编译告警和 sanitizer 报告为零。
正式门禁中的 tui_render 为 0.86/4.05/1.77 s，原 5 秒时限不变。
三套 sanitizer/source target 插桩与 patched public-header ABI 另有独立复核。

前一次完整 run 的 SSH 观察返回255，未把它当远端进程停止；检查原 PID 仍执行 UBSan，
等待其自然完成，三套 CTest 实际全通过（101.50/135.53/129.83 s）。
该观察日志 `/tmp/chat-t04-owned-render-complete-verify.log` 保留，不伪造总体 exit0；
确认原进程终止后才串行重新跑上述带真实 terminal exit marker 的最终 run，没有重复并发夹具。

### T04 与整体体验目标仍未完成

这些证据证明 carrier、typed span 和受测 CJK 路径，不证明复合 emoji 的终端宽度或字体 shaping。
主代理读当前 80 列真实 ZWJ capture，右侧仍有残留边界；tmux virtual DSR 不能代替 outer terminal。
正常 base+Mc 未在本次 native helper 中独立验收，不以 unit bytes 保留冒称视觉通过。
Qt 暂评87、TUI78不提高；T04 仍 P2。全页面、reference、responsive/HiDPI、无障碍、
完整日常使用及连续两次完整独立 fresh review 仍需推进，不将本阶段局部门禁视为整体完成。

## 阶段 22：孤立肤色修饰符与实际终端列策略

从 `7019086662a55f7ce2016f97698a527ef4a12359` 继续。没有以“全部 emoji 两列”替换
终端规则，也没有把第三方库的 API 数字当作实际字体、tmux 或完整产品验收。

### 确定的显示缺陷及窄修复

隔离原生对照发现孤立肤色修饰符没有显示出来。utf8proc 对其 scalar `charwidth` 为2，
原 `GlyphDisplayText` 因此认为存在基字；旧 FTXUI `string_width` 却将其作为 Extend 跳过，
最终 Text/Input 分配零列，光标、选择与可见输出不一致。
新增11组永久 fixture 保留全部旧断言，旧生产链接实际1096项中108项失败、exit1；
这是重复断言数，不是108个独立产品缺陷。日志 `/tmp/chat-t04-skin-red-test.log` 保留。

只排除无基字簇中的 `Extend && Sk` 作为基字，使用已有 replacement 显示载体。
独立扫描 utf8proc 2.12.0 / Unicode18 全部码点，该条件精确命中五种肤色修饰符；
没有排除全部 Extend，因后者还会误伤六个正宽 Mc/Lm 字符。
正常手势加肤色仍有真实基字，原字素不改写。连续五种修饰符是一整个无基字 EGC，
只显示一个载体。空格隔开时也是五个 EGC，但实际边界为首个孤立修饰符加四个
`space+modifier` 簇（GB9），不是五个孤立修饰符加四个独立空格，不混淆两种样本。

Text/Input 的原文、每个边界光标、鼠标、密码和完整/单字素复制全部保留；
显示载体不进入 SDK 正文或 clipboard。正式 targeted 结果1096/0、span80/0、官方边界853/0。
首次修改补丁 hunk 长度误填导致 build exit2；修正到真实177行后零 fuzz build成功，
失败 `/tmp/chat-t04-skin-green-build.log` 不覆盖、不冒充产品语义失败。

两个真实 TUI 在60/70/80/100/120/160列完成120条发送/Backspace流程，其中72条覆盖
五种肤色及连续簇，另48条重测既有孤立 mark/format。每条 SDK 正文精确、ID唯一，
另一 TUI 实际收到；两实际 `/proc/PID/exe` SHA 均为
`9d0eb3d19b98729764cc0a70d19d838b657137a2431ff08c930f49eb49b7e9ef`。
证据 `/tmp/chat-t04-skin-app.s14buU/result`；成功自建库由 SQL 再核不存在，
自有两个 TUI/服务器/18890端口清理，长期服务未动。
首个 driver 把输出写到不存在的临时父目录而失败，未创建数据库或启动客户端；
只纠正实际路径，保留 `/tmp/chat-t04-skin-app-run.log`，不改产品。

最终统一源码执行完整 `tests/verify.sh`，Qt/TUI均ON，外层真实`VALIDATION_EXIT=0`：

| 配置 | build / 全量 CTest | 实际耗时 |
|---|---|---:|
| normal Debug | PASS / 23/23 | 105.52 s |
| ASan | PASS / 23/23 | 147.39 s |
| UBSan | PASS / 23/23 | 132.58 s |

日志 `/tmp/chat-t04-skin-final-verify.log`，runner `/tmp/chat-t04-skin-final-validation.log`。
另全新目录 `/tmp/chat-t04-skin-fresh.ROUbgo` Debug `-Werror` 构建及23/23 CTest通过，102.40s；
没有编译警告、sanitizer报告、suppression、跳过或timeout放宽。正式render耗时0.86/4.10/1.75s，
原5秒时限不变。四套编译数据库各有76个FTXUI TU、没有重复TU；正式ASan/UBSan全部76个
及utf8proc C TU实际插桩，新增1096断言在正式三套测试中执行，不与旧隔离628 binary混用。

### 正式库的原生前后对照

独立 helper 链接最终正式 normal 的冻结 Screen/DOM/utf8proc 库及 patched headers，
没有使用候选列库。before/after × XTerm372/WezTerm20221119 × direct/tmux3.6a，
八种配置、96行观测、192份DSR，driver实际exit0。after helper SHA为
`fcfe972f85438dc30920a78f2f90e699e3ef0805fcb0bbdf3f4b1d9b6baf17bb`。
六个孤立输入在四种after环境的24次观测均显示单格replacement，完整行80列；
对应before24次为零列、内容不可见。主代理亲看四张原图，独立代理亲看八张窗口原图。

| 原生 XTerm helper | 未裁去剩余失败的窗口原图 |
| --- | --- |
| before | [孤立修饰符消失](images/experience/before-tui-orphan-skin-xterm.png) |
| after | [六个孤立输入可见](images/experience/after-tui-orphan-skin-xterm.png) |

这不是整个Chat页面的截图，也不证明全部12种输入通过。正常手势加肤色在XTerm
仍实际82列，tmux虚拟80列不能代替外层像素；空格隔开的样本声明5列，完整输出在
XTerm direct/tmux为88列、WezTerm direct为84列、tmux虚拟为88列，真实差异仍开放。
实际字素边界为 `[0,4,9,14,19,24]`。前次临时oracle误以为9列导致exit1，
只修正探针，不修改产品迎合它；shell引号和私有header的探针失败证据也保留。
完整报告 `/tmp/chat-t04-skin-carrier-native-final3.cBlpxU/REPORT.md`、raw/window原图及
输入/库/DSR SHA齐全，17个自有进程和专用tmux均实际清理，无数据库或服务启动。

### 列宽库不是字体 shaping 的替代证据

读取维护中的 [C11 libwcwidth API](https://github.com/jquast/wcwidth/blob/f1793abb4ebace173e307294fc15d8eb0d8fdb88/libwcwidth/include/wcwidth/wcwidth.h)、
[序列实现](https://github.com/jquast/wcwidth/blob/f1793abb4ebace173e307294fc15d8eb0d8fdb88/libwcwidth/src/wcswidth.c)和
[终端修正](https://github.com/jquast/wcwidth/blob/f1793abb4ebace173e307294fc15d8eb0d8fdb88/libwcwidth/src/wcstwidth.c)，
固定提交 `f1793abb4ebace173e307294fc15d8eb0d8fdb88`、C11、Unicode18，MIT并保留Kuhn声明。
这是近期加入的 C11 实现，不能把长期 Python 项目的历史当作它已有多年部署验证。
全新隔离默认构建实际12/12上游 C tests 通过；额外 C `-Wpedantic -Werror` 的全构建
在上游测试宏失败，严格库/C++探针可构建，不伪称全上游严格构建成功。

实际23种 canonical terminal profile、两种 ambiguous policy 的 UTF8/u32 API 对照，
32样本1856次观测、扩展43样本2494次观测；43包含原32，不能称75个独立案例。
`xterm:1` 与旧XTerm372默认32样本相同31项，肤色组合仍不符；
`wezterm:1` 与本机20221119版直接终端43样本相同41项，两个Mc组合仍不符。
没有一组 API profile 完全匹配本机43项；不存在 canonical tmux profile，传入tmux会静默
回退默认，不能因此宣布适配。表按终端名称，不是版本/字体/config的运行时 oracle。
报告与完整差异 `/tmp/chat-t04-libwcwidth-api.SSMbme/REPORT.md`。

实际 WezTerm20221119 / tmux3.6a、43样本、172份TTY DSR和原图保留在
`/tmp/chat-t04-wezterm-repertoire.osfpla`。字体包括 DejaVu Sans Mono、Lily Han Sans
HW SC、Noto Color Emoji，实际图可看到部分ZWJ/肤色合成；终端存在或字体安装不等于验收。
另把C11接到隔离FTXUI完整Screen helper，六种baseline/candidate/default × direct/tmux，
258个完整输出行、516份DSR；目录 `/tmp/chat-t04-width-render.wuvPv4`。
旧家庭行实际只有71列，候选为80列，表明候选能改善确定问题；
但候选direct仍有三行79列，tmux还存在virtual与outer ASCII边界位置不一致。
这些候选使用肤色修复前的冻结源码，不能混作本次最终production。
隔离 sanitizer 的628/80/853是旧原型测试 binary，不是当前新增1096断言或正式全项目门禁。

本次未集成 C11 列库、未改third或用户终端配置。T04继续开放：列模型须同时守住
实际版本/config、完整输出、tmux虚拟格及外层像素、真实输入/选择与换行，
不能用库返回数字或局部carrier通过代替整体证据。Qt暂评87、TUI78不提高，
reference、全页面/无障碍矩阵与最终两轮完整独立fresh review仍未完成。

## 阶段 23：搜索结果的键盘身份反馈

从 `86787802751618a84fd84460d0de31f4042b25d9` 继续。真实双Qt的四档搜索流程
发现Tab/Down后没有当前行标记。独立实际xcb/public-widget探针在100/200%确认：
Tab确实进入结果，Down确实选择第二/第三行，但整个列表像素变化为零；各六条视觉断言
失败、导航断言通过。没有用源码推测或“测试全绿”代替这个反例。

只在message delegate绘制末尾使用现有QAbstractItemView::selectionMode及标准
Selected/HasFocus：可选择的结果行显示克制轮廓，聚焦时轮廓更明确；NoSelection普通
聊天消息完全不变。实际绘制传入的widget在100/200%均为QListView，未凭objectName推断。
没有新增mode/member/index缓存，不改气泡颜色、正文、sizeHint、editorEvent或操作目标。

追加既有delegate/UI测试，保留全部旧断言。16个incoming/outgoing ×宽度320/640×
非零/零origin×DPR1/2组合检查selected/focus/null-widget、正文不重染、轮廓不越行；
普通NoSelection在受测组合逐像素不变。实际搜索dialog的Tab/Down逐行检查旧标记消失、
新标记出现，blur后保留选中身份；三条fixture完整可见，不让滚动变化代替反馈证据。

定向build exit0、delegate CTest1/1、最终offscreen widgets-only全部旧检查和新检查exit0。
独立native100/200%专项各十条断言GREEN，旧RED与图原样保留。
初次新fixture置于旧检查前干扰窗口焦点，移到两条main路径的全部旧检查之后，未删旧断言；
native整widgets200%的旧avatar像素断言仍失败，不冒称整个200% widgets测试通过。
主代理与独立代理亲看100/200%专项完整原图：当前行可辨，正文、avatar、日期未被覆盖。
专项的合成fixture日期不是日常Chat消息日期，不把该helper当作整个产品的使用证据。
独立报告 `/tmp/cpp-width-visual.cg0hjY/qt_search_focus_implementation_review.md`。

### 实际双Qt使用

正式normal客户端SHA `6e9022465a269c7ccbe919dc848d004668b0c2cbd7f810c6c02abbcdaa35df72`，
两个真实Qt完成A100/125/150/200%、peer C100%的原文搜索、Tab/Down、复制、peer编辑/
删除、显式重搜和未发送草稿恢复。第二次driver实际exit0，原文/编辑文本/删除占位的
系统clipboard逐byte一致，目标消息ID不漂移；没有把仍存在的Q21当作通过。
主代理亲看四档真实search焦点原图，独立专项并不代替其他页面/尺寸的完整评审。

| 真实X11搜索列表 | before | after |
| --- | --- | --- |
| 100% | [无当前行标记](images/experience/before-qt-search-keyboard-100.png) | [可见键盘当前行](images/experience/after-qt-search-keyboard-100.png) |
| 200% | [无当前行标记](images/experience/before-qt-search-keyboard-200.png) | [可见键盘当前行](images/experience/after-qt-search-keyboard-200.png) |

before与after为独立隔离数据库的真实相同操作，不是相同消息像素diff；严格同fixture
像素对照由前述public-widget及delegate测试提供。证据目录
`/tmp/chat-quality-search-focus-after-native-final2-20261006`。
首轮driver实际exit1：关闭搜索后枚举旧XID，xprop抛BadWindow；SDK流程已到显式重搜，
该轮不能称四档通过。仅临时driver重核消失窗口，保留失败目录/日志，不修改产品迎合探针。
最终driver仍保留两次窗口消失时的xprop stderr，不冒称所有工具日志零error；
重核窗口实际关闭后完整流程exit0。五个Qt PID已消失、两轮精确自建库SQL count0、
18893无listener，长期服务PID2876288仍存在；五个相关source start/end/current SHA一致。
独立代理亲看四档各focus/button/draft原图共12张，其他截图未冒称全部亲看。

全新Debug Qt/TUI ON `-Werror`构建成功，23/23 CTest通过，103.48s。
最终统一源码实际执行正式 `tests/verify.sh`，Qt/TUI均ON：

| 配置 | build / 全量CTest | 实际耗时 |
| --- | --- | ---: |
| normal Debug | PASS / 23/23 | 105.23s |
| ASan | PASS / 23/23 | 137.10s |
| UBSan | PASS / 23/23 | 130.38s |

本阶段日志 `/tmp/chat-search-focus-final-verify.log`；原runner日志
`/tmp/chat-search-focus-final-validation.log`记录实际 `VALIDATION_EXIT=0`。
最终SSH观察返回255，另读原PID已终止且上述marker/完整三套结果已落盘，未据此重启测试。
没有编译告警、sanitizer报告、skip、suppression或timeout放宽；
fresh与正式Qt编译参数、ASan/UBSan真实插桩另核，不借用上一阶段肤色门禁。

### 新发现的搜索语义缺口仍需修复

旧真实双Qt证据 `/tmp/chat-quality-search-live-native-fourth-20261006` 保存原文、live
编辑/删除、OSclipboard、SDK当前搜索及草稿四档流程：编辑后的正文不含query仍计一行，
删除占位仍计一行，而fresh SDK查询为零。这是Q21，不是本阶段键盘绘制修复的附带收益。
后端每次分页查询排除deleted、使用PostgreSQL字面lower匹配，不提供live search订阅或
跨页不可变事务快照；不得在客户端用另一Unicode casefold冒充服务端匹配。
下一阶段需明确“已加载的查询时命中、正文实时更新、删除不显示、显式重新搜索”，并
证明原始分页cursor、tombstone、迟到页面、原目标复制与草稿不回退，不能只改caption盖住错误。

递归审查还发现T06。临时public state probe加载ID30/20/10，选中20；删除前面的30后
当前索引仍为1、实际ID变为10，exit1；删除后面的10控制组保持20、draft保持不变。
证据 `/tmp/chat-search-selection-state-red.UvQcMF`。不将这项后续修复混入focus提交。
当前没有全页面独立fresh review或屏幕阅读器验收，Qt87/TUI78不提高，整体Goal继续开放。

## 当前原生无障碍登录验证

2026-10-06重新检查，本机AT-SPI组件已安装；此前环境缺bus的历史记录不再作为当前
无法测试的理由。独立新dbus-run-session、自有Xvfb和空XDG配置启动正式normal Qt，
使用前述 `6e902246…35df72` binary，URL为未使用的本地9端口，没有登录、凭据输入、
服务或数据库操作，不连接18080。实际AT-SPI D-Bus desktop树仅有此一个Chat应用，
不是public QAccessible近似读回；capture实际exit0。

原生XTest Tab的实际名称/角色/焦点依序为：用户名(text)、密码(password text)、
登录(push button)、创建账号(push button)、展开或收起服务器设置(check box)。
Space展开后服务器地址(text)为showing/focused，toggle为checked；Shift+Tab焦点返回
toggle，实际focused lost/gained事件与树快照一致。初始showing且focusable的元素无空名称。
隐藏stacked页面仍在树中但not showing，不把存在本身认定为缺陷。

证据 `/tmp/cpp-width-primary.3934Gy/atspi-login` 保存逐stage tree、事件、activation与
实际binary/PID信息；自有Chat按预期SIGTERM/wait=-15，专用bus/Python/Xvfb均已结束，
不是crash。主代理读取原始metadata和独立报告。
此项不证明Orca语音、输入密码后的隐私、label关系、动态错误announce、注册或登录后
完整无障碍矩阵。toggle目前以check box/Checked表达，不提供Expanded，需在真实
屏幕阅读器任务中继续评估，未因此新增状态或无依据地宣告P2。整体评分不提高。

## 阶段 24：搜索命中语义与稳定操作目标

基线 `f35e7290738677dd4f3325761a77369491dad55f`。Q21/T06不是caption装饰：
旧Qt删除占位仍计为搜索行，旧TUI删前项后选中ID漂移，均有实际RED。
本阶段保留服务端字面lower匹配和显式查询，不用前端casefold推导当前server匹配集合。

### 事实与操作身份

Qt的message_model仍是唯一正文、编辑revision、reaction revision和墓碑权威；
message_search_results_model只持有server返回过的ID集合，筛选membership且not deleted。
未知完整live正文/墓碑先进入既有model但不自动取得命中身份；未知reaction仅保存最高
revision，完整正文到达后消费，不能制造空正文替代真实消息。刷新清除本次pending facet。
next_before取当前原始页的合法最小ID，不取可见行或无关live缓存的first ID；
整页被墓碑过滤、甚至可见集合为空时仍保留真实的更早页入口。

持久说明、请求反馈、可见命中数量是三个不同label；没有新增pending/failed/received/
fresh-empty四个bool。live事件仅更新正文、可见数量和选中目标，不抹掉loading或error。
Qt/TUI均瞬时捕获当前ID，容器变化后按ID恢复；当前项删除才选择邻项，空集合安全清除。
Qt嵌套菜单事件循环之前捕获复制正文，即使目标删除且迟到页插入，也不改复制对象。
TUI搜索明确显示Loaded hits/live text及重新搜索提示，不把“没有已加载命中”称作全局空匹配。

永久测试追加未知edit/delete/reaction、独立清空revision、迟到初页/旧页、原始cursor、
all-filtered分页、同query刷新与tuple fencing、selected ID、邻项和已开菜单复制。
TUI补前/后/当前/最后/最终空删除、真实cursor以下的迟到页、history/query/draft隔离。
原控件和状态断言保留。Qt最终永久GREEN日志
`/tmp/cpp-width-primary.3934Gy/q21-permanent-final-green.log`；TUI RED/GREEN存于
`/tmp/chat-t06-selection-evidence.YdtU73`。最初TUI RED快照的迟到页fixture曾包含高于cursor
的ID，最终修正为5/2小于10，原选中身份RED不变；不伪称前后fixture清单完全相同。
TUI caption/render先RED后GREEN，targeted state/render/unicode/display/span实际5/5。

### 双Qt真实使用与HiDPI

最终normal Qt SHA `8445d49c0ba33f1076fa8981eb16efe5457f67a8b1010833df15da1c42c69630`。
A100/125/150/200%、peer C100%，五次真实/proc binary均相同。final3 driver实际exit0，
每档原文复制和live编辑后正文复制逐byte匹配OSclipboard；编辑不含query仍是已加载命中，
fresh SDK查询为零；peer删除后AT-SPI列表实际无行；显式重新搜索返回权威空结果。
草稿未重贴，原生返回composer后真实发送与SDK正文/身份一致。每档只有一个loaded hit，
多行/分页/重入竞态属于永久public-widget覆盖，不冒称native容量或全屏幕阅读器验收。

| 真实搜索列表 | before | after |
| --- | --- | --- |
| 100%删除 | [占位仍计一行](images/experience/before-qt-search-live-excluded-100.png) | [删除项真正排除](images/experience/after-qt-search-live-excluded-100.png) |
| 200%删除 | [占位仍计一行](images/experience/before-qt-search-live-excluded-200.png) | [删除项真正排除](images/experience/after-qt-search-live-excluded-200.png) |
| 200%编辑 | 原始命中正文实时改变 | [保留live正文及诚实数量](images/experience/after-qt-search-live-edited-200.png) |

before和after来自独立隔离数据库，不是逐像素同fixture对比。主代理亲看最终200%编辑/
删除和150%重搜原图；独立代理亲看final3四档edit/delete/fresh-empty/focus/draft共20张，
并独立解析20份AT-SPI JSON、八份clipboard、四组SDK目标/草稿。报告
`/tmp/chat-q21-independent-review-20261006.md`未发现本专项material回归，不是全产品fresh review。
fresh-empty仍同屏显示“没有匹配的消息”和“没有已加载的搜索命中”，两条近义提示略重复，
记录轻度展示polish候选，不借测试GREEN宣布所有视觉缺口消失。

final3证据 `/tmp/chat-quality-search-live-policy-native-final3-20261006`。首轮实际exit1为
关闭时xwininfo旧XID枚举失败；final2实际exit1为150%复制坐标来自AT-SPI逻辑extent，
未转换成XTest物理坐标，clipboard仍sentinel。final3按actual X11窗口与accessible dialog
的相对extent换算，不猜固定缩放倍数、不弱化原文assert，产品未为此修改。
失败目录和日志保留；最终仍有瞬时BadWindow枚举stderr，不能称所有工具日志零error。
三个精确自建库SQL为零，11个记录Qt PID全部消失，18894无listener，源码start/end/current
SHA一致，受保护长期服务2876288存在；不把这一PID检查外推到未逐个记录的所有工具进程。

### 双TUI真实选中身份

normal TUI SHA `39910f6a2efcd06ae7a50cd3ab1f48a754a24f03ee37b50496656a11b97ef95a`。
60/70/80/100/120/160×30六条真实双客户端流程：peer发送LOW/MID/TOP，A选MID，
peer确认删除TOP后A仍以inverse选MID；打开Copyable text取得完整UTF8正文，返回原草稿
不重贴发送，SDK逐byte及唯一ID核验。此处是terminal可选择正文，不是系统clipboard测试。
最终after actualexit0；旧冻结binary60×30同动作actualexit1，选中MID3漂到LOW2，
原styled失败capture保留。旧binary的runtime source快照是当前working tree，不能冒称
旧编译源码；独立state RED另有旧源码冻结证据。首个driver因prefix超过24字符actualexit1，
未登录或运行业务；只修临时夹具，不把它和产品RED混为一类。

| styled tmux证据 | 内容 |
| --- | --- |
| [before60列](images/experience/before-tui-search-live-selection-60.ansi) | 删除前项后错误选中LOW |
| [after60列](images/experience/after-tui-search-live-selection-60.ansi) | 删除前项仍选中MID |
| [after160列](images/experience/after-tui-search-live-selection-160.ansi) | 同目标和live说明保持 |

ANSI是带样式pane capture，不是outer terminal像素。主代理读60/160，独立代理读九份
after和两份before；ZWJ行仍有额外右边框，明确不称完整render/shaping PASS。
目录 `/tmp/chat-quality-t06-search-selection-native-final2-20261006`及before目录保存身份/
正文/源码/进程证据，三个精确库SQL为零，八个记录业务PID消失、18895无listener，
专用tmux无server，长期服务未动。

### 统一正式验证与剩余目标

最终八个源码SHA与formal source guard一致。全新Debug Qt/TUI ON `-Werror`
`/tmp/chat-search-live-fresh.l2VXbq`全构建及23/23 CTest通过，104.74s。
正式 `tests/verify.sh` 的原runner实际终止exit0，记录 `VALIDATION_EXIT=0`：

| 配置 | build / 全量CTest | 实际耗时 | tui_render（原5s） |
| --- | --- | ---: | ---: |
| normal Debug | PASS / 23/23 | 105.25s | 0.85s |
| ASan | PASS / 23/23 | 140.13s | 4.02s |
| UBSan | PASS / 23/23 | 132.08s | 1.74s |

日志 `/tmp/chat-search-live-final-verify.log`、runner
`/tmp/chat-search-live-final-validation.log`。无编译告警、sanitizer报告、skip、suppression
或timeout放宽，实际ASan/UBSan插桩另核，不借上一阶段结果。
最终源码真实Qt/X11导航4/4、TUI/tmux导航11/11，各driver actualexit0，
目录 `/tmp/chat-quality-search-live-qt-navigation-20261006`、
`/tmp/chat-quality-search-live-tui-navigation-20261006`；这是导航范围，不是完整日常使用。
独立cleanup checker实际exit0：Qt库`chat_qtx11_1006031007_144761`和TUI库
`chat_nav1006031008_144763`均SQL count0，18896/18897无listener，两个记录driver PID消失，
自有log/DB/port关联活进程为空，专用TUI socket无server，长期2876288仍存在。
Qt成功finally本来不写database-cleanup.txt，未因缺marker重启或伪造逐child PID清单；
报告 `/tmp/chat-search-live-navigation-cleanup-review-20261006.md`明确验证范围。

本次不修改server/client/SQL/third、无migration，无push。Q21/T06专项收口，
Qt87/TUI78暂评不提高；T04 outer列与font shaping、全页面/完整日常使用/无障碍矩阵、
最终两轮独立全产品fresh review仍缺证据。总体品质Goal继续推进，不标记完成。

## 阶段 25：操作目的与单一搜索反馈

基线 `00dff64089551e3e452d661162255de462079b05`。原生AT-SPI旧树中个人资料、
会话/消息列表、搜索输入/结果名称为空；不把placeholder或tooltip默认当作可读取目的。
本阶段为这些入口及同构的联系人/添加好友输入和结果，共九个控件设置稳定名称。
个人资料入口仍保留真实用户名tooltip，clicked所用身份未替换成动作文案。
没有新增状态、wrapper、几何或导航变化。

搜索在初始提示、loading、空query、初页error和fresh-empty时仅显示请求反馈；
删除最后已加载命中后仍显示独立的重新搜索提示，而不冒充服务端全局空结果。
非零loaded count与旧页error是两个不同事实，继续同时显示。
可见性直接取已有status可见状态和真实proxy行数，不保存另一份pending/empty bool。
永久widgets回归两项分别实际RED exit1后GREEN exit0，原断言均保留；覆盖初始、
loading、unknown live事件、初页error、empty query、fresh-empty、live删除归零及旧页error。
日志 `/tmp/chat-search-feedback-{red,green}.log`、
`/tmp/chat-operational-names-{red,green}.log`。

### 原生双Qt与视觉证据

正式normal Qt SHA `c3081ac1c633582b93fb45bf4f71313d7572407b041ed953fe45def0534b0946`。
final4同一轮A100/125/150/200%、C100%，五次实际/proc SHA一致，driver/bus实际exit0。
每档真实编辑/删除、原文和编辑正文OSclipboard逐byte核对、SDK当前查询、返回未重贴
草稿并真实发送均通过；loaded行1→1→0→0。原生树分别确认输入text、列表list和
资料push button名称；100%额外Contacts/AddFriend及资料执行实际AT-SPI Press成功，
资料页显示真实用户名。不是仅从QWidget属性读回，更不是Orca语音或完整动态announce验收。

| 原生搜索状态 | before | after |
| --- | --- | --- |
| 100% fresh-empty | [两条近义反馈](images/experience/before-qt-search-single-feedback-100.png) | [仅权威空结果](images/experience/after-qt-search-single-feedback-100.png) |
| 200% fresh-empty | [两条近义反馈](images/experience/before-qt-search-single-feedback-200.png) | [仅权威空结果](images/experience/after-qt-search-single-feedback-200.png) |

before来自上一阶段隔离数据库，不是同fixture逐像素差异。主代理亲看final4这两张、
200%live删除和100%资料原图；独立代理亲看四档edit/delete/fresh-empty/result-focus/
button-focus/draft共24张，加资料/联系人/添加好友3张，未见本专项material回退。
证据 `/tmp/chat-quality-q22-native-final4-20261006`，execution和independent-verification
保存真实SHA、动作、SDK、clipboard和清理结果。最初三轮driver失败依次为新bus树尚未
注册、已销毁model节点optional action getter、launcher未加载PG环境；未据此修改产品。
临时reader采用有界等待注册、记录optional getter错误，必需名称/角色/动作断言没有删。
瞬时XID BadWindow重枚举日志保留，不声称全部工具日志零error。

四个精确自建库均SQL count0，四轮记录的PID全部消失，18898无listener；final4 source
start/end/current一致，长期用户服务2876288仍在。Qt按预期SIGTERM/wait=-15清理，
不将其称为crash。专项仍发现Q23：消息行Name只有正文、Description空，也没有sender/
time子节点；接下来处理可读取消息上下文，不把当前Name修复替代全无障碍矩阵。

### 当前源码完整门禁

全新Debug Qt/TUI ON `-Werror`目录 `/tmp/chat-q22-fresh.oudW8V`构建和23/23 CTest通过，
107.43s。正式 `tests/verify.sh` Qt/TUI ON，原runner实际exit0，
`/tmp/chat-q22-final-validation.log`记录 `Q22_VALIDATION_EXIT=0`且三份source SHA guard成功。

| 配置 | build / 全量CTest | 实际耗时 | tui_render（原5s） |
| --- | --- | ---: | ---: |
| normal Debug | PASS / 23/23 | 106.49s | 0.84s |
| ASan | PASS / 23/23 | 144.97s | 4.02s |
| UBSan | PASS / 23/23 | 134.25s | 1.74s |

日志 `/tmp/chat-q22-final-verify.log`；实际插桩与严格编译参数另核，未放宽timeout、
skip或suppression。当前源码Qt/X11导航4/4、TUI/tmux导航11/11各actualexit0。
目录 `/tmp/chat-quality-q22-{qt,tui}-navigation-20261006`；主代理只读cleanup checker
actualexit0，精确库`chat_qtx11_1006034252_197085`/`chat_nav1006034253_197248`均为0，
18899/18900无listener，两个记录driver PID消失，自有DB/log/port关联活进程为空、
专用tmux socket无server。没有historical child PID清单，不伪称逐child核验。

T04通用malformed modifier候选隔离研究保留于
`/tmp/chat-t04-contextual-skin-candidate.2t19yN/REPORT.md`：官方Unicode18属性和紧邻关系
可解决非base前缀/非法插入关联，但合法ZWJ/emoji在不同outer terminal仍有宽度残留。
候选尚未进入正式构建，不把其临时GREEN记作T04完成。本阶段无server/client/SQL/third
改动、无migration或push；Qt87/TUI78不提高，完整日常使用和两轮全产品fresh review继续开放。

## 阶段 26：原生消息上下文

基线 `6519e9d3abe4051636a167df5b77bd40dd87802b`。旧原生消息/搜索行的Name只有正文，
没有发送者和时间子节点。本阶段采用Qt官方
[AccessibleTextRole](https://doc.qt.io/qt-6/qt.html#ItemDataRole-enum)：从现有message_model
的消息、引用、附件、mention、reaction和读者roles按需生成可读取上下文，不建立第二份
无障碍snapshot。自己的发送者为“你”，有效时间含本地日期与分钟；已编辑、提及你、
自己的已读和反应使用已有事实。正文DisplayRole/text_role、OS复制、动作目标和geometry不变。
删除后的Name仅保留身份、时间和墓碑，不泄露原文、引用、文件名、提及、反应或编辑标记。
读者/member/reaction的dataChanged通知同步包含AccessibleTextRole，没有增加状态或wrapper。

### 永久回归与真实bridge的边界

模型新增断言先actual RED1，再GREEN0：发送者/日期/正文、原文复制不变、引用/编辑/
stale revision、提及、反应、附件、删除隐私、group/direct读者、无timestamp不造1970。
widgets通过公开QAccessible table/list item验证初次和同model live编辑；late page/reset
继续核验实际proxy的canonical role，原断言保留。
日志 `/tmp/chat-q23-model-{red,green}.log` 和
`/tmp/chat-q23-permanent-final-{model-green,ui-green}.log`。

一次offscreen尝试在model reset后读缓存cell得到空Name，而model role正确；
`QAccessible::setActive(true)`仍不能建立原生platform bridge，诊断actual1保留。
实际Qt为6.2.4；[该版本官方源码](https://code.qt.io/cgit/qt/qtbase.git/plain/src/gui/accessible/qaccessible.cpp?h=v6.2.4)
表明model-change accessibility事件只有active bridge才交给cache。
最终没有强制active、人工modelChange或删除Qt缓存的产品/测试workaround。
reset后的真实Name必须由实际AT-SPI补证据，不能用model属性测试替代。

### 双Qt四档原生验证

`/tmp/chat-quality-q23-native-final3-20261006`为同一完整成功轮，driver/bus actualexit0。
A100/125/150/200%、C100%，五次实际/proc binary SHA均为
`a97470a36430f75d555aab378559a4cb0dd47bc294414c0ab1b42f5c3d64a6ad`。
每档在命名的消息记录/搜索结果直接子树定位真实消息，核验sender/date/reply/mention/
reaction以及自己消息的已读；同dialog重新搜索非空、live编辑后真空再新query真非空，
重新建立的原生Name仍正确。删除后搜索无行，消息记录墓碑无旧上下文。
100%另通过SDK真正发送/删除带引用与反应的附件，核验文件名不从墓碑泄露；
这不是完整附件上传/下载UI验收。Description可为空，完整上下文由原生Name读取。
OSclipboard逐byte仍为原文/编辑正文；未重贴草稿恢复并真实发送、SDK查询、焦点/操作
名称及Contacts/AddFriend/profile真实Press均保留严格断言。独立verification actual0。

主代理亲看[100%重新搜索](images/experience/after-qt-message-context-100.png)、
[200%重新搜索](images/experience/after-qt-message-context-200.png)、150%滚到底的消息区及
100%附件墓碑原图。可见搜索选择/正文/反应层级未因Name改变而改变；截图不是无障碍证明，
实际节点和SDK对应记录才是。本专项不宣称Orca语音、动态announce或全页面可访问性通过。

前两完整driver actual1不拼接到成功轮：首轮150% target底部未全滚入，旧CtrlEnd操作
没有达到viewing_latest；第二轮临时arrival断言误放附件helper，等待已删的普通target。
最终仅修/tmp驱动，先在实际A消息子树确认目标到达，再公开wheel到底，保存前后原图及
scrollbar查询。原生树未暴露vertical scrollbar Value节点，不能称数值达到maximum；
原15s SDK read-position断言不删除、不延长。各轮driver/reader快照保留。
三精确自建库SQL count0，各轮记录PID均消失，18901无listener；成功轮source首尾/current
一致，长期2876288未动。受控Qt SIGTERM不是crash。

### 当前冻结源码完整门禁

Qt三文件与同期TUI十文件共13份source SHA guard一致；以下是此冻结组合的正式门禁，
Qt和TUI实现没有相互依赖。全新Debug Qt/TUI ON、`-Werror`目录
`/tmp/chat-context-fresh.U3SIBY`全构建和23/23 CTest通过，109.59s。
`tests/verify.sh`原runner actualexit0，`CONTEXT_VALIDATION_EXIT=0`：

| 配置 | build / 全量CTest | 实际耗时 | tui_render（原5s） |
| --- | --- | ---: | ---: |
| normal Debug | PASS / 23/23 | 104.67s | 0.85s |
| ASan | PASS / 23/23 | 135.04s | 4.06s |
| UBSan | PASS / 23/23 | 129.09s | 1.76s |

正式compile command与实际asan/ubsan符号另核。fresh使用-Werror，正式脚本保留原
-Wall/-Wextra/-Wpedantic；未谎称正式三模式均使用-Werror。日志
`/tmp/chat-context-final-{validation,verify}.log`无编译告警/sanitizer报告，未skip或放宽超时。
同期fresh binary真实Qt/X11导航4/4、TUI/tmux导航11/11，各原handle actual0；
`/tmp/chat-quality-context-{qt,tui}-navigation-20261006`。主只读cleanup checker actual0：
精确库`chat_qtx11_1006041135_250442`/`chat_nav1006041137_250513`均SQL count0，
18903/18904无listener，记录driver PID消失、自有DB/log/port关联活进程为空、专用tmux无server。
未保存historical child PID清单，不冒称逐child核验。

仍需后续核验：可见group消息的已读人数（包括peer和0）与完整无障碍metadata的等价性、
keyboard-to-latest、Qt复杂emoji font/shaping。新原图仍可见woman/laptop分开，原始UTF8
正确不等于字形正确。Q23仅关闭已确认的sender/time上下文缺陷；Qt87/TUI78不提高，
T04、完整参考/全页面/日常使用/动态announce矩阵和连续两轮全产品fresh review仍开放。
本阶段无server/client/SQL/third改动，无migration或push。

## 阶段 27：肤色修饰符的局部显示关系

接续阶段26的冻结验证，把阶段25隔离研究中通用malformed modifier修复正式接入
现有FTXUI维护补丁。不是只处理空格的特例，也没有手写emoji白名单。
Unicode18官方[emoji-data](https://www.unicode.org/Public/18.0.0/ucd/emoji/emoji-data.txt)
完整原文件、派生表与Unicode License V3入仓；生成器离线、标准库、确定性stdout，
只提取51 ranges/136 codepoints的Emoji_Modifier_Base和5个Emoji_Modifier。
数据SHA `80d00f8e…33bf1`、表SHA `b200a53e…e6937`，完整pin和再生成方式见
`tui/cmake/README.md`。CMake正常配置不依赖Python，验证data/table与utf8proc源码pin，
COPYONLY表进入既有screen翻译单元；离线source override也不能绕过API/Unicode版本。
TUI OFF全新配置actual0，无property模块执行或utf8proc下载。未改变third子模块/gitlink。

显示策略依据[UTS#51 revision31 section2.4](https://www.unicode.org/reports/tr51/tr51-31.html#Diversity)：
只承认紧邻modifier-base或中间恰好一个VS16的既有例外；Mn、VS15、第二VS16、ZWJ、
前一个modifier均中断局部关联。同一EGC较早出现base不足以让后面的肤色合法。
RGI集合与属性结构集合不是同一约束，保留FAMILY的属性会员控制，不混成RGI白名单。
两个局部变量只活在当前显示调用栈，无长期snapshot/cache。非法附着肤色只在display
替换成载体；source/raw EGC、Input编辑、发送、选择复制的原UTF8仍不变。
正常space+Mn、合法手势/VS16/ZWJ与纯leading modifier既有单载体行为保留。
本次没有改变DisplayWidth/CellToGlyphIndex等旧列策略，不能称完整emoji列修复。

### 永久独立回归与配置验证

新增23组literal display fixtures，不从实现反推expected；包含所有五肤色、A/标点/
NBSP/空格、插入Mn/VS15/两VS16、ZWJ的局部关系、多个modifier及新Unicode18base。
全部fixture另外核验head/continuation span、任意半格选择仍复制完整raw EGC。
13组Input测试逐byte核验ArrowLeft/Right/Delete/Backspace；原官方853条分界与全部旧断言保留。
旧生产patch+新增测试actualRED1（2788 assertions、465 failures），最终GREEN0
（2788/0）；unicode/display/span 3/3 actual0。generator --check exact0，
错误checksum输入明确reject1，官方许可原文独立fetch/hash一致。
定向fresh证据 `/tmp/chat-t04-contextual-production.UqnATo/REPORT.md`，不是借旧隔离候选GREEN。

### 新生产archive链接的原生终端

`/tmp/chat-t04-contextual-production-native.cDWC2C`使用新正式构建的component/dom/screen/
utf8proc archives正常链接helper，无额外对象override；对应生成string源码SHA首尾一致。
18fixtures × before/after × XTerm372/WezTerm20221119 × direct/tmux3.6a，共144行，
每次实际helper/proc SHA、原始DSR、outer X11原图均保存；driver actual0和八term actual0。
独立像素读取真实外层竖线，不把tmux虚拟DSR代替宿主字形宽度。

| 四环境证据中的两个例子 | before | after |
| --- | --- | --- |
| XTerm direct | [原错列](images/experience/before-tui-contextual-skin-xterm.png) | [非base载体归位](images/experience/after-tui-contextual-skin-xterm.png) |
| WezTerm + tmux | [原错列](images/experience/before-tui-contextual-skin-wezterm-tmux.png) | [非base载体归位](images/experience/after-tui-contextual-skin-wezterm-tmux.png) |

主代理亲看这四张原图。前五类nonbase关系after在四环境DSR与outer像素均为80列，
但合法hand在XTerm仍82列，合法ZWJ在WezTerm仍77列等原失败完整保留。
这里actual0表示探针执行和本次context normalization核验成功，不表示144行全部达到80。

### 双真实TUI、原文与草稿

fresh全项目binary `/tmp/chat-context-fresh.U3SIBY/chat_tui` SHA
`32954f6ad547081972403cf737aa7fb1ca5f8cbb847bb79975d65bc1e5c62ba8`。
独立双客户端+SDK在60/80/160列、30行，每宽实际发送六种raw UTF8正文并检查peer，
选择消息进入copyable页，切会话恢复未发送draft再真实发送；共21facts，SDK正文逐byte
与原输入相同，四业务/proc SHA记录一致。`/tmp/chat-t04-contextual-production-wire-20261006`
driver actual0。copyable display载体可见不等于OS原始clipboard读回，未冒称已验收后者。

另完整fresh轮`/tmp/chat-t04-contextual-production-wire-camera-20261006` actual0，
同21facts并保存39张actual app outer原图；主亲看60载体选中、80copyable、80draft恢复、
160合法ZWJ选中。载体正文/边框稳定，可见[60列实机](images/experience/after-tui-contextual-skin-app-60.png)。
但[160列合法ZWJ](images/experience/tui-valid-zwj-width-open-160.png)仍有右边界和ENDN残留，
80列原图也可见旧列/shaping/incremental repaint问题。该确定缺口继续归T04，不删失败图。
摄影轮三个xterm受控terminate实际15，driver0/Xvfb0；不写成所有进程自然exit0。

主独立只读checker `/tmp/chat-context-tui-evidence-review.py` actual0：核helper144行、
两轮21facts原bytes、outer前五类列归位、16个native记录PID和8个业务PIDgone，
两精确库`chat_cm_10060414_258811`/`chat_cm_10060417_266137`SQL count0，18902无listener，
三个自有tmux socket无server，长期2876288仍在。报告
`/tmp/chat-context-tui-evidence-review-20261006.json`；仅精确自有范围，不触碰tmux0。

### 门禁与开放项

阶段26记录的同13份冻结源码fresh -Werror及正式normal/ASan/UBSan门禁覆盖本实现，
源码guard再次核验一致：各23/23、原runner actual0，无告警/sanitizer报告/skip/超时放宽。
同期Qt4/4、TUI11/11导航也使用新fresh binary，不借旧候选运行结果。
本次只收口通用malformed modifier显示关系，T04合法emoji/ZWJ列、字体与增量重绘仍开放；
不硬编码terminal名/emoji宽度、不修改用户终端配置。主后续将继续从真实terminal链寻找
维护中的cluster-width策略并验证原bytes/列/光标，而不是把宿主差异当成验收豁免。
Qt87/TUI78不提高，完整reference/页面/日常使用/无障碍/terminal lifecycle矩阵和最终
连续两轮独立全产品fresh review仍未完成。无server/client/SQL/third改动、无migration或push。

## 阶段 28：可见群读者信息与原生上下文一致

基线 `6318d8653f2516e643f8ac35440cf5d6d103a3d7`。delegate既有read_count_role对群消息
显示已读人数，包括incoming和0；原AccessibleTextRole仅描述outgoing且read=true，
两种呈现并不等价。本次直接复用同一read_count_role：valid时描述人数，否则保留单聊
自己的已读事实。没有新增状态、计数缓存或server规则，也不改变正文、复制或布局。
删除墓碑仍不描述已读/引用/附件等原上下文。

### 永久回归

新增own0与peer0/1断言在旧实现actualRED1，修复后models actual0、widgets actual0；
已有direct回退、编辑/删除隐私、reaction与dataChanged roles断言保留。日志
`/tmp/chat-group-read-context-{red,green,ui-green}.log`。

### 新binary双Qt与独立计数

`/tmp/chat-quality-q24-native-final3-20261006`同一完整轮driver/bus actual0，
A100/125/150/200%、C100%五次/proc SHA均为
`4d2ea422be432f498c3226842767ff7ff3c6e2b5e5485fa5d3251f596772eca6`。
每档own0、incoming0、同一incoming真实read-position更新为1、原target当前计数共16组，
从SDK members/read_positions独立计算，排除当前self，与原生Name精确一致；不从Name反推
expected，也不把消息author错误当成排除对象。主独立checker actual0：
`/tmp/chat-q24-independent-count-review-20261006.json`，同ID/同原生path live0→1成立。

100/125/200%的新增0/1目标实际可见；150%两次对应节点showing=false且原图位于视口外，
只证明exposedName，不称四档新增live1可见。该档随后原target当前0实际可见，是另一条
证据，不替代同一消息live1。主亲看[100%窗口0](images/experience/after-qt-group-read-zero-100.png)
与[200%live1](images/experience/after-qt-group-read-one-200.png)原图，可见文字与Name一致，
布局未改变；既有woman/laptop拆画仍可见，字体/Unicode专项继续开放。

原Q23/Q22严格链保持：同dialog重新搜索非空/live编辑/删除/墓碑隐私、12份OSclipboard
原bytes、未重贴draft恢复真实发送、SDK目标ID与操作Name/Press。独立verification actual0。
前两driver actual1分别误把send摘要当fullmessage、未透传target的mention/reaction上下文；
仅修/tmp驱动，不修改产品或弱化15s/SDK/人数断言。三精确自建DB count0、所有记录PID
消失、18901无listener，source首尾/current一致，长期2876288未动。受控Qt SIGTERM不是crash。

### 门禁与未完成边界

本阶段全新Qt/TUI ON Debug -Werror首次build actual2，两个编译单元无法向/tmp写汇编，
实际为磁盘耗尽。失败日志`/tmp/chat-read-context-fresh-build.log`保留；只移除主先前两套
已完成临时构建的六个可重建object目录，保留binary、JSON、原图和日志。资源回收后在同一
fresh目录`/tmp/chat-read-context-fresh.JwLH7s`续构actual0，全量23/23 CTest actual0，108.57s。
原`tests/verify.sh`此次actualexit0，`READ_CONTEXT_RESUME_VALIDATION_EXIT=0`，13份source
guard再次一致。正式编译保留原-Wall/-Wextra/-Wpedantic，实际sanitizer flags与符号另核；
只有fresh单列-Werror，不借阶段26门禁，也未放宽render原5s或skip任何测试。

| 配置 | build / 全量CTest | 实际耗时 | tui_render（原5s） |
| --- | --- | ---: | ---: |
| normal Debug | PASS / 23/23 | 105.84s | 0.85s |
| ASan | PASS / 23/23 | 143.36s | 4.00s |
| UBSan | PASS / 23/23 | 130.96s | 1.76s |

日志`/tmp/chat-read-context-final-verify.log`与`/tmp/chat-read-context-resume-validation.log`
保留，无新增编译告警/sanitizer报告。既有TUI源码不变，未将上一阶段TUI原生运行冒充本次新轮。

Q24计数上下文专项收口不等于全无障碍验收。主消息列表真实keyboard菜单/可见focus、
全页面完整日常任务、Orca与动态announce仍缺完整证据；T04合法emoji列/shaping仍开放。
Qt87/TUI78不提高，最终两轮独立全产品fresh review尚未开始。无push、无third改动。

## 阶段 29：主消息列表的可见键盘目标

基线`660b739025cfa7cb7d5a4fad6e8a5f1131a846d4`。100%两真实Qt探针从focused composer
公开ShiftTab/Home/Down逐行到SDK原目标ID4，原生focused/showing=true、selected=false，
原图却没有当前行提示：[before](images/experience/before-qt-main-message-keyboard-focus-100.png)。
确定问题是NoSelection同时禁止绘制HasFocus，不是键盘不能定位。
本次仅让HasFocus使用既有绿色轮廓；Selected-only仍遵守NoSelection，未聚焦气泡/正文/
布局不变，没有加入选择状态、菜单封装或新快捷键。

永久delegate矩阵先actualRED1，再GREEN0；同时保留普通/搜索selected/null-widget、正文
不变、轮廓不逃出row、宽窄/origin/1x和2x/outgoing与incoming所有旧约束。新增NoSelection
HasFocus与search同一焦点轮廓，unfocused与Selected-only仍等原plain。widgets实际0。
一次直接调用ui_test未提供其必需argv，进入main前返回1；随后按真实API --widgets-only
实际0，未修改产品去迎合错误调用。日志`/tmp/chat-message-focus-{red,green,ui-widgets-green}.log`。

### 原生失败与判定纠正

before`/tmp/chat-quality-message-keyboard-native-20261006`driver actual1：collector未见
AT-SPI menu items而误判Menu/ShiftF10没打开，原X11图实际有回复/置顶/回应/已读菜单。
主和独立代理亲看后撤销菜单不可达产品判断，只保留真实focus缺口；不修改chat_widget。
两SHA4d2…、原target树逐次变化、原图、driver-at-run与失败实际清理证据全部保留。

新正常Qt SHA`303ff6b0986049ca49706b124e9e87b1cdf9903f121190b41eae3c92d76bd63e`，
[after100原图](images/experience/after-qt-main-message-keyboard-focus-100.png)已有绿色当前行
轮廓。但首次after driver也actual1：X11窗口matcher把raw
regex写成双反斜线，真实保存tree上原matcher0项，修单转义后9项且唯一popup尺寸匹配。
这是/tmp探针错误，不据此修改菜单、删SDK原ID断言或称后续业务已通过。

### 当前验收范围

沿用上一阶段fresh目录增量构建-Werror后全量23/23 CTest实际0，104.81s；不是另一次
全新配置。原tests/verify.sh actual0，15份source SHA guard首尾一致；无放宽render5s、
skip或sanitizer workaround。

| 配置 | build / 全量CTest | 实际耗时 | tui_render（原5s） |
| --- | --- | ---: | ---: |
| normal Debug | PASS / 23/23 | 109.64s | 0.86s |
| ASan | PASS / 23/23 | 141.16s | 4.11s |
| UBSan | PASS / 23/23 | 137.22s | 1.77s |

日志`/tmp/chat-message-focus-{validation,final-verify}.log`，实际marker
`MESSAGE_FOCUS_VALIDATION_EXIT=0`，严格目录-Werror与正式脚本原warnings分别记账。
新完整100%`/tmp/chat-quality-message-keyboard-after-native-final3-20261006`driver actual0：
reply原ID/原draft、👍同原ID、read-details真实S005、后draft原bytes全部同轮严格通过。
第二次after actual1实际给同目标加了❤️：Right进入submenu时已经选择首项，额外Down移动
到第二项；原图和SDK一致，属于driver输入错误。仅/tmp改Home明确到首项，再Return，
没有弱化👍/原ID期待或修改菜单。此前actual1与source-at-run保留，不拼为成功轮。

四轮独立新环境分别为A100/125/150/200%、每轮C100%；实际execution
35290/5946/84954/7745全部terminal0，八个/proc binary SHA均为303ff6…。
125/150/200每轮都完整执行公开键盘定位、Menu、reply原ID4/原draft、👍同ID4、真实S005
读者和后draft精确OSclipboard；随后resize到980×640逻辑尺寸，实际物理尺寸分别为
1225×800、1470×960、1960×1280，再用公开按键定位同原ID并核draft。
100%未做这次最小尺寸步骤，不借后三轮替代。Menu实际成功，备用ShiftF10未执行，
不称两种快捷键都已验证。菜单AT-SPI items不可用，实际owned mapped X11 popup与
公开按键、SDK原ID是菜单判定依据；剪贴板字节在runtime严格断言，未另外保存raw文件。

主独立checker actual0：`/tmp/chat-message-focus-independent-review-20261006.json`，
核各轮actual exit、11份源码首尾/current一致、八process SHA、回复/回应/读者原ID与
15次原生current focused/showing且not-selected；缩放三轮driver snapshot SHA完全一致。
100%复制的snapshot多一个EOF空行，初次checker因此actual1；实际launcher记录SHA与原
driver当前SHA一致，精确字节关系为snapshot=original+一个LF，完整diff另存，没有覆盖
原证据或模糊normalization。只有明确核验此字节关系后checker actual0，不归为产品错误。

四档路径`/tmp/chat-quality-message-keyboard-after-{native-final3,125-native,150-native,200-native}-20261006`；
汇总`/tmp/chat-message-keyboard-scaled-native-REPORT.md`。主亲看100% current/read/draft、
125/150 normal/min-current与200% normal/min-current/read原图，轮廓清晰、不越row，
resize后reply/reaction/draft仍自然换行：[200%最小窗口](images/experience/after-qt-main-message-keyboard-focus-minimum-200.png)。
独立cleanup actual0：`/tmp/chat-message-keyboard-scaled-cleanup-independent.json`，四精确
自建库SQL count0、全部记录descendants消失、18905无listener、长期2876288仍在。
Qt受控-15属于清理，不冒称客户端自然退出0。

本次仅收口主消息当前行可见focus与上述三条键盘任务，不是全页面/完整键盘或Orca验收。
Qt87/TUI78不提高，T04、全页面/完整日常使用/无障碍与最终两轮fresh review继续开放。

## 阶段 30：列表的键盘激活与当前目标一致

基线`a5fe13eab6adcbc1e7aa938a538422d7c0f7928b`。独立真实双Qt探针先鼠标打开群，
随后通过公开Home/Down/Return选择历史B、联系人S005和搜索结果D。完整before final5
driver actual1，三项目标确实focused/showing，前两项selected=true，搜索结果的current
focused但selected=false。三次Enter均未打开目标，右侧仍是旧群；不是仅根据源码缺少
activated连接判BUG。[before原图](images/experience/before-qt-list-keyboard-enter-100.png)。

本次只连接三个QListView的原生activated到既有select_conversation/select_contact/
select_add_user。保留鼠标body/avatar分工、proxy映射、好友与只读规则，不增加状态、
键盘框架或后端接口。永久public键盘事件回归先actualRED1，再GREEN0，分别覆盖Return
和小键盘Enter的精确user/conversation，以及搜索资料恰好打开一次；旧单击body/头像
不串动作等全部widgets约束仍通过。日志`/tmp/chat-list-activation-{red,green}.log`。

新正常Qt SHA`33493abfbf69cf9202d0e62a6f36c4345a98d260f2a7aacb4c3edf188a858fdc`，
after final2两Qt实际/proc exe字节在运行时均严格核验，完整driver/bus actual0。历史B
打开SDK原conversation2并显示旧正文/只读解释；联系人S005打开正确header，独立已认证
S005的SDK同时出现与A的可写direct3；搜索D通过Tab/Home/Return打开同名pending资料，
未改变关系。[历史after](images/experience/after-qt-list-keyboard-enter-100.png)、
[搜索资料after](images/experience/after-qt-user-search-keyboard-enter-100.png)。主亲看历史
before/after、联系人after及双窗口/资料原图，动作、身份、只读与关系呈现相符。
原生这轮只验证100%的Return；小键盘Enter来自永久widget测试，不冒称四档原生或
完整Tab/Orca覆盖。没有直接emit信号、注入currentIndex或用SQL写入业务状态。

原生证据分别为`/tmp/chat-quality-list-keyboard-before-final5-native-20261006`和
`/tmp/chat-quality-list-keyboard-after-final2-native-20261006`。四个before驱动失败依次
为误假设历史row索引、使用未认证SDK actor、过早查询尚未到达的搜索结果、误要求current
搜索结果必须selected；只修/tmp定位，不改产品或放宽目标/15s。完整final5三项产品RED
独立保留，不拼局部失败为一轮。首次after漏载测试数据库环境，createdb在启动产品前
失败；单独日志保留，新目录加载实际环境后完整运行，未增加production fallback。

### 门禁与开放项

既有fresh目录`/tmp/chat-read-context-fresh.JwLH7s`此次增量-Werror全build和23/23
CTest actual0，109.70s；不是另一次fresh configure。原tests/verify.sh actual0，
`LIST_ACTIVATION_VALIDATION_EXIT=0`，16份冻结source首尾一致，Qt/TUI均ON。

| 配置 | build / 全量CTest | 实际耗时 | tui_render（原5s） |
| --- | --- | ---: | ---: |
| normal Debug | PASS / 23/23 | 107.04s | 0.85s |
| ASan | PASS / 23/23 | 139.26s | 4.05s |
| UBSan | PASS / 23/23 | 133.24s | 1.76s |

日志`/tmp/chat-list-activation-{validation,strict-build,strict-ctest,final-verify}.log`，
无新增编译告警/sanitizer报告、skip或timeout放宽。实际flags单独核：strict为-g/-Werror，
正式三模式保留原-Wall/-Wextra/-Wpedantic及各自插桩，不冒称正式脚本也加了-Werror。

独立checker actual0：`/tmp/chat-list-keyboard-stage30-independent-review.json`，主也
逐项核验三原生目标、S005的SDKdirect、driver snapshot逐字节一致与source4 current
一致。独立cleanup`/tmp/chat-list-keyboard-stage30-cleanup-independent.json`：before库
`chat_qtx11_1006053112_390937`和after库`chat_qtx11_1006053820_401345`SQL count0，
20个记录descendants消失，18906无listener，长期服务2876288仍在；不外推未记录的
所有进程。真实/proc binary SHA在driver运行时断言，没有单独保存每个进程SHA清单。

字体候选与终端T04研究未混入源代码；Qt87/TUI78不提高，全页面/完整日常任务/无障碍
及最终连续两轮fresh review仍开放。无server/client/SQL/third修改，无migration或push。
