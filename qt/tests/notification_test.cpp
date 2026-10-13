#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <iostream>
#include <stdexcept>
#include "chat_widget.hpp"
#include "main_window.hpp"

namespace
{
auto const service_name = QStringLiteral("org.freedesktop.Notifications");
auto const service_path = QStringLiteral("/org/freedesktop/Notifications");
void check(bool condition, char const* text) { if (!condition) { throw std::runtime_error(text); } }
template<class F> void wait(F predicate, char const* text)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate())
    {
        check(timer.elapsed() < 5000, text);
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

class notification_service final : public QDBusVirtualObject
{
   public:
    explicit notification_service(QDBusConnection connection) : connection_(connection) {}
    QString introspect(QString const&) const override
    {
        return QStringLiteral("<interface name='org.freedesktop.Notifications'>"
            "<method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/>"
            "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='in'/>"
            "<arg type='as' direction='in'/><arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
            "<arg type='u' direction='out'/></method><method name='CloseNotification'>"
            "<arg type='u' direction='in'/></method><signal name='ActionInvoked'>"
            "<arg type='u'/><arg type='s'/></signal><signal name='NotificationClosed'>"
            "<arg type='u'/><arg type='u'/></signal></interface>");
    }
    bool handleMessage(QDBusMessage const& message, QDBusConnection const& connection) override
    {
        if (message.member() == QStringLiteral("Notify"))
        {
            notifications.push_back(message);
            if (defer) { pending.push_back(message); }
            else { connection.send(message.createReply(QVariantList{++last_id})); }
            return true;
        }
        if (message.member() == QStringLiteral("CloseNotification"))
        {
            closed.push_back(message.arguments().front().toUInt());
            connection.send(message.createReply());
            return true;
        }
        return false;
    }
    uint finish_pending()
    {
        auto const id = ++last_id;
        connection_.send(pending.takeFirst().createReply(QVariantList{id}));
        return id;
    }
    void action(uint id, QString action)
    {
        auto message = QDBusMessage::createSignal(service_path, service_name, QStringLiteral("ActionInvoked"));
        message << id << action;
        connection_.send(message);
    }
    void close(uint id)
    {
        auto message = QDBusMessage::createSignal(service_path, service_name, QStringLiteral("NotificationClosed"));
        message << id << uint(2);
        connection_.send(message);
    }
    bool defer = false;
    uint last_id = 40;
    QList<QDBusMessage> notifications, pending;
    QList<uint> closed;
   private:
    QDBusConnection connection_;
};
}

