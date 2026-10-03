#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
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
#include <QScrollBar>
#include <QSystemTrayIcon>
#include <source_location>
#include <QTimer>
#include <QToolButton>
#include <chat/client.hpp>
#include <libpq-fe.h>
#include <future>
#include <memory>
#include <algorithm>
#include <array>
#include <iostream>
#include <unistd.h>
#include "main_window.hpp"
#include "chat_widget.hpp"
#include "group_dialog.hpp"
#include "client_bridge.hpp"
#include "message_model.hpp"
#include "conversation_model.hpp"
#include "user_delegate.hpp"
#include "message_delegate.hpp"
#include "theme.hpp"

void check(bool v, char const* text)
{
    if (!v)
    {
        std::cerr << "FAIL Qt assertion: " << text << '\n';
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
    auto promise = std::make_shared<std::promise<std::expected<T, chat::error>>>();
    auto future = promise->get_future();
    f([promise](auto r) { promise->set_value(std::move(r)); });
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
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
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
        check(server.state() == QProcess::Running, "Test server remains running on its dedicated port");
    };
    try
    {
        start();
        {
            chat_widget page;
            page.setStyleSheet(chat_style_sheet());
            page.resize(1180, 760);
            page.show();
            for (auto* button : page.findChildren<QToolButton*>())
            {
                if (button->text() == QStringLiteral("联系人")) { button->click(); break; }
            }
            auto* entry = page.findChild<QPushButton*>("newFriendsButton");
            check(entry && entry->isVisible(), "New friends entry is visible above contacts");
            auto const image = entry->grab().toImage();
            auto const background = image.pixelColor(10, image.height() / 2);
            check(background.alpha() == 255 && background.lightness() > 200,
                "New friends entry has an opaque light background for readable dark text");
        }
        {
            main_window registration(QStringLiteral("ws://127.0.0.1:18769/ws"));
            registration.show();
            registration.findChild<QPushButton*>("registerButton")->click();
            auto* dialog = registration.findChild<QDialog*>("registrationDialog");
            QLineEdit* username = nullptr;
            for (auto* field : dialog->findChildren<QLineEdit*>())
            {
                if (field->placeholderText() == QStringLiteral("用户名")) { username = field; }
                else { field->setText(QStringLiteral("valid password")); }
            }
            check(username, "Registration identity field");
            for (auto const& name : QList<QString>{QStringLiteral(" \u00a0\u3000"), QStringLiteral("a@b"),
                QStringLiteral("a\u0001b"), QStringLiteral("a\u0085b"), QStringLiteral("a\u202eb"), QString(65, 'x'),
                QString(22, QChar(0x4e2d)), QStringLiteral(" Alice"), QStringLiteral("Alice "),
                QStringLiteral(" 张三 "), QStringLiteral("\u00a0Alice"), QStringLiteral("Alice\u00a0"),
                QStringLiteral("\u3000张三"), QStringLiteral("张三\u3000")})
            {
                username->setText(name);
                dialog->findChild<QPushButton*>("registrationSubmitButton")->click();
                check(dialog->findChild<QLabel*>("subtleText")->text().contains(QStringLiteral("1–64 UTF-8")) &&
                    dialog->findChild<QLabel*>("subtleText")->text().contains(QStringLiteral("首尾不能有空白")) &&
                    dialog->findChild<QPushButton*>("registrationSubmitButton")->isEnabled(),
                    "Qt rejects invalid identity before connecting or registering");
            }
            dialog->reject();
        }
        {
            group_dialog dialog(1, 1, QStringLiteral("公告草稿"), QStringLiteral("旧公告"), false, nullptr);
            dialog.set_members(1, {{1, "owner", chat::member_role::owner, {}}, {2, "admin", chat::member_role::admin, {}}}, {});
            auto* title = dialog.findChild<QLineEdit*>("groupTitleEdit");
            auto* rename = dialog.findChild<QPushButton*>("groupRenameButton");
            check(rename, "Group rename control");
            for (auto const& value : QList<QString>{QStringLiteral(" \u00a0\u3000"), QString(257, 'x')})
            {
                title->setText(value);
                check(!rename->isEnabled(), "Qt group title rejects Unicode blank and oversized values");
            }
            QString requested_title;
            QObject::connect(&dialog, &group_dialog::rename_requested, &dialog, [&](QString value) { requested_title = value; });
            title->setText(QStringLiteral("  群 名  "));
            rename->click();
            check(requested_title == QStringLiteral("  群 名  "), "Qt preserves meaningful title whitespace");
            dialog.finish_action(1, false, {});
            dialog.set_invite(1, QString(64, 'a'), {});
            auto* invite_edit = dialog.findChild<QLineEdit*>("groupInviteLinkEdit");
            check(invite_edit->text() == QStringLiteral("chat://join/") + QString(64, 'a') &&
                dialog.findChild<QPushButton*>("groupCopyInviteButton")->isEnabled(), "Manager can display current invite link");
            dialog.set_requests(1, {{3, "applicant", false, 0, {}}}, 3, false, {});
            auto* tabs = dialog.findChild<QTabWidget*>("groupTabs");
            check(tabs->tabText(1) == QStringLiteral("成员") &&
                tabs->tabText(2) == QStringLiteral("入群申请 (1+)"),
                "Pending count updates the requests tab without renaming members");
            auto* requests_list = dialog.findChild<QListWidget*>("groupJoinRequestsList");
            requests_list->setCurrentRow(0);
            check(requests_list->count() == 1 && dialog.findChild<QPushButton*>("groupAcceptRequestButton")->isEnabled(),
                "Managers can review a real applicant independently of members");
            check(dialog.findChild<QPushButton*>("groupMoreRequestsButton")->isEnabled(), "Cursor enables the next pending page");
            dialog.refresh_requests();
            check(!dialog.findChild<QPushButton*>("groupMoreRequestsButton")->isEnabled(),
                "Refreshing the first page immediately invalidates the old pagination cursor");
            auto* edit = dialog.findChild<QPlainTextEdit*>("groupAnnouncementEdit");
            edit->setPlainText(QStringLiteral("未保存草稿"));
            conversation_data snapshot;
            snapshot.id = 1;
            snapshot.group = true;
            snapshot.username = QStringLiteral("公告草稿");
            snapshot.announcement = QStringLiteral("其他管理员发布的公告");
            dialog.set_conversations({snapshot}, {});
            check(edit->toPlainText() == QStringLiteral("未保存草稿"), "Realtime snapshot preserves unsaved announcement draft");
            dialog.set_members(1, {{1, "member", chat::member_role::member, {}}, {2, "owner", chat::member_role::owner, {}}}, {});
            dialog.set_invite(1, QString(64, 'b'), {});
            dialog.set_requests(1, {{3, "stale applicant", false, 0, {}}}, 0, false, {});
            check(requests_list->count() == 0 && !dialog.findChild<QTabWidget*>("groupTabs")->isTabVisible(2) &&
                !dialog.findChild<QPushButton*>("groupAcceptRequestButton")->isEnabled(),
                "Role loss clears private applications and rejects stale request callbacks");
            check(invite_edit->text().isEmpty() && !dialog.findChild<QPushButton*>("groupCopyInviteButton")->isEnabled(),
                "Role loss clears invite secret and rejects a stale link callback");
            check(edit->isReadOnly() && edit->toPlainText() == snapshot.announcement &&
                !dialog.findChild<QPushButton*>("groupAnnouncementButton")->isEnabled(), "Loss of management permission restores authoritative announcement");
            dialog.set_members(1, {{1, "owner", chat::member_role::owner, {}}, {2, "admin", chat::member_role::admin, {}}}, {});
            QString requested_announcement = QStringLiteral("not yet cleared");
            QObject::connect(&dialog, &group_dialog::announcement_requested, &dialog,
                [&](QString value) { requested_announcement = value; });
            edit->setPlainText(QStringLiteral(" \u00a0\u3000"));
            dialog.findChild<QPushButton*>("groupAnnouncementButton")->click();
            check(requested_announcement.isEmpty(), "Qt whitespace-only announcement uses clear semantics");
            snapshot.announcement.clear();
            dialog.set_conversations({snapshot}, {});
            dialog.finish_action(1, false, {});
            check(edit->toPlainText().isEmpty() && !edit->isReadOnly() &&
                !dialog.findChild<QPushButton*>("groupClearAnnouncementButton")->isEnabled(), "Explicit clear also clears an unsaved draft");
        }
        {
            std::promise<void> failed_connect;
            client_bridge bridge;
            int stale_results = 0;
            QObject::connect(&bridge, &client_bridge::attachment_sent, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::attachment_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::message_search_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::reaction_changed, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::message_image_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::read_marked, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::mute_finished, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::pin_finished, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::message_sent, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::message_updated, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::group_pin_finished, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::group_action_finished, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::group_invite_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::conversation_opened, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::members_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::group_join_requests_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::group_join_pending, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::authentication_finished, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::registration_finished, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::contacts_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::presences_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::users_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::contact_added, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::contact_removed, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::error, &bridge, [&](auto) { failed_connect.set_value(); }, Qt::DirectConnection);
            bridge.send_attachment(1, "stale.bin", "old upload");
            bridge.get_attachment(1, 1);
            bridge.get_message_image(1, 1);
            bridge.mark_read(1, 1);
            bridge.set_conversation_muted(1, true);
            bridge.set_conversation_pinned(1, true);
            bridge.send_message(1, "stale @user", 0);
            bridge.edit_message(1, 1, "stale edit @user");
            bridge.delete_message(1, 1);
            bridge.set_group_pinned_message(1, 1);
            bridge.set_group_pinned_message(1, std::nullopt);
            bridge.set_group_announcement(1, QStringLiteral("旧连接的公告"));
            bridge.group_invite(1);
            bridge.group_invite(1, true);
            bridge.group_invite(1, false);
            bridge.join_group(QString(64, 'a'));
            bridge.get_members(1);
            bridge.set_group_join_approval(1, true);
            bridge.get_group_join_requests(1);
            bridge.respond_group_join_request(1, 2, true);
            bridge.search_messages(1, "old search");
            bridge.set_message_reaction(1, 1, QStringLiteral("👍"));
            QObject::connect(&bridge, &client_bridge::avatar_received, &bridge, [&](auto...) { ++stale_results; });
            QObject::connect(&bridge, &client_bridge::avatar_update_finished, &bridge, [&](auto...) { ++stale_results; });
            bridge.get_avatar(1, 1);
            bridge.set_avatar("stale avatar");
            bridge.clear_avatar();
            bridge.authenticate("stale user", "old password");
            bridge.register_user("stale user", "old password");
            bridge.open_direct_conversation(1, "stale peer");
            bridge.create_group("stale group", {1, 2});
            bridge.set_group_admin(1, 2, true);
            bridge.rename_group(1, "stale title");
            bridge.transfer_group_owner(1, 2);
            bridge.remove_group_member(1, 2);
            bridge.invite_group_members(1, {2});
            bridge.leave_group(1);
            bridge.get_contacts();
            bridge.get_presence();
            bridge.search_users("stale search");
            bridge.add_contact(2);
            bridge.remove_contact(2);
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
                rpc<chat::friendship_result>([&](auto h) { c.send_friend_request(ids[i], h); });
                chat::client peer;
                std::promise<void> peer_connected;
                peer.set_connected_handler([&] { peer_connected.set_value(); });
                peer.connect(url);
                check(peer_connected.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Connect setup peer");
                rpc<chat::authentication_result>([&](auto h) { peer.authenticate(names[i].toStdString(), "ui password", h); });
                rpc<chat::friendship_result>([&](auto h) { peer.respond_friend_request(ids[0], true, h); });
                peer.close();
            }
            c.close();
            check(closed.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Close setup");
            QThread::msleep(100);
        }
        {
            std::vector<std::unique_ptr<main_window>> windows;
            std::vector<chat_widget*> pages;
            struct notification { qint64 conversation; QString title; QString summary; };
            QList<notification> notifications[3];
            for (int i = 0; i < 3; ++i)
            {
                auto w = std::make_unique<main_window>(QString::fromStdString(url));
                w->show();
                QObject::connect(w.get(), &main_window::notification_requested, w.get(),
                    [&, i](qint64 conversation, QString title, QString summary) {
                        notifications[i].push_back({conversation, std::move(title), std::move(summary)});
                    });
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
            auto activate = [&](int actor) {
                windows[actor]->showNormal();
                windows[actor]->raise();
                windows[actor]->activateWindow();
                wait([&] { return windows[actor]->isActiveWindow(); });
                QApplication::processEvents();
            };
            auto set_preference = [&](int actor, qint64 conversation, bool value, bool pin = false, bool refresh = false) {
                for (auto* button : windows[actor]->findChildren<QToolButton*>())
                {
                    if (button->text() == QStringLiteral("聊天")) { button->click(); }
                }
                auto* view = windows[actor]->findChild<QListView*>("conversationList");
                wait([&] { return view->isVisible(); });
                QModelIndex index;
                wait([&] {
                    for (int row = 0; row < view->model()->rowCount(); ++row)
                    {
                        auto const item = view->model()->index(row, 0);
                        if (item.data(conversation_model::id_role).toLongLong() == conversation) { index = item; return true; }
                    }
                    return false;
                });
                check(index.data(pin ? conversation_model::pinned_role : conversation_model::muted_role).toBool() != value,
                    "Preference menu starts from authoritative state");
                QTimer::singleShot(20, [&, actor, conversation, value, pin, refresh] {
                    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    check(menu, "Conversation preference menu");
                    auto const label = pin ? (value ? QStringLiteral("置顶") : QStringLiteral("取消置顶")) :
                        (value ? QStringLiteral("静音") : QStringLiteral("取消静音"));
                    QAction* selected = nullptr;
                    for (auto* action : menu->actions()) { if (action->text() == label) { selected = action; } }
                    check(selected, "Conversation preference action");
                    if (refresh)
                    {
                        int resets = 0;
                        auto const capture = QObject::connect(view->model(), &QAbstractItemModel::modelReset, menu, [&] { ++resets; });
                        auto const text = pin ? QStringLiteral("置顶菜单打开时的列表刷新") : QStringLiteral("静音菜单打开时的列表刷新");
                        pages[0]->send_message_requested(conversation, text, 0);
                        wait([&] { return resets > 0 && pages[actor]->conversation(conversation)->last_text == text; });
                        QObject::disconnect(capture);
                    }
                    menu->setActiveAction(selected);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(menu, &enter);
                });
                view->customContextMenuRequested(view->visualRect(index).center());
                wait([&] { auto const item = pages[actor]->conversation(conversation); return item && (pin ? item->pinned : item->muted) == value; });
            };
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
                                   auto* list = dialog->findChild<QListWidget*>("groupContactPicker");
                                   check(list->count() == 2, "Contact selection");
                                   for (int i = 0; i < list->count(); ++i)
                                   {
                                       list->item(i)->setCheckState(Qt::Checked);
                                   }
                                   auto* chips = dialog->findChild<QListWidget*>("groupSelectedContacts");
                                   check(chips->count() == 2, "Selected friend chips");
                                   chips->itemClicked(chips->item(0));
                                   check(chips->count() == 1, "Selected friend can be removed");
                                   list->item(0)->setCheckState(Qt::Checked);
                                   auto* search = dialog->findChild<QLineEdit*>("groupContactSearch");
                                   search->setText(names[1]);
                                   check(!list->item(0)->isHidden() && list->item(1)->isHidden(), "Group picker searches friends");
                                   search->clear();
                                   dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                                   auto* title = dialog->findChild<QLineEdit*>("newGroupTitleEdit");
                                   check(title->isVisible(), "Group name follows friend selection");
                                   title->setText(QStringLiteral("Qt 三人群"));
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
            wait([&] { return !peer_typing->isVisible() && !group_typing->isVisible(); }, 700);
            type_character(0);
            wait([&] { return peer_typing->isVisible(); });
            wait([&] { return !peer_typing->isVisible(); });
            check(edit->text() == "x", "Typing ends after idle without discarding the draft");
            type_character(0);
            wait([&] { return peer_typing->isVisible(); });
            edit->setText(QStringLiteral("Qt 群消息验证"));
            activate(1);
            windows[0]->findChild<QToolButton*>("sendButton")->click();
            wait([&] { return !peer_typing->isVisible() && !group_typing->isVisible(); });
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait([&] { return view->model()->rowCount() == 1; });
                check(view->model()->index(0, 0).data(message_model::outgoing_role).toBool() == (i == 0),
                      "Real author identity");
            }
            auto* receipt_view = windows[0]->findChild<QListView*>("messageList");
            auto const reaction_message = receipt_view->model()->index(0, 0).data(message_model::id_role).toLongLong();
            wait([&] { return notifications[2].size() == 1; });
            check(notifications[0].isEmpty() && notifications[1].isEmpty() &&
                notifications[2].front().conversation == group && notifications[2].front().title.contains(QStringLiteral("Qt 三人群")) &&
                notifications[2].front().title.contains(names[0]) && notifications[2].front().summary == QStringLiteral("Qt 群消息验证"),
                "Only background recipient receives a real group notification; sender and active reader do not");
            auto unread_sql = "SELECT last_read_message_id FROM conversation_members WHERE conversation_id=" +
                std::to_string(group) + " AND user_id=" + std::to_string(ids[2]);
            auto* unread_result = PQexec(db, unread_sql.c_str());
            check(PQresultStatus(unread_result) == PGRES_TUPLES_OK && PQntuples(unread_result) == 1 &&
                std::string_view(PQgetvalue(unread_result, 0, 0)) == "0", "Background view does not advance real read position");
            PQclear(unread_result);
            activate(2);
            activate(0);
            auto choose_reaction = [&](int actor, QString emoji) {
                auto* view = windows[actor]->findChild<QListView*>("messageList");
                QTimer::singleShot(20, [emoji] {
                    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    check(menu, "Reaction context menu");
                    QMenu* picker = nullptr;
                    for (auto* action : menu->actions())
                    {
                        if (action->text() == QStringLiteral("表情回应")) { picker = action->menu(); }
                    }
                    check(picker && picker->actions().size() == 6, "Finite reaction picker");
                    for (auto* action : picker->actions())
                    {
                        if (action->text() == emoji) { action->trigger(); picker->close(); menu->close(); return; }
                    }
                    check(false, "Reaction picker emoji");
                });
                view->customContextMenuRequested(view->visualRect(view->model()->index(0, 0)).center());
            };
            choose_reaction(1, QStringLiteral("👍"));
            wait([&] {
                auto const reactions = receipt_view->model()->index(0, 0).data(message_model::reactions_role).value<QList<reaction_data>>();
                return reactions.size() == 1 && reactions.front().users == QList<qint64>{ids[1]};
            });
            choose_reaction(2, QStringLiteral("👍"));
            wait([&] {
                auto const reactions = receipt_view->model()->index(0, 0).data(message_model::reactions_role).value<QList<reaction_data>>();
                return reactions.size() == 1 && reactions.front().users.size() == 2;
            });
            windows[0]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_reactions.png");
            choose_reaction(1, QStringLiteral("❤️"));
            wait([&] {
                return receipt_view->model()->index(0, 0).data(message_model::reactions_role).value<QList<reaction_data>>().size() == 2 &&
                    windows[1]->findChild<QListView*>("messageList")->model()->index(0, 0)
                        .data(message_model::own_reaction_role).toString() == QStringLiteral("❤️");
            });
            choose_reaction(1, QStringLiteral("❤️"));
            wait([&] { return receipt_view->model()->index(0, 0).data(message_model::reactions_role).value<QList<reaction_data>>().size() == 1; });
            pages[0]->reaction_requested(group, reaction_message, QStringLiteral("🎉"));
            wait([&] { return receipt_view->model()->index(0, 0).data(message_model::own_reaction_role).toString() == QStringLiteral("🎉"); });
            wait([&] { return receipt_view->model()->index(0, 0).data(message_model::read_count_role).toInt() == 2; });
            auto open_read_details = [&](int actor, int row) {
                auto* view = windows[actor]->findChild<QListView*>("messageList");
                QTimer::singleShot(20, [] {
                    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    check(menu, "Read detail menu");
                    QAction* readers = nullptr;
                    for (auto* action : menu->actions())
                    {
                        if (action->text() == QStringLiteral("已读详情")) { readers = action; }
                    }
                    check(readers, "Group read detail action");
                    menu->setActiveAction(readers);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(menu, &enter);
                });
                view->customContextMenuRequested(view->visualRect(view->model()->index(row, 0)).center());
            };
            QTimer read_poll;
            bool read_details = false;
            QObject::connect(&read_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog || dialog->objectName() != "readDetailsDialog") { return; }
                auto* list = dialog->findChild<QListWidget*>("readMembersList");
                if (list->count() != 2) { return; }
                check(list->item(0)->data(Qt::UserRole).toLongLong() == ids[1] &&
                      list->item(1)->data(Qt::UserRole).toLongLong() == ids[2] &&
                      list->item(0)->text() == names[1] && list->item(1)->text() == names[2],
                      "Read detail lists actual current readers and excludes self");
                check(dialog->findChild<QLabel*>("readDetailsCount")->text() == QStringLiteral("已读 2 人"),
                      "Read detail count matches members");
                dialog->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_read_details.png");
                read_details = true;
                read_poll.stop();
                dialog->accept();
            });
            read_poll.start(20);
            open_read_details(0, 0);
            check(read_details, "Read detail dialog completed");
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
            auto announcement_action = [&](int actor, QString const& text, bool clear = false) {
                bool finished = false;
                int step = 0;
                QTimer announcement_poll;
                QObject::connect(&announcement_poll, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                    if (!dialog || dialog->parentWidget() != windows[actor].get() ||
                        dialog->findChild<QListWidget*>("groupMembersList")->count() != 3) { return; }
                    auto* edit = dialog->findChild<QPlainTextEdit*>("groupAnnouncementEdit");
                    auto* save = dialog->findChild<QPushButton*>("groupAnnouncementButton");
                    auto* clear_button = dialog->findChild<QPushButton*>("groupClearAnnouncementButton");
                    if (step == 0)
                    {
                        if (edit->isReadOnly()) { return; }
                        if (clear)
                        {
                            check(clear_button->isEnabled(), "Manager can clear current announcement");
                        }
                        else
                        {
                            edit->setPlainText(QString(1366, QChar(0x4E2D)));
                            check(!save->isEnabled() && !save->toolTip().isEmpty(), "Announcement UI limit counts UTF-8 bytes");
                            edit->setPlainText(text);
                            check(save->isEnabled(), "Manager can save changed announcement");
                        }
                        ++step;
                        (clear ? clear_button : save)->click();
                        check(edit->isReadOnly(), "Existing pending action state prevents edits during upload");
                    }
                    else if (std::all_of(pages.begin(), pages.end(), [&](auto* page) {
                        return page->conversation(group)->announcement == text;
                    }) && !edit->isReadOnly() && edit->toPlainText() == text &&
                        !save->isEnabled() && clear_button->isEnabled() == !text.isEmpty())
                    {
                        check(!save->isEnabled() && clear_button->isEnabled() == !text.isEmpty(), "Snapshot confirms saved/cleared announcement");
                        if (!clear) { dialog->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_announcement.png"); }
                        finished = true;
                        announcement_poll.stop();
                        dialog->accept();
                    }
                });
                announcement_poll.start(20);
                windows[actor]->findChild<QPushButton*>("chatHeaderButton")->click();
                check(finished, "Announcement action completed through real Qt controls");
            };
            std::array<int, 3> before_announcement_notifications{int(notifications[0].size()), int(notifications[1].size()), int(notifications[2].size())};
            std::array<int, 3> before_announcement_messages;
            for (int i = 0; i < 3; ++i) { before_announcement_messages[i] = windows[i]->findChild<QListView*>("messageList")->model()->rowCount(); }
            auto const announcement_text = QStringLiteral("当前群公告\n<纯文本，不是消息>");
            announcement_action(0, QStringLiteral("第一版公告"));
            bool live_announcement = false;
            int announcement_view_step = 0;
            QTimer announcement_view_poll;
            QObject::connect(&announcement_view_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog || dialog->parentWidget() != windows[1].get() ||
                    dialog->findChild<QListWidget*>("groupMembersList")->count() != 3) { return; }
                auto* edit = dialog->findChild<QPlainTextEdit*>("groupAnnouncementEdit");
                if (!edit->isReadOnly()) { return; }
                if (announcement_view_step == 0)
                {
                    check(edit->toPlainText() == QStringLiteral("第一版公告") &&
                        !dialog->findChild<QPushButton*>("groupAnnouncementButton")->isEnabled() &&
                        !dialog->findChild<QPushButton*>("groupClearAnnouncementButton")->isEnabled(), "Member views plaintext without editing permission");
                    ++announcement_view_step;
                    announcement_action(0, announcement_text);
                }
                else if (edit->toPlainText() == announcement_text)
                {
                    live_announcement = true;
                    announcement_view_poll.stop();
                    dialog->accept();
                }
            });
            announcement_view_poll.start(20);
            windows[1]->findChild<QPushButton*>("chatHeaderButton")->click();
            check(live_announcement, "An already-open member dialog receives realtime announcement refresh");
            for (int i = 0; i < 3; ++i)
            {
                check(int(notifications[i].size()) == before_announcement_notifications[i] &&
                    windows[i]->findChild<QListView*>("messageList")->model()->rowCount() == before_announcement_messages[i],
                    "Announcement is neither a message nor a desktop notification");
            }
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
            auto avatar_update = [&](QString const& path, bool remove, bool close_early = false) {
                bool finished = false;
                QTimer update_poll;
                int step = 0;
                QObject::connect(&update_poll, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!dialog || dialog->objectName() != "profileDialog") { return; }
                    for (auto* action : dialog->findChildren<QToolButton*>("profileActionButton"))
                    {
                        check(!action->isVisible() || (action->text() != QStringLiteral("消息") && action->text() != QStringLiteral("添加好友")),
                            "Own profile has no self messaging or self contact action");
                    }
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
                check(dialog->findChild<QTabWidget*>("groupTabs")->currentIndex() == 0 &&
                    dialog->findChild<QLabel*>("groupOverviewCount")->text().contains(QStringLiteral("3")) &&
                    dialog->findChild<QListWidget*>("groupMemberPreview")->count() == 3,
                    "Group opens hierarchical overview with count and member preview");
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
                check(dialog->findChild<QPlainTextEdit*>("groupAnnouncementEdit")->isReadOnly() &&
                    dialog->findChild<QPlainTextEdit*>("groupAnnouncementEdit")->toPlainText() == announcement_text,
                    "Member profile consistently shows current announcement");
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
            auto accept_friend = [&](int receiver, int sender) {
                auto* incoming = windows[receiver]->findChild<QListWidget*>("incomingFriendRequests");
                wait([&] {
                    for (int row = 0; row < incoming->count(); ++row)
                    { if (incoming->item(row)->data(Qt::UserRole).toLongLong() == ids[sender]) { return true; } }
                    return false;
                });
                check(windows[receiver]->findChild<QPushButton*>("newFriendsButton")->text().contains(QStringLiteral("1")),
                    "Incoming friend count is visible at contacts entry");
                QTimer::singleShot(20, [&] {
                    auto* profile = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    check(profile && profile->objectName() == "profileDialog", "Incoming friend profile");
                    QToolButton* accept = nullptr;
                    for (auto* button : profile->findChildren<QToolButton*>("profileActionButton"))
                    { if (button->text() == QStringLiteral("接受申请")) { accept = button; } }
                    check(accept && accept->isEnabled(), "Incoming friend can be accepted explicitly");
                    accept->click();
                    wait([&] { return accept->text() == QStringLiteral("消息") && accept->isEnabled(); });
                    profile->accept();
                });
                windows[receiver]->findChild<QPushButton*>("newFriendsButton")->click();
                for (int row = 0; row < incoming->count(); ++row)
                {
                    if (incoming->item(row)->data(Qt::UserRole).toLongLong() == ids[sender])
                    { incoming->itemClicked(incoming->item(row)); break; }
                }
            };
            pages[2]->contact_remove_requested(ids[0]);
            wait([&] {
                for (auto* view : windows[2]->findChildren<QListView*>("userList"))
                { if (qobject_cast<QSortFilterProxyModel*>(view->model())) { return view->model()->rowCount() == 0; } }
                return false;
            });
            auto inspect_noncontact_profile = [&](QString const& username, bool add) {
                auto* profile = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(profile && profile->objectName() == "profileDialog" &&
                    profile->findChild<QLabel*>("profileDialogName")->text() == username, "Public non-contact profile identity");
                QToolButton* action = nullptr;
                for (auto* button : profile->findChildren<QToolButton*>("profileActionButton"))
                {
                    check(!button->isVisible() || button->text() != QStringLiteral("消息"), "Non-contact cannot directly message from profile");
                    if (button->text() == QStringLiteral("添加好友")) { action = button; }
                }
                check(action && action->isVisible() && action->isEnabled(), "Non-contact profile offers add");
                if (add)
                {
                    action->click();
                    check(!action->isEnabled(), "Profile waits for authoritative request result");
                    wait([&] { return action->text() == QStringLiteral("等待验证"); });
                    check(!action->isEnabled(), "Outgoing pending does not grant messaging");
                    if (username == names[1])
                    {
                        auto* cancel = profile->findChild<QPushButton*>("cancelFriendRequestButton");
                        check(cancel->isVisible() && cancel->isEnabled(), "Outgoing request can be cancelled");
                        cancel->click();
                        wait([&] { return action->text() == QStringLiteral("添加好友") && action->isEnabled(); });
                        action->click();
                        wait([&] { return action->text() == QStringLiteral("等待验证"); });
                        auto* incoming = windows[1]->findChild<QListWidget*>("incomingFriendRequests");
                        wait([&] { return incoming->count() == 1; });
                        QTimer::singleShot(20, [&] {
                            auto* received = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                            auto* reject = received->findChild<QPushButton*>("rejectFriendRequestButton");
                            check(reject && reject->isVisible() && reject->isEnabled(), "Incoming request offers reject");
                            reject->click();
                            wait([&] { return !reject->isVisible(); });
                            received->reject();
                        });
                        incoming->itemClicked(incoming->item(0));
                        wait([&] { return action->text() == QStringLiteral("添加好友") && action->isEnabled(); });
                        action->click();
                        wait([&] { return action->text() == QStringLiteral("等待验证"); });
                    }
                    accept_friend(username == names[0] ? 0 : 1, 2);
                    wait([&] { return action->isEnabled() && action->text() == QStringLiteral("消息"); });
                }
                profile->reject();
            };
            QTimer group_profile_poll;
            bool group_profile_checked = false;
            QObject::connect(&group_profile_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog) { return; }
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                if (list->count() != 3) { return; }
                group_profile_poll.stop();
                QTimer::singleShot(30, windows[2].get(), [&] { inspect_noncontact_profile(names[0], true); });
                list->itemDoubleClicked(list->item(0));
                group_profile_checked = true;
                dialog->accept();
            });
            group_profile_poll.start(20);
            windows[2]->findChild<QPushButton*>("chatHeaderButton")->click();
            check(group_profile_checked, "Group member profile uses explicit friendship confirmation");
            pages[2]->contact_remove_requested(ids[0]);
            QListView* third_contacts = nullptr;
            for (auto* view : windows[2]->findChildren<QListView*>("userList"))
            {
                if (qobject_cast<QSortFilterProxyModel*>(view->model())) { third_contacts = view; }
            }
            check(third_contacts, "Third user's own contact view");
            wait([&] { return third_contacts->model()->rowCount() == 0; });
            auto* sender_view = windows[2]->findChild<QListView*>("messageList");
            auto* sender_delegate = qobject_cast<message_delegate*>(sender_view->itemDelegate());
            check(sender_delegate, "Group sender avatar profile delegate");
            QTimer::singleShot(30, windows[2].get(), [&] { inspect_noncontact_profile(names[0], false); });
            sender_delegate->avatar_clicked(sender_view->model()->index(0, 0));
            windows[2]->findChild<QToolButton*>("sidebarTextButton")->click();
            QLineEdit* search_input = nullptr;
            QListView* search_view = nullptr;
            for (auto* input : windows[2]->findChildren<QLineEdit*>("userSearchEdit"))
            { if (input->isVisible()) { search_input = input; } }
            for (auto* view : windows[2]->findChildren<QListView*>("userList"))
            { if (view->isVisible()) { search_view = view; } }
            check(search_input && search_view, "Non-contact user search UI");
            search_input->setText(names[1]);
            search_input->returnPressed();
            wait([&] { return search_view->model()->rowCount() == 1; });
            auto* search_delegate = qobject_cast<user_delegate*>(search_view->itemDelegate());
            check(search_delegate, "Search avatar profile delegate");
            QTimer::singleShot(30, windows[2].get(), [&] { inspect_noncontact_profile(names[1], true); });
            QStyleOptionViewItem search_option;
            search_option.rect = search_view->visualRect(search_view->model()->index(0, 0));
            QMouseEvent avatar_click(QEvent::MouseButtonRelease,
                QPointF(search_option.rect.left() + chat_theme::dialog_left + 4,
                        search_option.rect.top() + chat_theme::dialog_avatar_top + 4),
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            check(search_delegate->editorEvent(&avatar_click, search_view->model(), search_option,
                search_view->model()->index(0, 0)), "Search result avatar opens public profile");
            search_input->clear();
            search_input->returnPressed();
            check(search_view->model()->rowCount() == 0, "Clear previous user search results");
            search_input->setText(names[1]);
            search_input->returnPressed();
            wait([&] { return search_view->model()->rowCount() == 1; });
            QTimer::singleShot(20, [&] {
                auto* profile = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(profile && profile->objectName() == "profileDialog", "Search accepted friend profile");
                bool message = false;
                for (auto* button : profile->findChildren<QToolButton*>("profileActionButton"))
                { if (button->text() == QStringLiteral("消息") && button->isEnabled()) { message = true; } }
                check(message && profile->findChild<QPushButton*>("removeContactButton")->isVisible(),
                    "Accepted search result offers messaging and friend removal");
                profile->reject();
            });
            search_view->clicked(search_view->model()->index(0, 0));
            pages[2]->contact_remove_requested(ids[1]);
            wait([&] { return third_contacts->model()->rowCount() == 0; });
            pages[0]->contact_add_requested(ids[2]);
            accept_friend(2, 0);
            std::cout << "PASS Qt friendship request, pending, cancellation, rejection, explicit acceptance and shared-group isolation\n";
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
            choose_reply(2, 1);
            auto* pending_reply_preview = windows[2]->findChild<QLabel*>("replyPreview");
            auto* pending_reply_draft = windows[2]->findChild<QLineEdit*>("messageEdit");
            pending_reply_draft->setText(QStringLiteral("尚未发送的草稿"));
            check(pending_reply_preview->isVisible(), "Prepare a reply before the target is deleted");
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
            check(!pending_reply_preview->isVisible() && pending_reply_draft->text() == QStringLiteral("尚未发送的草稿"),
                "Deleting a prepared reply target clears the invalid reference and preserves the draft");
            windows[2]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("离线编辑验证"));
            windows[2]->findChild<QToolButton*>("sendButton")->click();
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return windows[i]->findChild<QListView*>("messageList")->model()->rowCount() == 3; });
            }
            auto const retained_message = receipt_view->model()->index(2, 0).data(message_model::id_role).toLongLong();
            pages[0]->reaction_requested(group, retained_message, QStringLiteral("😂"));
            wait([&] { return receipt_view->model()->index(2, 0).data(message_model::own_reaction_role).toString() == QStringLiteral("😂"); });
            windows[0]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_smoke.png");
            type_character(1);
            wait([&] { return group_typing->isVisible(); });
            set_preference(2, group, true);
            set_preference(2, group, true, true);
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
                "WITH deleted AS (UPDATE messages SET body='',deleted=true,reaction_revision=reaction_revision+1 "
                "WHERE conversation_id=" + std::to_string(group) + " AND sender_id=" + std::to_string(ids[0]) +
                " RETURNING id) DELETE FROM message_reactions WHERE message_id IN (SELECT id FROM deleted)";
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
            auto offline_reaction_sql = "BEGIN; UPDATE message_reactions SET emoji='❤️' WHERE message_id=" +
                std::to_string(retained_message) + " AND user_id=" + std::to_string(ids[0]) +
                "; UPDATE messages SET reaction_revision=reaction_revision+1 WHERE id=" + std::to_string(retained_message) + "; COMMIT";
            auto* reaction_result = PQexec(db, offline_reaction_sql.c_str());
            check(PQresultStatus(reaction_result) == PGRES_COMMAND_OK, "Offline reaction fixture");
            PQclear(reaction_result);
            auto const notices_before_reconnect = notifications[0].size() + notifications[1].size() + notifications[2].size();
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
            wait([&] { return pages[2]->conversation(group) && pages[2]->conversation(group)->muted; });
            check(!pages[0]->conversation(group)->muted && !pages[1]->conversation(group)->muted,
                "Reconnect restores private mute only for the selecting user");
            wait([&] { return pages[2]->conversation(group)->pinned; });
            check(!pages[0]->conversation(group)->pinned && !pages[1]->conversation(group)->pinned,
                "Reconnect restores private conversation pin");

            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait([&] { return view->model()->index(0, 0).data(message_model::deleted_role).toBool(); });
                check(view->model()->index(0, 0).data(message_model::text_role).toString() ==
                              QStringLiteral("消息已删除") &&
                          view->model()->index(1, 0).data(message_model::deleted_role).toBool(),
                      "Recovered deletion markers");
                check(view->model()->index(0, 0).data(message_model::reactions_role).value<QList<reaction_data>>().isEmpty(),
                      "Deleted message no longer displays reactions");
                wait(
                    [&]
                    {
                        return view->model()->index(2, 0).data(message_model::text_role).toString() ==
                               QStringLiteral("offline retained edit") &&
                               view->model()->index(2, 0).data(message_model::reactions_role).value<QList<reaction_data>>().size() == 1 &&
                               view->model()->index(2, 0).data(message_model::reactions_role).value<QList<reaction_data>>().front().emoji == QStringLiteral("❤️");
                    });
            }
            windows[1]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("重连后的群消息 @") + names[2]);
            check(notifications[0].size() + notifications[1].size() + notifications[2].size() == notices_before_reconnect,
                "History, edits, deletion and reaction recovery produce no ordinary notification");
            activate(0);
            auto const muted_group_notices = notifications[2].size();
            windows[1]->findChild<QToolButton*>("sendButton")->click();
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return windows[i]->findChild<QListView*>("messageList")->model()->rowCount() == 4; });
            }
            wait([&] { return pages[2]->conversation(group)->last_id == pages[2]->latest_message_id(); });
            check(notifications[2].size() == muted_group_notices && pages[2]->conversation(group)->unread > 0,
                "Muted group still receives realtime messages and unread without desktop notification");
            auto const mention_message = pages[2]->latest_message_id();
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                auto const mentions = view->model()->index(3, 0).data(message_model::mentions_role).value<QList<mention_data>>();
                check(mentions.size() == 1 && mentions.front().user == ids[2] &&
                    view->model()->index(3, 0).data(message_model::mentioned_role).toBool() == (i == 2),
                    "Persisted mention reaches sender and peers, with current-user visual only on target");
            }
            windows[2]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_mention.png");
            auto group_pin_menu = [&](int actor, int row, bool clear, bool permitted) {
                auto* view = windows[actor]->findChild<QListView*>("messageList");
                QTimer::singleShot(20, [clear, permitted] {
                    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    check(menu, "Group message context menu");
                    QAction* selected = nullptr;
                    for (auto* action : menu->actions())
                    {
                        if (action->text() == (clear ? QStringLiteral("取消置顶消息") : QStringLiteral("置顶消息"))) { selected = action; }
                    }
                    check((selected != nullptr) == permitted, "Only current owner/admin has a group pin menu action");
                    if (selected)
                    {
                        menu->setActiveAction(selected);
                        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                        QApplication::sendEvent(menu, &enter);
                    }
                    else { menu->close(); }
                });
                view->customContextMenuRequested(view->visualRect(view->model()->index(row, 0)).center());
            };
            group_pin_menu(2, 3, false, false);
            group_pin_menu(0, 3, false, true);
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return pages[i]->conversation(group)->pinned_message.id == mention_message &&
                    windows[i]->findChild<QPushButton*>("pinnedMessageButton")->isVisible(); });
            }
            check(!windows[2]->findChild<QToolButton*>("unpinMessageButton")->isVisible(), "Ordinary member only views pinned summary");
            auto const edited_pin_text = QStringLiteral("重连后的群消息 @") + names[2] + QStringLiteral(" · 更新置顶");
            pages[1]->edit_message_requested(group, mention_message, edited_pin_text);
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return pages[i]->conversation(group)->pinned_message.text == edited_pin_text &&
                    pages[i]->conversation(group)->pinned_message.edited_at > 0 &&
                    windows[i]->findChild<QPushButton*>("pinnedMessageButton")->text().contains(QStringLiteral("更新置顶")); });
            }
            windows[2]->findChild<QPushButton*>("pinnedMessageButton")->click();
            check(!QApplication::activeModalWidget(), "Loaded pinned message is located without a dialog");
            group_pin_menu(0, 3, true, true);
            for (int i = 0; i < 3; ++i) { wait([&, i] { return pages[i]->conversation(group)->pinned_message.id == 0; }); }
            group_pin_menu(0, 3, false, true);
            for (int i = 0; i < 3; ++i) { wait([&, i] { return pages[i]->conversation(group)->pinned_message.id == mention_message; }); }
            windows[2]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_pinned_message.png");

            set_preference(2, group, false);
            activate(2);
            activate(0);
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                wait([&] { return view->model()->index(3, 0).data(message_model::read_count_role).toInt() == (i == 1 ? 2 : 1); });
            }
            windows[1]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_reconnected.png");
            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "messageSearchDialog", "Message search dialog");
                auto* input = dialog->findChild<QLineEdit*>("messageSearchEdit");
                auto* search = dialog->findChild<QPushButton*>("searchMessagesButton");
                auto* results = dialog->findChild<QListView*>("messageSearchResults");
                input->setText(QStringLiteral("重连后的群消息"));
                search->click();
                wait([&] { return results->model()->rowCount() == 1; });
                auto const mentions = results->model()->index(0, 0).data(message_model::mentions_role).value<QList<mention_data>>();
                check(mentions.size() == 1 && mentions.front().user == ids[2], "Qt search uses persisted mention targets");
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
            wait([&] { return pages[0]->conversation(direct) && pages[0]->conversation(direct)->can_send; });
            check(pages[0]->active_conversation() == direct &&
                      windows[0]->findChild<QLineEdit*>("messageEdit")->isEnabled() &&
                      windows[0]->findChild<QListView*>("messageList")->model()->rowCount() == 0,
                  "An authoritative snapshot preserves a newly opened empty direct chat");
            windows[0]->findChild<QLineEdit*>("messageEdit")->setText(QStringLiteral("保留单聊历史"));
            windows[0]->findChild<QToolButton*>("sendButton")->click();
            wait([&] { return windows[0]->findChild<QListView*>("messageList")->model()->rowCount() == 1; });
            wait([&] {
                return std::ranges::any_of(notifications[1], [&](auto const& value) {
                    return value.conversation == direct && value.title == names[0] && value.summary == QStringLiteral("保留单聊历史");
                });
            });
            choose_reaction(0, QStringLiteral("😮"));
            wait([&] { return windows[0]->findChild<QListView*>("messageList")->model()->index(0, 0)
                                  .data(message_model::own_reaction_role).toString() == QStringLiteral("😮"); });
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
            wait([&] { return pages[0]->conversation(direct) && !pages[0]->conversation(direct)->can_send; });
            check(pages[0]->active_conversation() == direct &&
                      windows[0]->findChild<QListView*>("messageList")->model()->rowCount() == 1,
                  "Contact removal retains active chat and history");
            check(!windows[0]->findChild<QLineEdit*>("messageEdit")->isEnabled() &&
                !windows[0]->findChild<QToolButton*>("sendButton")->isEnabled() &&
                !windows[0]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled() &&
                windows[0]->findChild<QLineEdit*>("messageEdit")->placeholderText().contains(QStringLiteral("还不是好友")),
                "The removed contact direct is explicitly read-only");
            auto contacts_sql = "SELECT count(*) FROM contacts WHERE owner_id=" + std::to_string(ids[0]) +
                                " AND contact_id=" + std::to_string(ids[1]);
            auto* contacts_result = PQexec(db, contacts_sql.c_str());
            check(PQresultStatus(contacts_result) == PGRES_TUPLES_OK &&
                      std::string_view(PQgetvalue(contacts_result, 0, 0)) == "0", "Contact removal persisted");
            PQclear(contacts_result);

            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "profileDialog", "Removed contact profile");
                QToolButton* action = nullptr;
                for (auto* candidate : dialog->findChildren<QToolButton*>())
                {
                    check(candidate->text() != QStringLiteral("消息"), "A non-contact has no direct message action");
                    if (candidate->text() == QStringLiteral("添加好友")) { action = candidate; }
                }
                check(action && action->isEnabled(), "Non-contact profile offers friend request");
                action->click();
                check(!windows[0]->findChild<QLineEdit*>("messageEdit")->isEnabled(), "Adding is not optimistic authorization");
                wait([&] { return action->text() == QStringLiteral("等待验证"); });
                accept_friend(1, 0);
                wait([&] { return action->text() == QStringLiteral("消息") && action->isEnabled(); });
                action->click();
            });
            windows[0]->findChild<QPushButton*>("chatHeaderButton")->click();
            wait([&] { return pages[0]->conversation(direct) && pages[0]->conversation(direct)->can_send &&
                windows[0]->findChild<QLineEdit*>("messageEdit")->isEnabled(); });
            check(pages[0]->active_conversation() == direct && windows[0]->findChild<QListView*>("messageList")->model()->rowCount() == 1,
                "Re-add and authoritative direct open restore writing without losing history");

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
            wait([&] { return !attachment_view->model()->index(1, 0).data(message_model::image_role).value<QPixmap>().isNull(); });
            windows[0]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_image_bubble.png");
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
            pages[1]->contact_remove_requested(ids[0]);
            wait([&] { return avatar_contacts->model()->rowCount() == 0 && !pages[1]->conversation(direct)->can_send; });
            auto* peer_attachment_view = windows[1]->findChild<QListView*>("messageList");
            wait([&] { return pages[1]->active_conversation() == direct && pages[1]->messages_ready() &&
                                  peer_attachment_view->model()->rowCount() == 2; });
            check(!pages[1]->conversation(direct)->can_send &&
                !windows[1]->findChild<QLineEdit*>("messageEdit")->isEnabled() &&
                !windows[1]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled(),
                "Removing friendship makes the historical direct read-only");
            wait([&] { return !peer_attachment_view->model()->index(1, 0).data(message_model::image_role).value<QPixmap>().isNull(); });
            check(peer_attachment_view->model()->index(0, 0).data(message_model::reactions_role)
                      .value<QList<reaction_data>>().size() == 1, "Direct history restores reaction");
            check(!peer_attachment_view->model()->index(0, 0).data(Qt::DecorationRole).value<QPixmap>().isNull(), "Direct message real avatar");
            check(!direct_index.data(Qt::DecorationRole).value<QPixmap>().isNull(), "Direct conversation real avatar");
            bool saw_avatar_profile = false;
            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "profileDialog", "Peer avatar profile");
                auto const picture = dialog->findChild<QLabel*>("profileDialogAvatar")->pixmap();
                check(picture.toImage() == avatar_icon(names[0], 104, pages[1]->avatars().image(ids[0])).pixmap(104, 104).toImage(), "Peer profile real image");
                QToolButton* action = nullptr;
                for (auto* button : dialog->findChildren<QToolButton*>("profileActionButton"))
                {
                    check(button->text() != QStringLiteral("消息"), "Non-contact historical peer has no message action");
                    if (button->text() == QStringLiteral("添加好友")) { action = button; }
                }
                check(action, "Non-contact historical peer offers add");
                action->click();
                check(!windows[1]->findChild<QLineEdit*>("messageEdit")->isEnabled(), "Add does not optimistically grant send");
                wait([&] { return action->text() == QStringLiteral("等待验证"); });
                accept_friend(0, 1);
                wait([&] { return action->text() == QStringLiteral("消息") && action->isEnabled() &&
                    pages[1]->conversation(direct)->can_send; });
                saw_avatar_profile = true;
                action->click();
            });
            windows[1]->findChild<QPushButton*>("chatHeaderButton")->click();
            check(saw_avatar_profile, "Peer profile avatar UI");
            choose_reaction(1, QStringLiteral("😮"));
            wait([&] {
                auto const reactions = windows[0]->findChild<QListView*>("messageList")->model()->index(0, 0)
                    .data(message_model::reactions_role).value<QList<reaction_data>>();
                return reactions.size() == 1 && reactions.front().users.size() == 2;
            });
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
                auto const image_id = attachment_view->model()->index(1, 0).data(message_model::id_role).toLongLong();
                check(preview->pixmap().cacheKey() == pages[0]->images().image(image_id).cacheKey(),
                      "Full preview reuses decoded image cache");
                check(dialog->findChild<QPushButton*>("saveAttachmentButton")->isEnabled(),
                      "Cached preview can save original bytes");
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
            auto const cached_image_id = attachment_view->model()->index(1, 0).data(message_model::id_role).toLongLong();
            auto const cached_image_key = pages[1]->images().image(cached_image_id).cacheKey();
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
            {
                // Keep the Qt event loop suspended until the SDK mutation and snapshot lock
                // are complete. Qt cannot authenticate or apply a recovery snapshot yet.
                chat::client actor;
                std::promise<void> actor_connected;
                actor.set_connected_handler([&] { actor_connected.set_value(); });
                actor.connect(url);
                check(actor_connected.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                    "Connect friend-removal actor while Qt recovery is suspended");
                rpc<chat::authentication_result>([&](auto handler) {
                    actor.authenticate(names[0].toStdString(), "ui password", handler);
                });
                check(rpc<bool>([&](auto handler) { actor.remove_contact(ids[1], handler); }),
                    "Real remove_contact RPC deletes friendship before Qt recovery");
                actor.close();
            }
            std::unique_ptr<PGresult, decltype(&PQclear)> held_snapshot(PQexec(db,
                "BEGIN; LOCK TABLE conversations IN ACCESS EXCLUSIVE MODE"), &PQclear);
            check(held_snapshot && PQresultStatus(held_snapshot.get()) == PGRES_COMMAND_OK, "Hold authoritative reconnect snapshot");
            wait([&] {
                std::unique_ptr<PGresult, decltype(&PQclear)> blocked(PQexec(db,
                    "SELECT pg_stat_clear_snapshot(); SELECT EXISTS(SELECT 1 FROM pg_stat_activity "
                    "WHERE datname=current_database() AND pg_backend_pid()=ANY(pg_blocking_pids(pid)))"), &PQclear);
                return blocked && PQresultStatus(blocked.get()) == PGRES_TUPLES_OK &&
                    std::string_view(PQgetvalue(blocked.get(), 0, 0)) == "t";
            });
            check(!windows[0]->findChild<QLineEdit*>("messageEdit")->isEnabled() &&
                !windows[0]->findChild<QToolButton*>("sendButton")->isEnabled(),
                "Reconnect cannot re-enable cached send permission before the authoritative snapshot");
            std::unique_ptr<PGresult, decltype(&PQclear)> released_snapshot(PQexec(db, "COMMIT"), &PQclear);
            check(released_snapshot && PQresultStatus(released_snapshot.get()) == PGRES_COMMAND_OK, "Release reconnect snapshot");
            wait([&] { return pages[0]->messages_ready() && !pages[0]->conversation(direct)->can_send; });
            check(!windows[0]->findChild<QLineEdit*>("messageEdit")->isEnabled() &&
                !windows[0]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled(), "Reconnect restores read-only direct");
            wait([&] { return !windows[1]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled() &&
                pages[1]->messages_ready() && windows[2]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled() &&
                pages[2]->messages_ready(); });
            check(pages[1]->images().image(cached_image_id).cacheKey() == cached_image_key,
                  "Reconnect preserves immutable downloaded image");
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
                check(pages[i]->images().bytes(cached_image_id).isEmpty(), "Deleted image leaves cache");
            }
            pages[0]->contact_add_requested(ids[1]);
            accept_friend(1, 0);
            wait([&] { return pages[0]->conversation(direct)->can_send &&
                windows[0]->findChild<QToolButton*>("sendButton")->isEnabled(); });

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
            activate(1);
            wait([&] {
                auto* view = windows[0]->findChild<QListView*>("messageList");
                return view->model()->index(view->model()->rowCount() - 1, 0)
                    .data(message_model::read_count_role).toInt() == 1;
            });
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
                        dialog->findChild<QPushButton*>("groupAllMembersButton")->click();
                        check(!button->isVisible(), "Member management uses context actions");
                        auto const label = button->text();
                        QTimer::singleShot(20, [label] {
                            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                            check(menu, "Member context menu");
                            QAction* action = nullptr;
                            for (auto* candidate : menu->actions()) { if (candidate->text() == label) { action = candidate; } }
                            check(action, "Authorized member context action");
                            QTimer::singleShot(20, [] {
                                auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                                check(confirmation, "Member action confirmation");
                                confirmation->button(QMessageBox::Yes)->click();
                            });
                            menu->setActiveAction(action);
                            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                            QApplication::sendEvent(menu, &enter);
                        });
                        list->customContextMenuRequested(list->visualItemRect(list->item(row)).center());
                    }
                    else if (step == 1 && (button_name == "groupRemoveButton" ? row == -1 :
                        row >= 0 && list->item(row)->text().contains(QStringLiteral("群主"))))
                    {
                        if (button_name == "groupTransferButton")
                        {
                            check(list->item(1)->data(Qt::UserRole).toLongLong() == ids[0] && list->item(1)->text().contains(QStringLiteral("管理员")), "Former Qt owner becomes admin");
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
            for (auto const& modal : {QStringLiteral("groupDialog"), QStringLiteral("messageSearchDialog"),
                                     QStringLiteral("attachmentDialog"), QStringLiteral("readDetailsDialog")})
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
                else if (modal == "readDetailsDialog")
                {
                    open_read_details(2, windows[2]->findChild<QListView*>("messageList")->model()->rowCount() - 1);
                }
                else { pages[2]->attachment_open_requested(group, file_message, "removal.bin", false); }
                wait([&] { return pages[2]->active_conversation() == 0; });
                check(!QApplication::activeModalWidget() && windows[2]->findChild<QListView*>("messageList")->model()->rowCount() == 0 &&
                    windows[2]->findChild<QLineEdit*>("messageEdit")->text().isEmpty() &&
                    !windows[2]->findChild<QLabel*>("replyPreview")->isVisible() && !group_typing->isVisible() &&
                    !windows[2]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled(),
                    "Removal closes nested modals and clears history, reply, draft, typing and attachment UI");
                invite_again();
            }
            auto link_action = [&](int actor, std::optional<bool> create) {
                QString link;
                bool finished = false;
                int step = 0;
                QTimer link_poll;
                QObject::connect(&link_poll, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                    if (!dialog || dialog->findChild<QListWidget*>("groupMembersList")->count() < 2) { return; }
                    auto* edit = dialog->findChild<QLineEdit*>("groupInviteLinkEdit");
                    auto* create_button = dialog->findChild<QPushButton*>("groupCreateInviteButton");
                    auto* revoke_button = dialog->findChild<QPushButton*>("groupRevokeInviteButton");
                    if (step == 0 && create.has_value())
                    {
                        auto* action = *create ? create_button : revoke_button;
                        if (!action->isEnabled()) { return; }
                        ++step;
                        action->click();
                        return;
                    }
                    if (create == std::optional<bool>(false) ? edit->text().isEmpty() && create_button->isEnabled() :
                        !edit->text().isEmpty() && revoke_button->isEnabled())
                    {
                        link = edit->text();
                        if (!link.isEmpty())
                        {
                            dialog->findChild<QPushButton*>("groupCopyInviteButton")->click();
                            check(QApplication::clipboard()->text() == link, "Copy invitation uses actual persisted token");
                        }
                        dialog->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_invite.png");
                        finished = true;
                        link_poll.stop();
                        dialog->accept();
                    }
                });
                link_poll.start(20);
                windows[actor]->findChild<QPushButton*>("chatHeaderButton")->click();
                check(finished, "Invite link action completes in real Qt dialog");
                return link;
            };
            auto const original_link = link_action(0, true);
            check(original_link.startsWith(QStringLiteral("chat://join/")) && original_link.size() == 76 &&
                link_action(1, std::nullopt) == original_link, "Administrator sees same stable owner-created link");
            auto join_link = [&](QString const& link) {
                bool pasted = false;
                QTimer::singleShot(20, [&] {
                    auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                    check(input && input->windowTitle() == QStringLiteral("加入群聊"), "Join entry opens invitation input");
                    input->setTextValue(link);
                    pasted = true;
                    input->accept();
                });
                windows[2]->findChild<QToolButton*>("joinGroupButton")->click();
                check(pasted, "Paste invitation through real Qt input");
            };
            member_action(0, ids[2], "groupRemoveButton");
            wait([&] { return pages[2]->active_conversation() == 0 && !pages[2]->conversation(group); });
            join_link(original_link);
            wait([&] { return pages[2]->active_conversation() == group && pages[2]->messages_ready() &&
                pages[2]->conversation(group) && pages[2]->conversation(group)->member_count == 3; });
            check(pages[2]->conversation(group)->username == pages[0]->conversation(group)->username,
                "Joined conversation opens with authoritative title and old history");
            join_link(original_link);
            wait([&] { return pages[2]->messages_ready() && pages[2]->conversation(group)->member_count == 3; });
            link_action(1, false);
            member_action(0, ids[2], "groupRemoveButton");
            wait([&] { return pages[2]->active_conversation() == 0 && !pages[2]->conversation(group); });
            join_link(original_link);
            wait([&] { return std::ranges::any_of(windows[2]->findChildren<QLabel*>(), [](auto* label) {
                return label->text().contains(QStringLiteral("Invite unavailable"));
            }); });
            check(pages[2]->active_conversation() == 0, "Revoked link cannot reopen a removed conversation");
            auto const replacement_link = link_action(0, true);
            check(replacement_link != original_link, "Recreate generates a fresh link in Qt");
            join_link(replacement_link);
            wait([&] { return pages[2]->active_conversation() == group && pages[2]->messages_ready(); });
            auto approval_action = [&](int actor, bool required) {
                bool finished = false, submitted = false;
                int polls = 0;
                QTimer approval_poll;
                QObject::connect(&approval_poll, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                    if (++polls > 250) { approval_poll.stop(); if (dialog) { dialog->reject(); } return; }
                    if (!dialog || dialog->findChild<QListWidget*>("groupMembersList")->count() < 2) { return; }
                    auto* checkbox = dialog->findChild<QCheckBox*>("groupJoinApprovalCheck");
                    if (!submitted && checkbox->isEnabled())
                    {
                        check(checkbox->isChecked() != required, "Approval toggle starts from authoritative current setting");
                        submitted = true;
                        checkbox->click();
                    }
                    else if (submitted && checkbox->isEnabled() && checkbox->isChecked() == required)
                    {
                        finished = true;
                        approval_poll.stop();
                        dialog->accept();
                    }
                });
                approval_poll.start(20);
                windows[actor]->findChild<QPushButton*>("chatHeaderButton")->click();
                check(finished, "Approval setting completes through actual Qt controls");
                wait([&] { return pages[0]->conversation(group)->join_approval == required && pages[1]->conversation(group)->join_approval == required; });
            };
            auto review_application = [&](int actor, bool accept) {
                bool finished = false, submitted = false;
                int polls = 0;
                QTimer review_poll;
                QObject::connect(&review_poll, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                    if (++polls > 250) { review_poll.stop(); if (dialog) { dialog->reject(); } return; }
                    if (!dialog || dialog->findChild<QListWidget*>("groupMembersList")->count() < 2) { return; }
                    auto* tabs = dialog->findChild<QTabWidget*>("groupTabs");
                    auto* requests = dialog->findChild<QListWidget*>("groupJoinRequestsList");
                    tabs->setCurrentIndex(2);
                    if (!submitted && requests->count() == 1)
                    {
                        check(requests->item(0)->data(Qt::UserRole).toLongLong() == ids[2], "Application shows actual requester, not a group member");
                        requests->setCurrentRow(0);
                        auto* button = dialog->findChild<QPushButton*>(accept ? "groupAcceptRequestButton" : "groupRejectRequestButton");
                        if (!button->isEnabled()) { return; }
                        dialog->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_join_request.png");
                        submitted = true;
                        button->click();
                    }
                    else if (submitted && requests->count() == 0)
                    {
                        finished = true;
                        review_poll.stop();
                        dialog->accept();
                    }
                });
                review_poll.start(20);
                windows[actor]->findChild<QPushButton*>("chatHeaderButton")->click();
                check(finished, "Manager decides an application through real Qt buttons");
            };
            approval_action(0, true);
            member_action(0, ids[2], "groupRemoveButton");
            wait([&] { return pages[2]->active_conversation() == 0 && !pages[2]->conversation(group); });
            join_link(replacement_link);
            wait([&] { return std::ranges::any_of(windows[2]->findChildren<QLabel*>(), [](auto* label) {
                return label->text().contains(QStringLiteral("申请"));
            }); });
            check(pages[2]->active_conversation() == 0 && !pages[2]->conversation(group) &&
                windows[2]->findChild<QListView*>("messageList")->model()->rowCount() == 0,
                "Pending Qt join never opens group history or injects a conversation");
            join_link(replacement_link);
            review_application(1, false);
            wait([&] { return std::ranges::any_of(windows[2]->findChildren<QLabel*>(), [](auto* label) {
                return label->text().contains(QStringLiteral("被拒绝"));
            }); });
            check(!pages[2]->conversation(group), "Reject result is realtime and grants no membership");
            join_link(replacement_link);
            wait([&] {
                auto const query = "SELECT user_id FROM group_join_requests WHERE conversation_id=" + std::to_string(group) +
                    " AND user_id=" + std::to_string(ids[2]);
                std::unique_ptr<PGresult, decltype(&PQclear)> pending(PQexec(db, query.c_str()), &PQclear);
                return pending && PQresultStatus(pending.get()) == PGRES_TUPLES_OK && PQntuples(pending.get()) == 1;
            });
            server.terminate();
            check(server.waitForFinished(3000), "Restart with pending application");
            wait([&] { return !windows[0]->findChild<QToolButton*>("sendButton")->isEnabled(); });
            start();
            for (int i = 0; i < 2; ++i)
            { wait([&, i] { return pages[i]->messages_ready() && windows[i]->findChild<QToolButton*>("sendButton")->isEnabled(); }); }
            wait([&] { return windows[2]->findChild<QToolButton*>("joinGroupButton")->isEnabled(); });
            check(!pages[2]->conversation(group) && pages[2]->active_conversation() == 0, "Reconnect retains pending isolation");
            review_application(0, true);
            wait([&] { return pages[2]->conversation(group) && pages[2]->conversation(group)->member_count == 3; });
            select_group(2);
            wait([&] { return pages[2]->messages_ready() && pages[2]->active_conversation() == group; });
            check(pages[2]->conversation(group)->join_approval, "Acceptance restores authoritative group and approval metadata after reconnect");
            approval_action(1, false);
            std::cout << "PASS Qt approval toggle, pending isolation, repeat request, administrator rejection, pending reconnect and owner acceptance\n";
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
                check(list->item(0)->text().contains(QStringLiteral("群主")) &&
                    list->item(1)->text().contains(QStringLiteral("管理员")), "Ownership roles recover in Qt role groups after reconnect");
                list->setCurrentRow(1);
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
            check(link_action(1, std::nullopt) == replacement_link, "Invite link persists through reconnect and owner transfer");
            for (auto* page : pages) { check(page->conversation(group)->announcement == announcement_text, "Announcement survives reconnect and ownership transfer"); }
            announcement_action(0, QStringLiteral("转让后管理员更新的公告"));
            announcement_action(1, {}, true);
            for (auto* page : pages)
            {
                check(page->avatars().state(ids[0]) == chat::avatar_state{3, false} && page->avatars().image(ids[0]).isNull(),
                    "Cleared avatar remains fallback after reconnect");
            }
            for (int i = 0; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("messageList");
                bool restored = false;
                for (int row = 0; row < view->model()->rowCount(); ++row)
                {
                    auto const item = view->model()->index(row, 0);
                    if (item.data(message_model::id_role).toLongLong() != mention_message) { continue; }
                    auto const mentions = item.data(message_model::mentions_role).value<QList<mention_data>>();
                    restored = mentions.size() == 1 && mentions.front().user == ids[2] &&
                        item.data(message_model::mentioned_role).toBool() == (i == 2);
                }
                check(restored, "Reconnect and rejoin restore historical mention facts");
                wait([&, i] { return pages[i]->conversation(group)->pinned_message.id == mention_message; });
            }
            windows[0]->findChild<QToolButton*>("unpinMessageButton")->click();
            for (int i = 0; i < 3; ++i) { wait([&, i] { return pages[i]->conversation(group)->pinned_message.id == 0; }); }
            pages[1]->group_message_pin_requested(group, mention_message);
            for (int i = 0; i < 3; ++i) { wait([&, i] { return pages[i]->conversation(group)->pinned_message.id == mention_message; }); }
            pages[1]->delete_message_requested(group, mention_message);
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return pages[i]->conversation(group)->pinned_message.id == 0 &&
                    !windows[i]->findChild<QPushButton*>("pinnedMessageButton")->isVisible(); });
            }
            auto const before_group_image = pages[0]->latest_message_id();
            pages[0]->attachment_send_requested(group, "group.png", image_bytes, 0);
            auto* group_images = windows[2]->findChild<QListView*>("messageList");
            wait([&] {
                auto const index = group_images->model()->index(group_images->model()->rowCount() - 1, 0);
                return pages[2]->latest_message_id() > before_group_image &&
                    !index.data(message_model::image_role).value<QPixmap>().isNull();
            });
            windows[2]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_image_bubble.png");
            bool opened_group_image = false;
            QTimer::singleShot(50, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "attachmentDialog" &&
                    !dialog->findChild<QLabel*>("attachmentImage")->pixmap().isNull(), "Group image bubble opens cached preview");
                opened_group_image = true;
                dialog->reject();
            });
            auto const image_row = group_images->model()->index(group_images->model()->rowCount() - 1, 0);
            auto const image_point = group_images->visualRect(image_row).topLeft() + QPoint(120, 100);
            QMouseEvent image_press(QEvent::MouseButtonPress, QPointF(image_point), QPointF(image_point),
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(group_images->viewport(), &image_press);
            QMouseEvent image_release(QEvent::MouseButtonRelease, QPointF(image_point), QPointF(image_point),
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(group_images->viewport(), &image_release);
            check(opened_group_image, "Real group image bubble click");
            check(std::ranges::any_of(notifications[1], [](auto const& value) { return value.summary == QStringLiteral("图片：photo.png"); }) &&
                std::ranges::any_of(notifications[2], [](auto const& value) { return value.summary == QStringLiteral("文件：removal.bin"); }),
                "Image and ordinary file notifications use compact summaries");
            activate(1);
            set_preference(1, direct, true, true, true);
            auto* pinned_list = windows[1]->findChild<QListView*>("conversationList");
            wait([&] { return pinned_list->model()->index(0, 0).data(conversation_model::id_role).toLongLong() == direct; });
            check(!pages[0]->conversation(direct)->pinned, "Qt pin is private");
            pages[0]->send_message_requested(group, QStringLiteral("新群消息不挤掉个人置顶"), 0);
            wait([&] { return pages[1]->conversation(group)->last_text == QStringLiteral("新群消息不挤掉个人置顶"); });
            check(pinned_list->model()->index(0, 0).data(conversation_model::id_role).toLongLong() == direct,
                "New ordinary conversation activity stays below pinned tier");
            windows[1]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_conversation_pin.png");
            set_preference(1, direct, false, true);
            wait([&] { return pinned_list->model()->index(0, 0).data(conversation_model::id_role).toLongLong() == group; });
            set_preference(1, direct, true, false, true);
            auto const other_conversation_notices = notifications[1].size();
            activate(0);
            pages[0]->send_message_requested(direct, QStringLiteral("静音单聊仍实时到达"), 0);
            wait([&] { return pages[1]->conversation(direct)->last_text == QStringLiteral("静音单聊仍实时到达") &&
                pages[1]->conversation(direct)->unread > 0; });
            check(notifications[1].size() == other_conversation_notices && !pages[0]->conversation(direct)->muted,
                "Personal direct mute preserves realtime activity and unread");
            windows[1]->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_conversation_mute.png");
            set_preference(1, direct, false);
            activate(1);
            pages[0]->send_message_requested(direct, QStringLiteral("前台查看其他会话"), 0);
            wait([&] { return notifications[1].size() == other_conversation_notices + 1; });
            check(notifications[1].back().conversation == direct && notifications[1].back().title == names[0],
                "Foreground window still notifies for another conversation");
            auto* direct_list = windows[1]->findChild<QListView*>("conversationList");
            QModelIndex notice_conversation;
            wait([&] {
                for (int row = 0; row < direct_list->model()->rowCount(); ++row)
                {
                    auto const index = direct_list->model()->index(row, 0);
                    if (index.data(conversation_model::id_role).toLongLong() == direct) { notice_conversation = index; return true; }
                }
                return false;
            });
            direct_list->clicked(notice_conversation);
            wait([&] { return pages[1]->active_conversation() == direct && pages[1]->messages_ready(); });
            windows[1]->showMinimized();
            wait([&] { return windows[1]->isMinimized(); });
            auto const minimized_notices = notifications[1].size();
            pages[0]->send_message_requested(direct, QStringLiteral("最小化后的消息"), 0);
            wait([&] { return notifications[1].size() == minimized_notices + 1; });
            auto* tray = windows[1]->findChild<QSystemTrayIcon*>("notificationTray");
            check(tray && QMetaObject::invokeMethod(tray, "messageClicked", Qt::DirectConnection), "Notification restore signal wiring");
            wait([&] { return !windows[1]->isMinimized() && windows[1]->isActiveWindow(); });
            auto const foreground_notices = notifications[1].size();
            auto const long_message = QStringLiteral("history line\n").repeated(60);
            pages[0]->send_message_requested(direct, long_message, 0);
            auto* direct_messages = windows[1]->findChild<QListView*>("messageList");
            auto* read_model = dynamic_cast<message_model*>(direct_messages->model());
            check(read_model, "Real conversation message model");
            wait([&] {
                return direct_messages->model()->index(direct_messages->model()->rowCount() - 1, 0).data(message_model::text_role).toString() == long_message &&
                    direct_messages->verticalScrollBar()->maximum() > 200 &&
                    direct_messages->verticalScrollBar()->value() == direct_messages->verticalScrollBar()->maximum() &&
                    read_model->read_position(ids[1]) >= pages[1]->latest_message_id();
            });
            check(notifications[1].size() == foreground_notices, "Foreground latest view has no extra notification");
            auto* direct_scroll = direct_messages->verticalScrollBar();
            direct_scroll->setValue(direct_scroll->maximum() - 150);
            auto const history_scroll = direct_scroll->value();
            auto const previous_read = pages[1]->latest_message_id();
            auto const sender_notices = notifications[0].size();
            pages[0]->send_message_requested(direct, long_message + QStringLiteral("\n历史阅读时的新消息"), 0);
            wait([&] { return notifications[1].size() == foreground_notices + 1 && pages[1]->latest_message_id() > previous_read; });
            check(direct_scroll->value() == history_scroll && notifications[1].back().summary.size() == 120 &&
                notifications[0].size() == sender_notices, "History scroll stays fixed; notification is compact and sender is excluded");
            auto const notified_message = pages[1]->latest_message_id();
            auto read_query = "SELECT last_read_message_id FROM conversation_members WHERE conversation_id=" +
                std::to_string(direct) + " AND user_id=" + std::to_string(ids[1]);
            auto* history_read = PQexec(db, read_query.c_str());
            check(PQresultStatus(history_read) == PGRES_TUPLES_OK && PQntuples(history_read) == 1 &&
                std::stoll(PQgetvalue(history_read, 0, 0)) < notified_message, "Viewing old history does not read the new message");
            PQclear(history_read);
            direct_messages->scrollToBottom();
            wait([&] {
                auto* read = PQexec(db, read_query.c_str());
                bool const seen = PQresultStatus(read) == PGRES_TUPLES_OK && PQntuples(read) == 1 &&
                    std::stoll(PQgetvalue(read, 0, 0)) >= notified_message;
                PQclear(read);
                return seen;
            });
            wait([&] { return read_model->read_position(ids[1]) >= notified_message; });
            int redundant_reads = 0;
            auto const read_capture = QObject::connect(pages[1], &chat_widget::read_requested, windows[1].get(),
                [&](qint64, qint64) { ++redundant_reads; });
            for (int i = 0; i < 10; ++i)
            {
                direct_scroll->setValue(direct_scroll->maximum() - 20);
                direct_messages->scrollToBottom();
            }
            check(redundant_reads == 0, "Already acknowledged read position prevents duplicate scroll writes");
            QObject::disconnect(read_capture);
            pages[0]->edit_message_requested(direct, notified_message, QStringLiteral("已编辑通知原消息"));
            wait([&] { return direct_messages->model()->index(direct_messages->model()->rowCount() - 1, 0)
                .data(message_model::text_role).toString() == QStringLiteral("已编辑通知原消息"); });
            pages[0]->delete_message_requested(direct, notified_message);
            wait([&] { return direct_messages->model()->index(direct_messages->model()->rowCount() - 1, 0)
                .data(message_model::deleted_role).toBool(); });
            check(notifications[1].size() == foreground_notices + 1, "Edit and deletion do not generate ordinary notifications");
            avatar_update(avatar_path, false, true);
            wait([&] { return pages[0]->avatars().state(ids[0]) == chat::avatar_state{4, true} && !pages[0]->avatars().image(ids[0]).isNull(); });
            QTimer::singleShot(20, [&] {
                auto* profile = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(profile && profile->objectName() == "profileDialog", "Account opens from bottom avatar");
                QTimer::singleShot(20, [] {
                    auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    check(confirmation && confirmation->text().contains(QStringLiteral("登录页")), "Logout confirmation");
                    confirmation->button(QMessageBox::Yes)->click();
                });
                profile->findChild<QPushButton*>("profileLogoutButton")->click();
            });
            windows[0]->findChild<QToolButton*>("profileAvatar")->click();
            wait([&] { return !pages[0]->isVisible() && windows[0]->findChild<QPushButton*>("loginButton")->isEnabled(); });
            check(pages[0]->avatars().image(ids[0]).isNull(), "Logout clears current account cache");
            check(pages[0]->images().bytes(pages[2]->latest_message_id()).isEmpty(), "Logout clears image cache");
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
            auto seed = "INSERT INTO messages(sender_id,conversation_id,body) VALUES(" + std::to_string(ids[1]) + "," +
                std::to_string(group) + ",E'\\n  \\n较早的置顶消息') RETURNING id";
            auto* early = PQexec(db, seed.c_str());
            check(PQresultStatus(early) == PGRES_TUPLES_OK && PQntuples(early) == 1, "Persist older pinned-message fixture");
            auto const early_id = std::stoll(PQgetvalue(early, 0, 0));
            PQclear(early);
            seed = "INSERT INTO messages(sender_id,conversation_id,body) SELECT " + std::to_string(ids[1]) + "," +
                std::to_string(group) + ",'分页消息 '||n::text FROM generate_series(1,60) n";
            auto* newer = PQexec(db, seed.c_str());
            check(PQresultStatus(newer) == PGRES_COMMAND_OK, "Persist messages beyond current history page");
            PQclear(newer);
            pages[1]->group_message_pin_requested(group, early_id);
            wait([&] { return pages[0]->conversation(group)->pinned_message.id == early_id; });
            pages[0]->open_conversation(*pages[0]->conversation(group));
            auto* recent = windows[0]->findChild<QListView*>("messageList");
            wait([&] { return recent->model()->rowCount() == 50 && pages[0]->messages_ready(); });
            check(recent->model()->index(0, 0).data(message_model::id_role).toLongLong() > early_id,
                "Pinned target is older than the current history page");
            QTimer::singleShot(20, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "messageSearchDialog", "Older pin opens existing search");
                auto* results = dialog->findChild<QListView*>("messageSearchResults");
                wait([&] { return results->model()->rowCount() == 1; });
                check(results->model()->index(0, 0).data(message_model::id_role).toLongLong() == early_id,
                    "Pinned summary searches the persisted message beyond cursor page");
                dialog->reject();
            });
            windows[0]->findChild<QPushButton*>("pinnedMessageButton")->click();
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
