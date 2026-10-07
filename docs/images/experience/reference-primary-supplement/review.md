# 官方参考补查：有限结论，不是完整运行采集

观察日：2026-10-07（Asia/Shanghai）。只读核对 Chat HEAD7375815 的 docs/experience-quality.md 参考审查段；未改仓库、未安装依赖、未启动 Chat/QQ/微信客户端、未运行 sanitizer。网页工具两次503/auth_not_found，改为 curl 官方原页/资源；网络有间歇DNS/传输超时，不能把失败管道末尾exit0当下载成功。下载/渲染仅在下述自有tmp。

本地：/tmp/chat-reference-primary.YOcY4oLY
远端：gyl@172.20.45.187:/tmp/chat-reference-primary.eEMedP9a

## 有效新增证据

1. 微信4.0+官方升级帮助中，加载、空间不足、权限失败有明确原图和恢复步骤。3张图已亲看；是官方裁切/标红的帮助插图，不是本机运行whole截图，也不能绑定4.1.15。
2. Awwwards Mobile Excellence PDF 本轮下载实际成功，全文只有一页，已用pdftotext完整读到EOF。此前“PDF未下载”可更新为本次取得；文档无发布日，并含旧式WebSQL/first-interactive beta术语，不能视作2026最新评分权重或桌面/TUI标准。
3. Awwwards当前SOTD twks官方条目和提交原图，以及FWA Ceramic Beats官方case API与2张提交原图已核。都是奖项平台上的作者提交材料；本轮没有实际操作作者站点，不新增交互PASS。
4. 负面证据：当前QQ Windows split_pc_win.png及win.mp4的2/7/10秒帧只是Windows装饰背景；微信下载页image1.svg是无文本示意。不能将这些算真实桌面UI截图。

## 官方来源、日期与精确边界