int main(int argc, char* argv[])
{
    QProcess bus;
    bus.start(QStringLiteral("dbus-daemon"), {QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1")});
    if (!bus.waitForStarted() || !bus.waitForReadyRead()) { std::cerr << "FAIL private D-Bus startup\n"; return 1; }
    qputenv("DBUS_SESSION_BUS_ADDRESS", bus.readLine().trimmed());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    int result = 0;
    try
    {
        auto connection = QDBusConnection::connectToBus(QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS")), QStringLiteral("notification-test-service"));
        notification_service service(connection);
        check(connection.registerService(service_name) && connection.registerVirtualObject(service_path, &service), "Private notification fixture registers");
        main_window window(QStringLiteral("ws://127.0.0.1:1/ws"));
        auto* chat = window.findChild<chat_widget*>();
        auto* pages = qobject_cast<QStackedWidget*>(chat->parentWidget());
        auto signed_in = [&] { chat->set_user(QStringLiteral("fixture"), 1); pages->setCurrentWidget(chat); window.show(); };
        signed_in();
        auto notice = [&](QString body) {
            auto const previous = service.notifications.size();
            window.notification_requested(7, QStringLiteral("Chat"), body);
            wait([&] { return service.notifications.size() == previous + 1; }, "Notify reaches native D-Bus boundary");
        };
        notice(QStringLiteral("正文 中文 🙂"));
        auto const args = service.notifications.back().arguments();
        check(args.size() == 8 && args[3].toString() == QStringLiteral("Chat") && args[4].toString() == QStringLiteral("正文 中文 🙂") &&
            args[5].toStringList() == QStringList{QStringLiteral("default"), QStringLiteral("显示 Chat")}, "Information notification preserves content and offers default restore action");
        wait([&] { return window.findChildren<QObject*>(QStringLiteral("desktopNotificationPending")).isEmpty(); }, "Notify reply registered before activation");
        check(qdbus_cast<QVariantMap>(args[6]).value(QStringLiteral("urgency")).toUInt() == 1, "Notification uses normal urgency rather than a forged critical alert");
        auto const own_id = service.last_id;
        auto dispatch_barrier = [&] {
            notice(QStringLiteral("notification dispatch barrier"));
            wait([&] { return window.findChildren<QObject*>(QStringLiteral("desktopNotificationPending")).isEmpty(); }, "Notification round trip drains prior actions");
        };
        window.hide(); service.action(own_id + 100, QStringLiteral("default")); service.action(own_id, QStringLiteral("other"));
        dispatch_barrier();
        check(!window.isVisible(), "Unowned IDs and unrelated actions do not restore the window");
        service.action(own_id, QStringLiteral("default"));
        wait([&] { return window.isVisible(); }, "Owned default action restores the window");
        service.close(own_id); dispatch_barrier();
        window.hide(); service.action(own_id, QStringLiteral("default")); dispatch_barrier();
        check(!window.isVisible(), "Closed notification no longer owns an activation");
        window.show();

        notice(QStringLiteral("active before global OFF"));
        wait([&] { return window.findChildren<QObject*>(QStringLiteral("desktopNotificationPending")).isEmpty(); }, "Active ID registered before disabling notifications");
        auto const active_id = service.last_id;
        service.defer = true;
        notice(QStringLiteral("pending before global OFF"));
        window.findChild<QAction*>("showSettingsAction")->trigger();
        window.findChild<QPushButton*>("settingsNotificationsButton")->click();
        window.findChild<QCheckBox*>("settingsDesktopNotifications")->click();
        wait([&] { return service.closed.contains(active_id); }, "Global OFF closes existing owned notifications");
        auto const cancelled_id = service.finish_pending();
        wait([&] { return service.closed.contains(cancelled_id); }, "Global OFF closes a late notification reply");
        window.findChild<QCheckBox*>("settingsDesktopNotifications")->click();
        window.findChild<QDialog*>("settingsDialog")->reject();
        notice(QStringLiteral("pending before same-account logout"));
        window.findChild<QAction*>("showSettingsAction")->trigger();
        QTimer::singleShot(0, &window, [] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { dialog->button(QMessageBox::Yes)->click(); }
        });
        window.findChild<QPushButton*>("settingsLogoutButton")->click();
        signed_in();
        auto const logout_id = service.finish_pending();
        wait([&] { return service.closed.contains(logout_id); }, "Logout cancels old pending replies even after the same account returns");
        service.defer = false;
        window.hide(); service.action(logout_id, QStringLiteral("default")); dispatch_barrier();
        check(!window.isVisible(), "Cancelled account notifications cannot reactivate a new session");
        service.defer = false;
        signed_in(); notice(QStringLiteral("new account notification"));
        wait([&] { return window.findChildren<QObject*>(QStringLiteral("desktopNotificationPending")).isEmpty(); }, "New valid notification remains functional");
        window.hide(); service.action(service.last_id, QStringLiteral("default"));
        wait([&] { return window.isVisible(); }, "New session default activation is accepted");
        check(connection.unregisterService(service_name), "Notification fixture service stops");
        window.notification_requested(7, QStringLiteral("Chat"), QStringLiteral("No daemon"));
        QApplication::processEvents();
        check(window.isVisible(), "Missing notification service keeps the application usable");
        std::cout << "PASS Qt native D-Bus actions, ownership, closed IDs, privacy payload and cancelled late replies\n";
    }
    catch (std::exception const& error) { std::cerr << "FAIL " << error.what() << '\n'; result = 1; }
    bus.terminate();
    if (!bus.waitForFinished(3000)) { bus.kill(); bus.waitForFinished(); }
    return result;
}
