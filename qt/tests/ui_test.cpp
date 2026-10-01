#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QKeyEvent>
#include <QLabel>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QThread>
#include <QTemporaryDir>
#include <QSortFilterProxyModel>
#include <source_location>
#include <QTimer>
#include <QToolButton>
#include <chat/client.hpp>
#include <libpq-fe.h>
#include <future>
#include <iostream>
#include <unistd.h>
#include "main_window.hpp"
#include "chat_widget.hpp"
#include "group_dialog.hpp"
#include "client_bridge.hpp"
#include "message_model.hpp"
#include "conversation_model.hpp"

void check(bool v, char const* text)
{
    if (!v)
    {
        throw std::runtime_error(text);
    }
}
template <class F> void wait(F f, int attempts = 500, std::source_location location = std::source_location::current())
{
    for (int i = 0; i < attempts && !f(); ++i)
    {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    if (!f()) { throw std::runtime_error("UI timeout at line " + std::to_string(location.line())); }
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
    std::vector<long> ids;
    long group = 0;
    int result = 1;
    auto db = PQconnectdb("");
    auto start = [&]
    {
        server.start(QString::fromLocal8Bit(argv[1]), {"18769", "8", "4"});
        check(server.waitForStarted(), "Start server");
        QThread::msleep(100);
    };
    try
    {
        start();
        {
            std::promise<void> failed_connect;
            client_bridge bridge;
            int stale_results = 0;
            QObject::connect(&bridge, &client_bridge::attachment_sent, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::attachment_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::message_search_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::error, &bridge, [&](auto) { failed_connect.set_value(); }, Qt::DirectConnection);
            bridge.send_attachment(1, "stale.bin", "old upload");
            bridge.get_attachment(1, 1);
            bridge.search_messages(1, "old search");
            QObject::connect(&bridge, &client_bridge::avatar_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::avatar_update_finished, &bridge, [&](auto...) { ++stale_results; });
            bridge.get_avatar(1, 1);
            bridge.set_avatar("stale avatar");
            bridge.clear_avatar();
            bridge.connect_to_server(QStringLiteral("http://127.0.0.1"));
            check(failed_connect.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Invalid URL callback");
            QCoreApplication::sendPostedEvents(&bridge, QEvent::MetaCall);
            check(stale_results == 0, "New connection discards old upload/download/search callbacks");
        }
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
            auto type_character = [&](int actor) {
                auto* input = windows[actor]->findChild<QLineEdit*>("messageEdit");
                QKeyEvent key(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
                QApplication::sendEvent(input, &key);
            };
            auto* peer_typing = windows[1]->findChild<QLabel*>("typingStatusLabel");
            auto* group_typing = windows[2]->findChild<QLabel*>("typingStatusLabel");
            int starts = 0;
            auto typing_capture = QObject::connect(pages[0], &chat_widget::typing_requested, windows[0].get(),
                [&](qint64 conversation, bool typing) { if (conversation == group && typing) { ++starts; } });
            for (int i = 0; i < 3; ++i)
            {
                type_character(0);
            }
            check(starts == 1, "Typing refresh is throttled");
            wait([&] { return peer_typing->isVisible() && peer_typing->text().contains(names[0]) &&
                                  group_typing->isVisible(); });
            windows[2]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_typing.png");
            QTimer::singleShot(2100, [&] { type_character(0); });
            wait([&] { return starts == 2; });
            for (int i = 0; i < 4; ++i)
            {
                QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
                QApplication::sendEvent(edit, &backspace);
            }
            wait([&] { return !peer_typing->isVisible() && !group_typing->isVisible(); });
            QObject::disconnect(typing_capture);
            pages[0]->typing_requested(group, true);
            pages[1]->typing_requested(group, true);
            wait([&] { return group_typing->text().contains(names[0]) && group_typing->text().contains(names[1]); });
            wait([&] { return !group_typing->isVisible(); }, 700);
            type_character(0);
            wait([&] { return peer_typing->isVisible(); });
            wait([&] { return !peer_typing->isVisible(); });
            check(edit->text() == "x", "Typing ends after idle without discarding the draft");
            type_character(0);
            wait([&] { return peer_typing->isVisible(); });
            edit->setText(QStringLiteral("Qt 群消息验证"));
            windows[0]->findChild<QToolButton*>("sendButton")->click();
            wait([&] { return !peer_typing->isVisible() && !group_typing->isVisible(); });
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait([&] { return view->model()->rowCount() == 1; });
                check(view->model()->index(0, 0).data(message_model::outgoing_role).toBool() == (i == 0),
                      "Real author identity");
            }
            bool members = false;
            int role_step = 0;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog)
                {
                    return;
                }
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                auto* button = dialog->findChild<QPushButton*>("groupAdminButton");
                if (list->count() != 3)
                {
                    return;
                }
                if (role_step == 0)
                {
                    check(list->item(0)->text().contains(names[0]) &&
                              list->item(0)->text().contains(QStringLiteral("群主")) &&
                              list->item(1)->text().contains(names[1]) &&
                              list->item(2)->text().contains(names[2]), "Members and creator role");
                    list->setCurrentRow(0);
                    check(!button->isEnabled(), "Owner cannot be an administrator");
                    list->setCurrentRow(1);
                    check(button->isEnabled(), "Owner can promote a member");
                    ++role_step;
                    button->click();
                }
                else if (role_step == 1 && button->isEnabled() &&
                         list->item(1)->text().contains(QStringLiteral("管理员")))
                {
                    dialog->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_roles.png");
                    ++role_step;
                    button->click();
                }
                else if (role_step == 2 && button->isEnabled() &&
                         !list->item(1)->text().contains(QStringLiteral("管理员")))
                {
                    members = true;
                    poll.stop();
                    dialog->accept();
                }
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
            QTemporaryDir avatar_files;
            QImage avatar_image(256, 256, QImage::Format_RGB32);
            quint32 noise = 17;
            for (int y = 0; y < avatar_image.height(); ++y)
            {
                for (int x = 0; x < avatar_image.width(); ++x)
                {
                    noise = noise * 1664525U + 1013904223U;
                    avatar_image.setPixel(x, y, qRgb(noise >> 24, (noise >> 16) & 255, (noise >> 8) & 255));
                }
            }
            auto avatar_path = avatar_files.filePath("avatar.png");
            check(avatar_files.isValid() && avatar_image.save(avatar_path), "Avatar PNG fixture");
            check(QFile(avatar_path).size() > static_cast<qint64>(chat::attachment_chunk_size), "Avatar uses multiple transport chunks");
            auto const avatar_color = avatar_image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation).pixelColor(0, 0);
            pages[1]->contact_add_requested(ids[0]);
            auto avatar_update = [&](QString const& path, bool remove, bool close_early = false) {
                bool finished = false;
                QTimer update_poll;
                int step = 0;
                QObject::connect(&update_poll, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!dialog || dialog->objectName() != "profileDialog") { return; }
                    auto* button = dialog->findChild<QPushButton*>(remove ? "removeAvatarButton" : "changeAvatarButton");
                    if (step == 0 && button->isEnabled())
                    {
                        step = 1;
                        if (!remove)
                        {
                            QTimer::singleShot(30, [&] {
                                auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                                check(picker, "Avatar file picker");
                                picker->setDirectory(avatar_files.path());
                                QTimer::singleShot(100, picker, [picker, path] {
                                    picker->findChild<QLineEdit*>("fileNameEdit")->setText(path);
                                    QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
                                });
                                QTimer::singleShot(1500, picker, &QDialog::reject);
                            });
                        }
                        button->click();
                        if (close_early)
                        {
                            finished = true;
                            update_poll.stop();
                            dialog->reject();
                        }
                    }
                    else if (step == 1 && dialog->findChild<QPushButton*>("changeAvatarButton")->isEnabled() &&
                             dialog->findChild<QLabel*>("avatarUploadStatus")->text().isEmpty())
                    {
                        finished = true;
                        update_poll.stop();
                        dialog->accept();
                    }
                });
                update_poll.start(20);
                windows[0]->findChild<QToolButton*>("profileAvatar")->click();
                check(finished, "Self profile avatar action");
            };
            avatar_update(avatar_path, false);
            for (int i = 0; i < 3; ++i)
            {
                wait([&] { return !pages[i]->avatars().image(ids[0]).isNull(); });
                check(pages[i]->avatars().state(ids[0]) == chat::avatar_state{1, true}, "Realtime group avatar metadata");
                check(pages[i]->avatars().image(ids[0]).toImage().pixelColor(0, 0) == avatar_color, "Real group avatar bytes");
            }
            QListView* avatar_contacts = nullptr;
            for (auto* view : windows[1]->findChildren<QListView*>("userList"))
            {
                if (qobject_cast<QSortFilterProxyModel*>(view->model())) { avatar_contacts = view; }
            }
            check(avatar_contacts, "Avatar contacts view");
            wait([&] { return avatar_contacts->model()->rowCount() == 1; });
            check(!avatar_contacts->model()->index(0, 0).data(Qt::DecorationRole).value<QPixmap>().isNull(), "Contact list realtime avatar");
            auto* group_messages = windows[1]->findChild<QListView*>("messageList");
            check(!group_messages->model()->index(0, 0).data(Qt::DecorationRole).value<QPixmap>().isNull(), "Group message avatar");
            windows[1]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_avatar_group.png");
            bool avatar_members = false;
            QTimer avatar_members_poll;
            QObject::connect(&avatar_members_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog) { return; }
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                if (list->count() != 3) { return; }
                check(list->item(0)->icon().pixmap(32, 32).toImage() == avatar_icon(names[0], 32, pages[1]->avatars().image(ids[0])).pixmap(32, 32).toImage(), "Group member avatar");
                avatar_members = true;
                avatar_members_poll.stop();
                dialog->accept();
            });
            avatar_members_poll.start(20);
            windows[1]->findChild<QPushButton*>("chatHeaderButton")->click();
            check(avatar_members, "Group member real avatar UI");

            bool ordinary_member = false;
            QTimer member_poll;
            QObject::connect(&member_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog)
                {
                    return;
                }
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                if (list->count() != 3)
                {
                    return;
                }
                list->setCurrentRow(2);
                check(!dialog->findChild<QPushButton*>("groupAdminButton")->isEnabled(),
                      "Ordinary member cannot manage administrators");
                check(!dialog->findChild<QPushButton*>("groupRenameButton")->isEnabled() &&
                          !dialog->findChild<QPushButton*>("groupInviteButton")->isEnabled() &&
                          dialog->findChild<QPushButton*>("groupLeaveButton")->isEnabled(), "Ordinary member permissions");
                ordinary_member = true;
                member_poll.stop();
                dialog->accept();
            });
            member_poll.start(20);
            for (auto* b : windows[1]->findChildren<QPushButton*>())
            {
                if (b->text() == QStringLiteral("Qt 三人群"))
                {
                    b->click();
                    break;
                }
            }
            wait([&] { return ordinary_member; });
            auto choose_reply = [&](int actor = 1, int row = 0)
            {
                auto* view = windows[actor]->findChild<QListView*>("messageList");
                QTimer::singleShot(20,
                                   []
                                   {
                                       auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                                       check(menu, "Reply menu");
                                       menu->setActiveAction(menu->actions().front());
                                       QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                                       QApplication::sendEvent(menu, &enter);
                                   });
                view->customContextMenuRequested(view->visualRect(view->model()->index(row, 0)).center());
            };
            choose_reply();
            check(windows[1]->findChild<QLabel*>("replyPreview")->isVisible(), "Reply preview");
            windows[1]->findChild<QToolButton*>("cancelReplyButton")->click();
            check(!windows[1]->findChild<QLabel*>("replyPreview")->isVisible(), "Cancel reply");
            choose_reply();
            windows[1]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("Qt 引用回复"));
            windows[1]->findChild<QToolButton*>("sendButton")->click();
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait([&] { return view->model()->rowCount() == 2; });
                check(view->model()->index(1, 0).data(message_model::reply_id_role).toLongLong() ==
                          view->model()->index(0, 0).data(message_model::id_role).toLongLong(),
                      "Qt reply target");
                check(view->model()
                          ->index(1, 0)
                          .data(message_model::reply_text_role)
                          .toString()
                          .contains(QStringLiteral("Qt 群消息验证")),
                      "Qt reply contents");
            }
            auto* own_view = windows[0]->findChild<QListView*>("messageList");
            QTimer edit_dialog;
            QObject::connect(&edit_dialog, &QTimer::timeout,
                             [&]
                             {
                                 auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                                 if (!dialog)
                                 {
                                     return;
                                 }
                                 dialog->findChild<QPlainTextEdit*>()->setPlainText(QStringLiteral("Qt 已编辑群消息"));
                                 dialog->accept();
                                 edit_dialog.stop();
                             });
            edit_dialog.start(20);
            QTimer::singleShot(20,
                               []
                               {
                                   auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                                   check(menu, "Edit menu");
                                   QAction* edit = nullptr;
                                   for (auto* action : menu->actions())
                                   {
                                       if (action->text() == QStringLiteral("编辑"))
                                       {
                                           edit = action;
                                       }
                                   }
                                   check(edit, "Author edit action");
                                   menu->setActiveAction(edit);
                                   QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                                   QApplication::sendEvent(menu, &enter);
                               });
            own_view->customContextMenuRequested(own_view->visualRect(own_view->model()->index(0, 0)).center());
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait(
                    [&]
                    {
                        return view->model()->index(0, 0).data(message_model::text_role).toString() ==
                               QStringLiteral("Qt 已编辑群消息");
                    });
                check(view->model()->index(0, 0).data(message_model::edited_at_role).toLongLong() > 0,
                      "Qt edited timestamp");
                check(view->model()
                          ->index(1, 0)
                          .data(message_model::reply_text_role)
                          .toString()
                          .contains(QStringLiteral("Qt 已编辑群消息")),
                      "Live edited quote");
            }
            auto* reply_view = windows[1]->findChild<QListView*>("messageList");
            QTimer delete_dialog;
            QObject::connect(&delete_dialog, &QTimer::timeout,
                             [&]
                             {
                                 auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                                 if (!dialog)
                                 {
                                     return;
                                 }
                                 dialog->button(QMessageBox::Yes)->click();
                                 delete_dialog.stop();
                             });
            delete_dialog.start(20);
            QTimer::singleShot(20,
                               []
                               {
                                   auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                                   check(menu, "Delete menu");
                                   QAction* remove = nullptr;
                                   for (auto* action : menu->actions())
                                   {
                                       if (action->text() == QStringLiteral("删除"))
                                       {
                                           remove = action;
                                       }
                                   }
                                   check(remove, "Author delete action");
                                   menu->setActiveAction(remove);
                                   QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                                   QApplication::sendEvent(menu, &enter);
                               });
            reply_view->customContextMenuRequested(reply_view->visualRect(reply_view->model()->index(1, 0)).center());
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait([&] { return view->model()->index(1, 0).data(message_model::deleted_role).toBool(); });
                check(view->model()->index(1, 0).data(message_model::text_role).toString() ==
                          QStringLiteral("消息已删除"),
                      "Live deletion marker");
            }
            windows[2]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("离线编辑验证"));
            windows[2]->findChild<QToolButton*>("sendButton")->click();
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return windows[i]->findChild<QListView*>("messageList")->model()->rowCount() == 3; });
            }
            windows[0]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_smoke.png");
            type_character(1);
            wait([&] { return group_typing->isVisible(); });
            server.terminate();
            check(server.waitForFinished(3000), "Stop server");
            wait([&] { return !windows[0]->findChild<QToolButton*>("sendButton")->isEnabled(); });
            auto offline_sql =
                "UPDATE messages SET body='offline edit',edited_at=greatest(clock_timestamp(),edited_at+interval '1 "
                "millisecond') WHERE conversation_id=" +
                std::to_string(group) + " AND sender_id=" + std::to_string(ids[0]);
            auto* offline_result = PQexec(db, offline_sql.c_str());
            check(PQresultStatus(offline_result) == PGRES_COMMAND_OK, "Offline edit fixture");
            PQclear(offline_result);
            auto offline_delete_sql =
                "UPDATE messages SET body='',deleted=true WHERE conversation_id=" + std::to_string(group) +
                " AND sender_id=" + std::to_string(ids[0]);
            auto* deleted_result = PQexec(db, offline_delete_sql.c_str());
            check(PQresultStatus(deleted_result) == PGRES_COMMAND_OK, "Offline deletion fixture");
            PQclear(deleted_result);
            auto retained_edit_sql = "UPDATE messages SET body='offline retained "
                                     "edit',edited_at=greatest(clock_timestamp(),coalesce(edited_at,'epoch'::"
                                     "timestamptz)+interval '1 millisecond') WHERE conversation_id=" +
                                     std::to_string(group) + " AND sender_id=" + std::to_string(ids[2]);
            auto* retained_result = PQexec(db, retained_edit_sql.c_str());
            check(PQresultStatus(retained_result) == PGRES_COMMAND_OK, "Retained offline edit fixture");
            PQclear(retained_result);
            start();
            for (int i = 0; i < 3; ++i)
            {
                wait(
                    [&, i]
                    {
                        return windows[i]->findChild<QToolButton*>("sendButton")->isEnabled() &&
                               pages[i]->messages_ready();
                    });
                check(!windows[i]->findChild<QLabel*>("typingStatusLabel")->isVisible(),
                      "Reconnect does not restore stale typing");
                check(pages[i]->avatars().state(ids[0]) == chat::avatar_state{1, true} &&
                    pages[i]->avatars().image(ids[0]).toImage().pixelColor(0, 0) == avatar_color, "Reconnect preserves matching avatar cache");
            }
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait([&] { return view->model()->index(0, 0).data(message_model::deleted_role).toBool(); });
                check(view->model()->index(0, 0).data(message_model::text_role).toString() ==
                              QStringLiteral("消息已删除") &&
                          view->model()->index(1, 0).data(message_model::deleted_role).toBool(),
                      "Recovered deletion markers");
                wait(
                    [&]
                    {
                        return view->model()->index(2, 0).data(message_model::text_role).toString() ==
                               QStringLiteral("offline retained edit");
                    });
            }
            windows[1]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("重连后的群消息"));
            windows[1]->findChild<QToolButton*>("sendButton")->click();
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return windows[i]->findChild<QListView*>("messageList")->model()->rowCount() == 4; });
            }
            windows[1]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_reconnected.png");
            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "messageSearchDialog", "Message search dialog");
                auto* input = dialog->findChild<QLineEdit*>("messageSearchEdit");
                auto* search = dialog->findChild<QPushButton*>("searchMessagesButton");
                auto* results = dialog->findChild<QListView*>("messageSearchResults");
                input->setText(QStringLiteral("OFFLINE retained"));
                search->click();
                wait([&] { return results->model()->rowCount() == 1; });
                check(results->model()->index(0, 0).data(message_model::text_role).toString() ==
                          QStringLiteral("offline retained edit"), "Search returns edited current content");
                input->setText(QStringLiteral("离线编辑验证"));
                search->click();
                wait([&] { return dialog->findChild<QLabel*>("messageSearchStatus")->text() ==
                                     QStringLiteral("没有匹配的消息。"); });
                check(results->model()->rowCount() == 0, "Search excludes previous message content");
                dialog->reject();
            });
            windows[0]->findChild<QToolButton*>("messageSearchButton")->click();
            for (auto* button : windows[0]->findChildren<QToolButton*>())
            {
                if (button->text() == QStringLiteral("联系人"))
                {
                    button->click();
                    break;
                }
            }
            QListView* contact_view = nullptr;
            for (auto* view : windows[0]->findChildren<QListView*>("userList"))
            {
                if (view->isVisible())
                {
                    contact_view = view;
                    break;
                }
            }
            check(contact_view, "Visible contact list");
            wait([&] { return contact_view->model()->rowCount() == 2; });
            type_character(0);
            wait([&] { return group_typing->isVisible(); });
            contact_view->clicked(contact_view->model()->index(0, 0));
            wait([&] { return pages[0]->active_conversation() != group && pages[0]->messages_ready(); });
            wait([&] { return !group_typing->isVisible(); });
            auto const direct = pages[0]->active_conversation();
            pages[0]->set_conversations({});
            check(pages[0]->active_conversation() == direct &&
                      windows[0]->findChild<QLineEdit*>("messageEdit")->isEnabled() &&
                      windows[0]->findChild<QListView*>("messageList")->model()->rowCount() == 0,
                  "Refreshing conversations preserves a newly opened empty direct chat");
            windows[0]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("保留单聊历史"));
            windows[0]->findChild<QToolButton*>("sendButton")->click();
            wait([&] { return windows[0]->findChild<QListView*>("messageList")->model()->rowCount() == 1; });
            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "profileDialog", "Contact profile");
                auto* remove = dialog->findChild<QPushButton*>("removeContactButton");
                check(remove && remove->isEnabled(), "Contact removal action");
                QTimer::singleShot(50, [] {
                    auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    check(confirmation && confirmation->text().contains(QStringLiteral("聊天记录")),
                          "Contact removal confirmation");
                    confirmation->button(QMessageBox::Yes)->click();
                });
                remove->click();
            });
            windows[0]->findChild<QPushButton*>("chatHeaderButton")->click();
            wait([&] { return contact_view->model()->rowCount() == 1; });
            check(pages[0]->active_conversation() == direct &&
                      windows[0]->findChild<QListView*>("messageList")->model()->rowCount() == 1,
                  "Contact removal retains active chat and history");
            auto contacts_sql = "SELECT count(*) FROM contacts WHERE owner_id=" + std::to_string(ids[0]) +
                                " AND contact_id=" + std::to_string(ids[1]);
            auto* contacts_result = PQexec(db, contacts_sql.c_str());
            check(PQresultStatus(contacts_result) == PGRES_TUPLES_OK &&
                      std::string_view(PQgetvalue(contacts_result, 0, 0)) == "0", "Contact removal persisted");
            PQclear(contacts_result);

            QTemporaryDir attachments(QString::fromLocal8Bit(argv[2]) + "/attachments_XXXXXX");
            check(attachments.isValid(), "Attachment fixture directory");
            auto const image_path = attachments.filePath("photo.png");
            QImage image(40, 24, QImage::Format_RGB32);
            image.fill(Qt::green);
            check(image.save(image_path), "PNG fixture");
            QFile original(image_path);
            check(original.open(QIODevice::ReadOnly), "Open image fixture");
            auto const image_bytes = original.readAll();
            QTimer::singleShot(50, [&] {
                auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                check(picker, "Attachment file picker");
                picker->setDirectory(attachments.path());
                QTimer::singleShot(100, picker, [picker, image_path] {
                    picker->findChild<QLineEdit*>("fileNameEdit")->setText(image_path);
                    check(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection), "Accept selected file");
                });
                QTimer::singleShot(1500, picker, &QDialog::reject);
            });
            windows[0]->findChild<QToolButton*>("sendAttachmentButton")->click();
            auto* attachment_view = windows[0]->findChild<QListView*>("messageList");
            wait([&] { return attachment_view->model()->rowCount() == 2; });
            check(attachment_view->model()->index(1, 0).data(message_model::attachment_name_role).toString() == "photo.png" &&
                      attachment_view->model()->index(1, 0).data(message_model::attachment_type_role).toString() == "image/png",
                  "Uploaded PNG metadata");
            auto* peer_conversations = windows[1]->findChild<QListView*>("conversationList");
            QModelIndex direct_index;
            wait([&] {
                for (int row = 0; row < peer_conversations->model()->rowCount(); ++row)
                {
                    auto const index = peer_conversations->model()->index(row, 0);
                    if (index.data(conversation_model::id_role).toLongLong() == direct)
                    {
                        direct_index = index;
                        return true;
                    }
                }
                return false;
            });
            peer_conversations->clicked(direct_index);
            auto* peer_attachment_view = windows[1]->findChild<QListView*>("messageList");
            wait([&] { return pages[1]->active_conversation() == direct && pages[1]->messages_ready() &&
                                  peer_attachment_view->model()->rowCount() == 2; });
            check(!peer_attachment_view->model()->index(0, 0).data(Qt::DecorationRole).value<QPixmap>().isNull(), "Direct message real avatar");
            check(!direct_index.data(Qt::DecorationRole).value<QPixmap>().isNull(), "Direct conversation real avatar");
            bool saw_avatar_profile = false;
            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "profileDialog", "Peer avatar profile");
                auto const picture = dialog->findChild<QLabel*>("profileDialogAvatar")->pixmap();
                check(picture.toImage() == avatar_icon(names[0], 104, pages[1]->avatars().image(ids[0])).pixmap(104, 104).toImage(), "Peer profile real image");
                saw_avatar_profile = true;
                dialog->accept();
            });
            windows[1]->findChild<QPushButton*>("chatHeaderButton")->click();
            check(saw_avatar_profile, "Peer profile avatar UI");
            windows[1]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_avatar_direct.png");
            avatar_image.fill(QColor("#df6f22"));
            check(avatar_image.save(avatar_path, "JPEG"), "Avatar JPEG replacement fixture");
            avatar_update(avatar_path, false);
            for (int i = 0; i < 3; ++i)
            {
                wait([&] { return pages[i]->avatars().state(ids[0]) == chat::avatar_state{2, true} &&
                    !pages[i]->avatars().image(ids[0]).isNull(); });
            }
            check(pages[1]->avatars().image(ids[0]).toImage().pixelColor(0, 0).red() > 200, "JPEG replacement refresh");
            auto* self_avatar = windows[0]->findChild<QToolButton*>("profileAvatar");
            check(self_avatar->icon().pixmap(44, 44).toImage() == avatar_icon(names[0], 44, pages[0]->avatars().image(ids[0])).pixmap(44, 44).toImage(), "Current account real avatar");
            avatar_update({}, true);
            for (int i = 0; i < 3; ++i)
            {
                wait([&] { return pages[i]->avatars().state(ids[0]) == chat::avatar_state{3, false}; });
                check(pages[i]->avatars().image(ids[0]).isNull(), "Clear realtime fallback");
            }
            check(direct_index.data(Qt::DecorationRole).value<QPixmap>().isNull() &&
                peer_attachment_view->model()->index(0, 0).data(Qt::DecorationRole).value<QPixmap>().isNull(), "Direct list/message fallback after clear");
            windows[1]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_avatar_cleared.png");
            check(peer_attachment_view->model()->index(1, 0).data(message_model::attachment_name_role).toString() == "photo.png",
                  "Attachment restored from history");
            auto attachment_action = [&](int actor, QString action_name) {
                QTimer::singleShot(20, [action_name] {
                    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    check(menu, "Attachment context menu");
                    QAction* selected = nullptr;
                    for (auto* action : menu->actions())
                    {
                        check(action->text() != QStringLiteral("编辑"), "Attachment cannot be edited");
                        if (action->text() == action_name)
                        {
                            selected = action;
                        }
                    }
                    check(selected, "Attachment action available");
                    menu->setActiveAction(selected);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(menu, &enter);
                });
                auto* view = windows[actor]->findChild<QListView*>("messageList");
                view->customContextMenuRequested(view->visualRect(view->model()->index(1, 0)).center());
            };
            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "attachmentDialog", "Image preview dialog");
                auto* preview = dialog->findChild<QLabel*>("attachmentImage");
                wait([&] { return !preview->pixmap().isNull(); });
                check(preview->pixmap().size() == QSize(640, 384), "PNG preview decoded and scaled");
                dialog->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_attachment_preview.png");
                dialog->reject();
            });
            attachment_action(0, QStringLiteral("查看图片"));
            auto const saved_path = attachments.filePath("saved.png");
            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "attachmentDialog", "File download dialog");
                auto* save = dialog->findChild<QPushButton*>("saveAttachmentButton");
                wait([&] { return save->isEnabled(); });
                QTimer::singleShot(50, [&] {
                    auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                    check(picker, "Save attachment picker");
                    picker->setDirectory(attachments.path());
                    QTimer::singleShot(100, picker, [picker, saved_path] {
                        picker->findChild<QLineEdit*>("fileNameEdit")->setText(saved_path);
                        check(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection), "Accept save path");
                    });
                    QTimer::singleShot(1500, picker, &QDialog::reject);
                });
                save->click();
                QFile saved(saved_path);
                check(saved.open(QIODevice::ReadOnly) && saved.readAll() == image_bytes, "Downloaded image bytes saved exactly");
                dialog->reject();
            });
            attachment_action(1, QStringLiteral("下载文件"));
            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "attachmentDialog", "Download disconnect dialog");
                server.terminate();
                check(server.waitForFinished(3000), "Stop server during attachment dialog");
                wait([&] { return dialog->findChild<QLabel*>("attachmentStatus")->text().contains(QStringLiteral("连接已断开")); });
                dialog->reject();
            });
            attachment_action(1, QStringLiteral("下载文件"));
            start();
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return windows[i]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled() &&
                                      pages[i]->messages_ready(); });
            }
            QTimer::singleShot(50, [] {
                auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                check(confirmation, "Delete attachment confirmation");
                confirmation->button(QMessageBox::Yes)->click();
            });
            attachment_action(0, QStringLiteral("删除"));
            for (int i = 0; i < 2; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait([&] { return view->model()->index(1, 0).data(message_model::deleted_role).toBool(); });
                check(view->model()->index(1, 0).data(message_model::attachment_name_role).toString().isEmpty(),
                      "Deleted attachment has no download metadata");
            }

            auto select_group = [&](int actor) {
                auto* list = windows[actor]->findChild<QListView*>("conversationList");
                QModelIndex selected;
                wait([&] {
                    for (int row = 0; row < list->model()->rowCount(); ++row)
                    {
                        auto index = list->model()->index(row, 0);
                        if (index.data(conversation_model::id_role).toLongLong() == group)
                        {
                            selected = index;
                            return true;
                        }
                    }
                    return false;
                });
                list->clicked(selected);
                wait([&] { return pages[actor]->active_conversation() == group && pages[actor]->messages_ready(); });
            };
            select_group(0);
            bool managed = false;
            int manage_step = 0;
            QTimer manage_poll;
            QObject::connect(&manage_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog)
                {
                    return;
                }
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                if (list->count() != 3)
                {
                    return;
                }
                auto* title = dialog->findChild<QLineEdit*>("groupTitleEdit");
                auto* promote = dialog->findChild<QPushButton*>("groupAdminButton");
                if (manage_step == 0)
                {
                    check(!dialog->findChild<QPushButton*>("groupLeaveButton")->isEnabled(), "Owner cannot leave in Qt");
                    title->setText(QStringLiteral("Qt 管理群"));
                    ++manage_step;
                    dialog->findChild<QPushButton*>("groupRenameButton")->click();
                }
                else if (manage_step == 1 && dialog->windowTitle().startsWith(QStringLiteral("Qt 管理群")))
                {
                    list->setCurrentRow(1);
                    if (promote->isEnabled())
                    {
                        ++manage_step;
                        promote->click();
                    }
                }
                else if (manage_step == 2 && promote->isEnabled() &&
                         list->item(1)->text().contains(QStringLiteral("管理员")))
                {
                    managed = true;
                    manage_poll.stop();
                    dialog->accept();
                }
            });
            manage_poll.start(20);
            windows[0]->findChild<QPushButton*>("chatHeaderButton")->click();
            wait([&] { return managed; });
            select_group(1);
            bool administrator = false;
            QTimer admin_poll;
            int admin_step = 0;
            QObject::connect(&admin_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog)
                {
                    return;
                }
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                if (list->count() != 3 || !dialog->findChild<QLineEdit*>("groupTitleEdit")->isEnabled())
                {
                    return;
                }
                if (admin_step == 0)
                {
                    check(list->item(1)->text().contains(QStringLiteral("管理员")), "Realtime role visible to administrator");
                    list->setCurrentRow(2);
                    check(!dialog->findChild<QPushButton*>("groupAdminButton")->isEnabled(), "Admin cannot appoint another admin");
                    check(dialog->findChild<QPushButton*>("groupRemoveButton")->isEnabled(), "Admin may remove ordinary member");
                    check(!dialog->findChild<QPushButton*>("groupTransferButton")->isEnabled(), "Admin cannot transfer ownership");
                    list->setCurrentRow(0);
                    check(!dialog->findChild<QPushButton*>("groupRemoveButton")->isEnabled(), "Admin cannot remove owner");
                    dialog->findChild<QLineEdit*>("groupTitleEdit")->setText(QStringLiteral("Qt 管理员改名群"));
                    ++admin_step;
                    dialog->findChild<QPushButton*>("groupRenameButton")->click();
                }
                else if (dialog->windowTitle().startsWith(QStringLiteral("Qt 管理员改名群")))
                {
                    dialog->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_management.png");
                    administrator = true;
                    admin_poll.stop();
                    dialog->accept();
                }
            });
            admin_poll.start(20);
            windows[1]->findChild<QPushButton*>("chatHeaderButton")->click();
            wait([&] { return administrator; });
            select_group(2);
            windows[2]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("退出后清除的草稿"));
            QTimer leave_poll;
            QObject::connect(&leave_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog || !dialog->findChild<QPushButton*>("groupLeaveButton")->isEnabled())
                {
                    return;
                }
                leave_poll.stop();
                QTimer::singleShot(20, [] {
                    auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    check(confirmation, "Group leave confirmation");
                    confirmation->button(QMessageBox::Yes)->click();
                });
                dialog->findChild<QPushButton*>("groupLeaveButton")->click();
            });
            leave_poll.start(20);
            windows[2]->findChild<QPushButton*>("chatHeaderButton")->click();
            wait([&] { return pages[2]->active_conversation() == 0; });
            check(windows[2]->findChild<QListView*>("messageList")->model()->rowCount() == 0 &&
                      windows[2]->findChild<QLineEdit*>("messageEdit")->text().isEmpty() &&
                      !windows[2]->findChild<QToolButton*>("sendButton")->isEnabled() &&
                      !windows[2]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled(),
                  "Leaving clears active history, draft and sending controls");
            wait([&] { return windows[0]->findChild<QLabel*>("chatPresence")->text().contains(QStringLiteral("2 名成员")); });
            wait([&] { return windows[2]->findChild<QListView*>("conversationList")->model()->rowCount() == 0; });
            auto const before_leave_message = pages[0]->latest_message_id();
            windows[0]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("退出期间的群消息"));
            windows[0]->findChild<QToolButton*>("sendButton")->click();
            wait([&] { return pages[0]->latest_message_id() > before_leave_message; });
            check(windows[2]->findChild<QListView*>("conversationList")->model()->rowCount() == 0,
                  "Former member receives no new conversation");
            bool reinvited = false;
            int invite_step = 0;
            QTimer invite_poll;
            QObject::connect(&invite_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog)
                {
                    return;
                }
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                auto* invite = dialog->findChild<QPushButton*>("groupInviteButton");
                if (invite_step == 0 && list->count() == 2 && invite->isEnabled())
                {
                    ++invite_step;
                    QTimer::singleShot(20, [&] {
                        auto* picker = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                        check(picker && picker->objectName() == "groupInviteDialog", "Invite contact dialog");
                        auto* contacts = picker->findChild<QListWidget*>();
                        check(contacts->count() == 1 && contacts->item(0)->text() == names[2], "Invite only own nonmember contacts");
                        contacts->item(0)->setCheckState(Qt::Checked);
                        picker->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                    });
                    invite->click();
                }
                else if (invite_step == 1 && list->count() == 3)
                {
                    reinvited = true;
                    invite_poll.stop();
                    dialog->accept();
                }
            });
            invite_poll.start(20);
            windows[0]->findChild<QPushButton*>("chatHeaderButton")->click();
            wait([&] { return reinvited && pages[2]->active_conversation() == group && pages[2]->messages_ready(); });
            check(windows[2]->findChild<QListView*>("messageList")->model()->rowCount() ==
                      windows[0]->findChild<QListView*>("messageList")->model()->rowCount(), "Reinvited Qt member recovers full history");
            auto member_action = [&](int actor, qint64 target, QString const& button_name) {
                bool finished = false;
                int step = 0;
                QTimer action_poll;
                QObject::connect(&action_poll, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                    if (!dialog) { return; }
                    auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                    int row = -1;
                    for (int i = 0; i < list->count(); ++i)
                    {
                        if (list->item(i)->data(Qt::UserRole).toLongLong() == target) { row = i; }
                    }
                    auto* button = dialog->findChild<QPushButton*>(button_name);
                    if (step == 0 && row >= 0)
                    {
                        list->setCurrentRow(row);
                        if (!button->isEnabled()) { return; }
                        ++step;
                        QTimer::singleShot(20, [] {
                            auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                            check(confirmation, "Member action confirmation");
                            confirmation->button(QMessageBox::Yes)->click();
                        });
                        button->click();
                    }
                    else if (step == 1 && (button_name == "groupRemoveButton" ? row == -1 :
                        row >= 0 && list->item(row)->text().contains(QStringLiteral("群主"))))
                    {
                        if (button_name == "groupTransferButton")
                        {
                            check(list->item(0)->text().contains(QStringLiteral("管理员")), "Former Qt owner becomes admin");
                            check(dialog->findChild<QPushButton*>("groupLeaveButton")->isEnabled(), "Former Qt owner may leave");
                            dialog->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_transfer.png");
                        }
                        finished = true;
                        action_poll.stop();
                        dialog->accept();
                    }
                });
                action_poll.start(20);
                windows[actor]->findChild<QPushButton*>("chatHeaderButton")->click();
                check(finished, "Member action completed in Qt");
            };
            auto invite_again = [&] {
                bool finished = false;
                int step = 0;
                QTimer invite_again_poll;
                QObject::connect(&invite_again_poll, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                    if (!dialog) { return; }
                    auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                    auto* button = dialog->findChild<QPushButton*>("groupInviteButton");
                    if (step == 0 && list->count() == 2 && button->isEnabled())
                    {
                        ++step;
                        QTimer::singleShot(20, [] {
                            auto* picker = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                            check(picker && picker->objectName() == "groupInviteDialog", "Reinvite removed Qt member");
                            picker->findChild<QListWidget*>()->item(0)->setCheckState(Qt::Checked);
                            picker->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                        });
                        button->click();
                    }
                    else if (step == 1 && list->count() == 3)
                    {
                        finished = true;
                        invite_again_poll.stop();
                        dialog->accept();
                    }
                });
                invite_again_poll.start(20);
                windows[0]->findChild<QPushButton*>("chatHeaderButton")->click();
                check(finished, "Reinvite action completed");
                wait([&] { return pages[2]->active_conversation() == group && pages[2]->messages_ready(); });
            };
            for (auto const& modal : {QStringLiteral("groupDialog"), QStringLiteral("messageSearchDialog"), QStringLiteral("attachmentDialog")})
            {
                select_group(2);
                if (modal == "groupDialog")
                {
                    choose_reply(2, windows[2]->findChild<QListView*>("messageList")->model()->rowCount() - 1);
                    check(windows[2]->findChild<QLabel*>("replyPreview")->isVisible(), "Reply pending before removal");
                    windows[2]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("移除后清理的草稿"));
                    pages[0]->typing_requested(group, true);
                    wait([&] { return group_typing->isVisible(); });
                }
                qint64 file_message = 0;
                if (modal == "attachmentDialog")
                {
                    auto const before_file = pages[0]->latest_message_id();
                    pages[0]->attachment_send_requested(group, "removal.bin", QByteArray("group file"), 0);
                    wait([&] { return pages[0]->latest_message_id() > before_file && pages[2]->latest_message_id() > before_file; });
                    file_message = pages[2]->latest_message_id();
                }
                QTimer::singleShot(50, [&] {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    check(dialog && dialog->objectName() == modal, "Removed member has a conversation modal open");
                    if (modal == "attachmentDialog")
                    {
                        auto* save = dialog->findChild<QPushButton*>("saveAttachmentButton");
                        wait([&] { return save->isEnabled(); });
                        QTimer::singleShot(50, [&] {
                            check(qobject_cast<QFileDialog*>(QApplication::activeModalWidget()), "Removed member has save picker open");
                            member_action(0, ids[2], "groupRemoveButton");
                        });
                        save->click();
                    }
                    else
                    {
                        member_action(0, ids[2], "groupRemoveButton");
                    }
                });
                if (modal == "groupDialog") { windows[2]->findChild<QPushButton*>("chatHeaderButton")->click(); }
                else if (modal == "messageSearchDialog") { windows[2]->findChild<QToolButton*>("messageSearchButton")->click(); }
                else { pages[2]->attachment_open_requested(group, file_message, "removal.bin", false); }
                wait([&] { return pages[2]->active_conversation() == 0; });
                check(!QApplication::activeModalWidget() && windows[2]->findChild<QListView*>("messageList")->model()->rowCount() == 0 &&
                    windows[2]->findChild<QLineEdit*>("messageEdit")->text().isEmpty() &&
                    !windows[2]->findChild<QLabel*>("replyPreview")->isVisible() && !group_typing->isVisible() &&
                    !windows[2]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled(),
                    "Removal closes nested modals and clears history, reply, draft, typing and attachment UI");
                invite_again();
            }
            member_action(0, ids[1], "groupTransferButton");
            server.terminate();
            check(server.waitForFinished(3000), "Restart after Qt ownership transfer");
            wait([&] { return !windows[0]->findChild<QToolButton*>("sendButton")->isEnabled(); });
            start();
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return pages[i]->messages_ready() && windows[i]->findChild<QToolButton*>("sendButton")->isEnabled(); });
            }
            bool recovered_owner = false;
            QTimer owner_poll;
            QObject::connect(&owner_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog) { return; }
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                if (list->count() != 3) { return; }
                check(list->item(0)->text().contains(QStringLiteral("管理员")) &&
                    list->item(1)->text().contains(QStringLiteral("群主")), "Ownership roles recover in Qt after reconnect");
                list->setCurrentRow(0);
                check(dialog->findChild<QPushButton*>("groupRemoveButton")->isEnabled(), "New Qt owner can remove administrator");
                list->setCurrentRow(2);
                check(!dialog->findChild<QPushButton*>("groupTransferButton")->isEnabled(), "New Qt owner cannot transfer to ordinary member");
                recovered_owner = true;
                owner_poll.stop();
                dialog->accept();
            });
            owner_poll.start(20);
            windows[1]->findChild<QPushButton*>("chatHeaderButton")->click();
            check(recovered_owner, "Reconnect member snapshot");
            for (auto* page : pages)
            {
                check(page->avatars().state(ids[0]) == chat::avatar_state{3, false} && page->avatars().image(ids[0]).isNull(),
                    "Cleared avatar remains fallback after reconnect");
            }
            avatar_update(avatar_path, false, true);
            wait([&] { return pages[0]->avatars().state(ids[0]) == chat::avatar_state{4, true} && !pages[0]->avatars().image(ids[0]).isNull(); });
            pages[0]->logout_requested();
            wait([&] { return !pages[0]->isVisible() && windows[0]->findChild<QPushButton*>("loginButton")->isEnabled(); });
            check(pages[0]->avatars().image(ids[0]).isNull(), "Logout clears current account cache");
            for (auto* input : windows[0]->findChildren<QLineEdit*>())
            {
                if (input->parent()->objectName() == "loginCard" && input->placeholderText() == QStringLiteral("密码"))
                { input->setText("ui password"); }
            }
            windows[0]->findChild<QPushButton*>("loginButton")->click();
            wait([&] { return pages[0]->isVisible() && pages[0]->avatars().state(ids[0]) == chat::avatar_state{4, true} &&
                !pages[0]->avatars().image(ids[0]).isNull(); });
            check(windows[0]->findChild<QToolButton*>("profileAvatar")->icon().pixmap(44, 44).toImage() ==
                avatar_icon(names[0], 44, pages[0]->avatars().image(ids[0])).pixmap(44, 44).toImage(), "Authentication restores own avatar after login");
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
