# 桌面消息通知取舍

2026-10-02 核查项目当前 Qt Widgets 依赖、Qt 6.2 官方源码和 Qt 官方文档。项目没有 QtDBus 依赖，不新增通知 daemon 或第三方通知框架。

`QSystemTrayIcon` 支持 Windows、macOS，以及提供 StatusNotifierItem 或 XEmbed 托盘的 Linux 桌面；可检查 `isSystemTrayAvailable()` 和 `supportsMessages()`。系统设置可能让 `showMessage()` 不展示，桌面提示不能替代持久化消息及会话未读。[Qt 官方文档](https://doc.qt.io/qt-6/qsystemtrayicon.html)

`messageClicked()` 不携带通知身份，Windows 点击托盘图标时也可能触发。macOS 底层收到具体通知对象后同样只向 Qt 发出无参数信号。多个会话同时通知时，用最近会话 ID 解释这个信号可能打开错误会话；本项目因此只恢复窗口，不承诺按任意旧通知精确跳转。[Qt 6.2 公共接口](https://github.com/qt/qtbase/blob/v6.2.0/src/widgets/util/qsystemtrayicon.cpp#L363-L370)、[Windows 实现](https://github.com/qt/qtbase/blob/v6.2.0/src/plugins/platforms/windows/qwindowssystemtrayicon.cpp#L251-L304)、[macOS 实现](https://github.com/qt/qtbase/blob/v6.2.0/src/plugins/platforms/cocoa/qcocoasystemtrayicon.mm#L243-L261)

X11 的 Qt fallback 每次展示会替换单个气泡，但这个行为不能推广到所有平台。[Qt 6.2 X11 fallback](https://github.com/qt/qtbase/blob/v6.2.0/src/widgets/util/qsystemtrayicon.cpp#L453-L477)

现有 CTest 用 offscreen，可以验证真实服务器消息触发的通知请求、摘要、阅读判定、重连和窗口恢复；它不能证明系统桌面实际展示了弹窗。Qt 自己的托盘测试也把 offscreen 的平台可用性与消息展示区分开。[Qt 6.2 托盘测试](https://github.com/qt/qtbase/blob/v6.2.0/tests/auto/widgets/util/qsystemtrayicon/tst_qsystemtrayicon.cpp#L109-L125)

项目仅从实时新消息发起通知，不从 history 或 reconnect 恢复发起。窗口在前台、目标会话已加载且视图位于底部才视为正在阅读；滚动到历史时保留位置。通知标题使用权威会话资料和发送者，首次遇到未知会话先刷新资料；退出、断线和 logout 清理尚待资料的通知。

额外验证使用独立 Xvfb、stalonetray 和 D-Bus session，完整真实 Qt UI 回归通过，并检查了实际 X11 通知气泡截图。为了自动操作文件控件，测试显式选择 Qt 文件对话框；应用的原生文件对话框行为保持。offscreen 的通知请求断言和这次原生展示验证分别证明业务决策与实际平台展示，不混称。
