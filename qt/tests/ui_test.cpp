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
#include <QProcessEnvironment>
#include <QPushButton>
#include <QThread>
#include <QTemporaryDir>
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
#include "conversation_model.hpp"

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
            auto choose_reply = [&]
            {
                auto* view = windows[1]->findChild<QListView*>("messageList");
                QTimer::singleShot(20,
                                   []
                                   {
                                       auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                                       check(menu, "Reply menu");
                                       menu->setActiveAction(menu->actions().front());
                                       QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                                       QApplication::sendEvent(menu, &enter);
                                   });
                view->customContextMenuRequested(view->visualRect(view->model()->index(0, 0)).center());
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
            contact_view->clicked(contact_view->model()->index(0, 0));
            wait([&] { return pages[0]->active_conversation() != group && pages[0]->messages_ready(); });
            auto const direct = pages[0]->active_conversation();
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
