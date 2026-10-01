#include <QApplication>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <chat/client.hpp>
#include <libpq-fe.h>
#include <future>
#include <iostream>
#include <unistd.h>
#include "main_window.hpp"
#include "chat_widget.hpp"
#include "message_model.hpp"

void check(bool v, char const* text)
{
    if (!v)
    {
        throw std::runtime_error(text);
    }
}
template <class F> void wait(F f)
{
    for (int i = 0; i < 500 && !f(); ++i)
    {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    check(f(), "UI timeout");
}
template <class T, class F> T rpc(F f)
{
    std::promise<std::expected<T, chat::error>> p;
    auto future = p.get_future();
    f([&](auto r) { p.set_value(std::move(r)); });
    check(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "RPC timeout");
    auto r = future.get();
    check(r.has_value(), "RPC failed");
    return std::move(*r);
}
int main(int argc, char** argv)
{
    if (argc != 3)
    {
        return 1;
    }
    QApplication app(argc, argv);
    QProcess server;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PGHOST", "172.20.54.83");
    environment.insert("PGUSER", "chat");
    environment.insert("PGDATABASE", "chat");
    server.setProcessEnvironment(environment);
    std::vector<long> ids;
    long group = 0;
    int result = 1;
    auto db = PQconnectdb("hostaddr=172.20.54.83 dbname=chat user=chat sslmode=disable");
    auto start = [&]
    {
        server.start(QString::fromLocal8Bit(argv[1]), {"18769", "8", "4"});
        check(server.waitForStarted(), "Start server");
        QThread::msleep(100);
    };
    try
    {
        start();
        std::string url = "ws://127.0.0.1:18769/ws";
        std::vector<QString> names;
        {
            chat::client c;
            std::promise<void> connected, closed;
            c.set_connected_handler([&] { connected.set_value(); });
            c.set_disconnected_handler([&] { closed.set_value(); });
            c.connect(url);
            check(connected.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Connect");
            for (int i = 0; i < 3; ++i)
            {
                names.push_back(QString("qt_smoke_%1_%2").arg(getpid()).arg(i));
                ids.push_back(
                    rpc<long>([&](auto h) { c.register_user(names.back().toStdString(), "ui password", h); }));
            }
            rpc<chat::authentication_result>([&](auto h) { c.authenticate(names[0].toStdString(), "ui password", h); });
            for (int i = 1; i < 3; ++i)
            {
                rpc<chat::user>([&](auto h) { c.add_contact(ids[i], h); });
            }
            c.close();
            check(closed.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Close setup");
            QThread::msleep(100);
        }
        {
            std::vector<std::unique_ptr<main_window>> windows;
            std::vector<chat_widget*> pages;
            for (int i = 0; i < 3; ++i)
            {
                auto w = std::make_unique<main_window>(QString::fromStdString(url));
                w->show();
                for (auto* e : w->findChildren<QLineEdit*>())
                {
                    if (e->placeholderText() == QStringLiteral("用户名") && e->parent()->objectName() == "loginCard")
                    {
                        e->setText(names[i]);
                    }
                    if (e->placeholderText() == QStringLiteral("密码") && e->parent()->objectName() == "loginCard")
                    {
                        e->setText("ui password");
                    }
                }
                w->findChild<QPushButton*>("loginButton")->click();
                auto* page = w->findChild<chat_widget*>();
                wait([&] { return page->isVisible(); });
                pages.push_back(page);
                windows.push_back(std::move(w));
            }
            QToolButton* create = nullptr;
            for (auto* b : windows[0]->findChildren<QToolButton*>())
            {
                if (b->text() == QStringLiteral("建群"))
                {
                    create = b;
                }
            }
            check(create, "Create button");
            QTimer::singleShot(50,
                               [&]
                               {
                                   auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                                   check(dialog, "Create modal");
                                   dialog->findChild<QLineEdit*>()->setText(QStringLiteral("Qt 三人群"));
                                   auto* list = dialog->findChild<QListWidget*>();
                                   check(list->count() == 2, "Contact selection");
                                   for (int i = 0; i < list->count(); ++i)
                                   {
                                       list->item(i)->setCheckState(Qt::Checked);
                                   }
                                   dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                               });
            create->click();
            wait([&] { return pages[0]->active_conversation() > 0 && pages[0]->messages_ready(); });
            group = pages[0]->active_conversation();
            for (int i = 1; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("conversationList");
                wait([&] { return view->model()->rowCount() == 1; });
                view->clicked(view->model()->index(0, 0));
                wait([&] { return pages[i]->active_conversation() == group && pages[i]->messages_ready(); });
            }
            auto* edit = windows[0]->findChild<QLineEdit*>("messageEdit");
            edit->setText(QStringLiteral("Qt 群消息验证"));
            windows[0]->findChild<QToolButton*>("sendButton")->click();
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait([&] { return view->model()->rowCount() == 1; });
                check(view->model()->index(0, 0).data(message_model::outgoing_role).toBool() == (i == 0),
                      "Real author identity");
            }
            bool members = false;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout,
                             [&]
                             {
                                 auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                                 if (!box)
                                 {
                                     return;
                                 }
                                 members = box->text().contains(names[0]) && box->text().contains(names[1]) &&
                                           box->text().contains(names[2]);
                                 box->accept();
                                 poll.stop();
                             });
            poll.start(20);
            for (auto* b : windows[0]->findChildren<QPushButton*>())
            {
                if (b->text() == QStringLiteral("Qt 三人群"))
                {
                    b->click();
                    break;
                }
            }
            wait([&] { return members; });
            windows[0]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_smoke.png");
            server.terminate();
            check(server.waitForFinished(3000), "Stop server");
            wait([&] { return !windows[0]->findChild<QToolButton*>("sendButton")->isEnabled(); });
            start();
            for (int i = 0; i < 3; ++i)
            {
                wait(
                    [&, i]
                    {
                        return windows[i]->findChild<QToolButton*>("sendButton")->isEnabled() &&
                               pages[i]->messages_ready();
                    });
            }
            windows[1]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("重连后的群消息"));
            windows[1]->findChild<QToolButton*>("sendButton")->click();
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return windows[i]->findChild<QListView*>("messageList")->model()->rowCount() == 2; });
            }
            windows[1]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_reconnected.png");
        }
        std::cout << "PASS three real Qt windows: login, contacts group creation, member list, message author, "
                     "realtime, server restart and automatic recovery\n";
        result = 0;
    }
    catch (std::exception const& e)
    {
        std::cerr << "FAIL Qt smoke: " << e.what() << '\n';
        std::cerr << server.readAllStandardError().toStdString() << server.readAllStandardOutput().toStdString();
    }
    server.terminate();
    server.waitForFinished(3000);
    if (group)
    {
        auto q = "DELETE FROM conversations WHERE id=" + std::to_string(group);
        PQclear(PQexec(db, q.c_str()));
    }
    for (auto id : ids)
    {
        auto q = "DELETE FROM users WHERE id=" + std::to_string(id);
        PQclear(PQexec(db, q.c_str()));
    }
    PQfinish(db);
    return result;
}