| 来源 | 发布/记录日期 | 本次实际看到什么 | 可以支持什么，不能支持什么 |
| --- | --- | --- | --- |
| [QQ Windows当前配置](https://qq-web.cdn-go.cn/im.qq.com_new/latest/rainbow/pcConfig.json)；[当前Windows页](https://im.qq.com/index/#/windows) | 配置9.9.36，2026-09-24 | JSON版本/更新日期；Windows页实际bundle映射win.mp4/split_pc_win.png，素材已下载与查看 | 只证当前发布事实和营销素材，不证登录布局、主窗、新朋友或9.9.36任何操作；im.qq.com/download服务端仍回旧9.7.1，不能当当前权威版本 |
| [QQ电脑端登录常见问题](https://kf.qq.com/faq/221212meEbqq2212127zeMNf.html) | 页面未给发布/修订日期 | 全文8种失败原因和处理方法，含版本过低、安全风险、网络不稳、多端重新输入密码、频繁操作 | 支持原因+可执行下一步的文字原则；没有当前登录截图，不证布局或实际恢复。不能复制其风控/扫码业务规则 |
| [微信Windows下载](https://pc.weixin.qq.com/)；[4.1.15更新](https://weixin.qq.com/updates?platform=windows&version=4.1.15) | 4.1.15发布日期2026-09-15 | 页面版本、实际发布日期；940×540 SVG为3个重叠示意窗和方块/线条 | 只证版本及非常粗的导航/列表/正文分区示意；无真实文字、身份或动作，不证当前任何业务过程 |
| [怎么登录微信for Windows](https://kf.qq.com/faq/161224EjYriq161224JnUvE3.html) | 未给发布日期/适用构建 | 官方帮助文字给既有身份登录或扫码后手机确认的两种方法 | 支持主要身份任务与确认步骤分层；不证4.1.15登录尺寸、焦点、错误或注册。网页和客户端并存限制也不能外推到Chat |
| [微信Windows4.0及以上升级问题](https://kf.qq.com/faq/250710MraIZj250710QniyAr.html) | 页面未给发布日期；图片路径202507不是确切发布日期 | 官方帮助完整读；3张实际图：[加载](https://file.service.qq.com/user-files/uploads/202507/142445b672c087d48959d0141bde22a8.png)、[空间不足](https://file.service.qq.com/user-files/uploads/202507/6a4e0afc4e175a907f54011733a11301.png)、[权限失败](https://file.service.qq.com/user-files/uploads/202507/dfe1485c361efd60f4d88ab9fe8a9753.png) | 直接可见搜索旁常驻状态条、spinner/感叹号+文字、可打开详细原因、明确再试与不处理。正文区分历史加载与新消息仍可收发、搜索索引未建完与搜索不到；不是空状态/断网截图，更不是4.1.15完整主窗 |
| [微信Windows消息类型](https://kf.qq.com/faq/161224mEvqaI161224jqUzqI.html)；[QQ文件发送/接收常见问题](https://kf.qq.com/faq/2407092uArUn240709uA32Ez.html) | 均未给发布日期/构建 | 实际官方能力/格式不支持/过期文字；未将QQ内嵌会员配额图当下载保存UI | 支持区分“存在附件”“可预览”“已保存”“不可处理/已过期”状态；不证Chooser/Download/Save布局，不复制14天、4G或会员规则 |
| [微信Windows群功能](https://kf.qq.com/faq/161224q6RZnq161224y6n6NB.html) | 未给日期/构建 | Windows明确文字：群通话按钮→选参与人→确认；群名入口→群主移除成员 | 支持群通话上下文内入口与选择后确认的层次；不是创建群/邀请操作，无创建/邀请/成员列表截图，不证当前构建 |
| [微信群创建及设置](https://kf.qq.com/faq/161223m22e2I1612236zQz2M.html) | 未给日期/平台 | “轻触”等手机指向的建群、选好友、确认、群名/邀请权限说明 | 只作非平台特定的任务阶段旁证；不能算Windows创建群/邀请直接证据，不能移植群权限 |
| [QQ电脑端撤回方法](https://kf.qq.com/faq/190511eeMFrY190511uqYZfU.html) | 未给日期/构建 | 明确电脑端右键已发消息→撤回；成功后聊天内有结果文字 | 支持与消息关联的上下文入口和可见结果；撤回不等于reply/edit，不能借它声称回复/编辑已覆盖 |
| [Awwwards Mobile Excellence PDF](https://www.awwwards.com/mobile-excellence-guidelines.pdf) | 文档未注明发布日期；HTTP last-modified不是内容发布日期 | 原PDF838372字节、全文一页，实际包含易见CTA、搜索、表单错误、可贴密码、字体/对比度/目标尺寸/viewport等检查 | 可转译成审查问题；移动网页性能指标、Service Worker和触控标准不机械套用到Qt/TUI，未推导官方Desktop权重 |
| [twks官方Awwwards条目](https://www.awwwards.com/sites/twks-1)；[官方提交图](https://assets.awwwards.com/awards/submissions/2026/08/6a8d4591d07cb861638495.jpg) | 官方条目SOTD2026-10-07 | 1600×1200提交原图已亲看：左品牌、少量文本导航、独立contact、绿色影像主视觉 | 仅craft观察：少量明晰入口、导航文字统一、品牌不靠堆按钮。不能据静态图推断菜单hover、键盘、loading或响应式完成；不把中央横排导航/大影像搬进聊天产品 |
| [Ceramic Beats官方FWA case API](https://thefwa.com/api/cases/ceramicbeats) | API createdDate2026-09-16；FOTD2026-09-22 | API实际URL/award日期；[home](https://thefwa.com/dyn/resources/Case_Model_Case/slide1/7/19187/1789536495/1364_span12/6aaa28a4c198bceramic-beats-home.png)与[tile](https://thefwa.com/dyn/resources/Case_Model_Case/slide3/7/19187/1789536495/1364_span12/6aaa28b7172e5ceramic-beats-tile.png)已亲看 | 图直接显示主动作、主网格、分类标签、持续底部状态，以及条目详情渐进展开。不是本轮实际播放/CLEAR/保存；旧docs中的真实CLEAR仅保留历史边界，不能让静态API图片扩大运行claim |

Abatable官方页本轮真实可读，仅标Nominee2026-10-05，不是SOTD；Product UI图片下载540618/1174893字节时超时，未复制、未查看，不列有效视觉证据。Awwwards/FWA后续重取HTML/API副本发生DNS超时，先前成功响应的日期与链接是工具读取证据，不能声称已保存它们的完整HTML/API档案。

## 状态覆盖最小映射

| 要求状态 | 本轮可补的直接材料 | 仍欠缺的当前QQ NT / 微信Windows直接证据 |
| --- | --- | --- |
| login / register | QQ登录原因帮助、微信Windows登录方法文字 | 两者当前登录原图/完整失败恢复/注册；QQ注册网址本次空壳页，不外推桌面注册 |
| navigation / sidebar / search | 微信4.0+帮助裁图真实可见搜索/左rail/局部列表；索引建立的文字说明 | 当前完整主窗、跨页导航、真实搜索结果打开；裁图不证整个Sidebar |
| newfriends / contacts / profile | 无当前直接图；QQ失败原因只帮助文字 | 两者当前申请、接受/拒绝、contacts/profile业务完整图/步骤 |
| group / create / invite | Windows群通话选参与人再确认、群名内移除成员的官方文字；移动建群流程明确降级旁证 | 群通话不等于建群；当前Windows新群picker→确认→真实创建、加入/邀请、权限失败/成员全行原图 |
| empty / loading / offline | 微信4.0+官方loading和可恢复error图有效 | empty/no-results、真实offline/reconnect（加载失败不是断网）；需当前原图/完整恢复 |
| attachments | 格式/过期官方文字 | 当前发送chooser→下载→保存→原字节验证、失败/取消流程和原图 |
| reply / edit | QQ消息右键撤回仅上下文原则 | 两者当前reply/edit原图和操作；不等同撤回，不推测不存在的编辑能力 |

## 适合现有Chat品牌的具体取舍（设计推论，不是抄视觉/增加功能）

1. 保持cream/dark-green、Qt已验的compact460×600；本轮没有QQ尺寸证据，不能以“精确QQ比例”重新调整窗口。主身份动作最高优先级，服务器设置保留次级齿轮且可读Name。
2. 导航和输入位置稳定，已有搜索入口清楚标示对象（好友/群搜索与消息搜索不能同名误导）；不因QQ渠道/AI宣传而新增入口。
3. 现有loading/offline/error状态用“图标+简短原因+下一步”表达，文字而非只用绿色/红色。对可继续的任务保留列表/草稿；全屏错误不能无谓抹去已显示内容。这是微信升级帮助到Chat的推论，不声称其断网行为相同。
4. 分开无会话、无搜索结果、无好友申请、加载未完成和失败，不把它们画成一个空白页；只提供本产品已有的下一步。
5. 群选择→确认阶段让完整成员身份/选项保留可审：计数不代替姓名，全行不截断；确认页名称/焦点、返回再进入与真实成员SQL仍以本产品native证据判定，参考不能豁免测试。
6. 附件名称/大小/处理状态为首层，已有下载保存动作清晰，错误原因就地说明；不复制QQ离线保存期限/额度，不用静态文件卡替代真实保存验证。
7. 回复/编辑维持与原消息关联的入口、引用/编辑状态和完成反馈；因为本轮无竞品当前reply/edit图，这一条是本产品任务一致性建议，非“QQ/微信同款已证”。
8. Awwwards已读指南适合变成CTA、表单错误、字体、对比、焦点可辨、窄窗任务完成的审查问题，不增加装饰动画，不等同官方评分体系。
9. twks仅借少数明确文本入口与品牌一致性；Ceramic Beats仅借主任务/次级动作/持续状态/详情四层。它们的大主视觉、手写字体、材质纹理、hover关键内容不适合照搬高密度聊天/TUI。

## 下一最小参考验证

优先获取有版本身份的当前QQ NT/微信Windows官方或自有原生流程证据：登录与失败、好友申请→接受、创建群picker→确认、消息search→打开、附件send→save、offline→恢复。已有current official版本不等于这些流程已经采集。若只有手机步骤或无构建日期的帮助文章，应继续标为旁证；不要以再堆宣传图关闭参考矩阵，也不要给全品质评分升分。本产品的缺口仍通过自己的真实四端日常、页面状态/DPI/终端/键盘与两次fresh审查补齐。
