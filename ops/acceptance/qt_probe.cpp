#include <iostream>
#include <QListWidget>
#include <QScrollBar>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QKeyEvent>
#include <QInputDialog>
#include <QLabel>
#include <QAbstractButton>
#include <QFileDialog>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSocketNotifier>
#include <QTimer>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QMessageBox>
#include <QPushButton>
#include "main_window.hpp"
#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include "download_dialog.hpp"
#include <optional>
#include <tuple>
#include <utility>
#include "conversation_model.hpp"
#include "message_model.hpp"
#include "theme_manager.hpp"
#include "theme.hpp"
#include "settings_dialog.hpp"

namespace
{
void choose_menu_action(QWidget* view, QPoint point, QString const& label)
{
    QTimer::singleShot(0,
                       view,
                       [label]
                       {
                           auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                           if (!menu)
                           {
                               return;
                           }
                           for (auto* action : menu->actions())
                           {
                               if (action->text() == label)
                               {
                                   menu->setActiveAction(action);
                                   QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                                   QApplication::sendEvent(menu, &key);
                                   return;
                               }
                           }
                           menu->close();
                       });
    emit view->customContextMenuRequested(point);
}

template <class... Args>
struct completion_barrier
{
    bool held = false;
    std::optional<std::tuple<Args...>> completion;

    template <class Deliver>
    void receive(Deliver deliver, Args... args)
    {
        if (held)
        {
            Q_ASSERT(!completion);
            completion.emplace(std::move(args)...);
        }
        else
        {
            deliver(std::move(args)...);
        }
    }

    template <class Deliver>
    void release(Deliver deliver)
    {
        held = false;
        if (auto done = std::exchange(completion, std::nullopt))
        {
            std::apply(deliver, std::move(*done));
        }
    }
};

struct probe_controls
{
    completion_barrier<qint64, QString> text_save;
    completion_barrier<qint64, quint64, QString> download;
};

QJsonObject dispatch(main_window& window, probe_controls& controls, QJsonObject const& cmd)
{
    auto* edit = window.findChild<QPlainTextEdit*>("messageEdit");
    auto* conv = window.findChild<QListView*>("conversationList");
    auto* view = window.findChild<QListView*>("messageList");
    auto* chat = window.findChild<chat_widget*>();
    auto* model = view->model();
    auto button = [&window](const char* name) { return window.findChild<QAbstractButton*>(name); };
    auto op = cmd["op"].toString();
    QJsonObject out;
    if (op == "login")
    {
        window.findChild<QLineEdit*>("loginUsernameEdit")->setText(cmd["user"].toString());
        window.findChild<QLineEdit*>("loginPasswordEdit")->setText(cmd["password"].toString(QStringLiteral("pass")));
        button("loginButton")->click();
    }
    else if (op == "devices")
    {
        if (cmd["open"].toBool())
        {
            window.show_settings();
            auto* open = button("settingsDevicesButton");
            QTimer::singleShot(0, open, &QAbstractButton::click);
        }
        auto* dialog = window.findChild<QDialog*>("loginSessionsDialog");
        if (dialog && dialog->isVisible())
        {
            auto* list = dialog->findChild<QTreeWidget*>("loginSessionsList");
            QJsonArray rows;
            for (int i = 0; i < list->topLevelItemCount(); ++i)
            {
                auto* row = list->topLevelItem(i);
                auto const id = row->data(0, Qt::UserRole).toLongLong();
                rows << QJsonObject{{"id", id},
                                    {"name", row->text(0)},
                                    {"type", row->text(1)},
                                    {"created", row->text(2)},
                                    {"seen", row->text(3)},
                                    {"current", row->data(0, Qt::UserRole + 1).toBool()}};
                if (cmd["select"].toInteger() == id)
                {
                    list->setCurrentItem(row);
                }
            }
            out["rows"] = rows;
            out["status"] = dialog->findChild<QLabel*>("loginSessionsStatus")->text();
            out["revoke_enabled"] = button("revokeLoginSession")->isEnabled();
            out["other_enabled"] = button("revokeOtherLoginSessions")->isEnabled();
            if (cmd["revoke"].toBool())
            {
                auto* revoke = button("revokeLoginSession");
                QTimer::singleShot(0, revoke, &QAbstractButton::click);
            }
            if (cmd["others"].toBool())
            {
                auto* revoke = button("revokeOtherLoginSessions");
                QTimer::singleShot(0, revoke, &QAbstractButton::click);
            }
            if (cmd.contains("new"))
            {
                dialog->findChild<QLineEdit*>("currentLoginPassword")->setText(cmd["password"].toString());
                dialog->findChild<QLineEdit*>("newLoginPassword")->setText(cmd["new"].toString());
                dialog->findChild<QLineEdit*>("repeatLoginPassword")->setText(cmd["new"].toString());
                button("changeLoginPassword")->click();
            }
            if (cmd["close"].toBool())
            {
                dialog->reject();
                window.settings_->reject();
            }
        }
    }
    else if (op == "login_state")
    {
        out["user"] = chat->self_user();
        out["status"] = window.status_label_->text();
        out["reconnecting"] = window.reconnecting_;
        out["password_empty"] = window.password_edit_->text().isEmpty();
    }
    else if (op == "draft_debounce")
    {
        if (cmd["hold"].toBool())
        {
            chat->draft_timer_->setInterval(60000);
        }
        out["pending"] = chat->draft_timer_->isActive();
    }
    else if (op == "select")
    {
        auto* cm = static_cast<conversation_model*>(conv->model());
        auto idx = cm->index_for_conversation(cmd["conv"].toInteger());
        out["ok"] = idx.isValid();
        if (idx.isValid())
        {
            conv->setCurrentIndex(idx);
            QMetaObject::invokeMethod(conv->itemDelegate(), "body_clicked", Q_ARG(QModelIndex, idx));
        }
    }
    else if (op == "send")
    {
        if (cmd.contains("text"))
        {
            edit->setPlainText(cmd["text"].toString());
        }
        out["enabled"] = button("sendButton")->isEnabled();
        button("sendButton")->click();
    }
    else if (op == "group")
    {
        auto id = cmd["id"].toInteger();
        QTimer::singleShot(0, chat, [chat, id] { emit chat->members_requested(id, chat->self_user(), "QA"); });
    }
    else if (op == "moderate")
    {
        auto* dialog = window.findChild<QDialog*>("groupDialog");
        out["open"] = dialog != nullptr;
        if (dialog)
        {
            auto* mute = dialog->findChild<QCheckBox*>("groupMuteAll");
            auto* dissolve = dialog->findChild<QPushButton*>("groupDissolveButton");
            out["manager"] = mute->isEnabled();
            out["muted"] = mute->isChecked();
            out["owner"] = dissolve->isEnabled();
            if (cmd["toggle"].toBool())
            {
                mute->click();
            }
            if (cmd["members"].toBool())
            {
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                if (cmd["showList"].toBool())
                {
                    dialog->findChild<QTabWidget*>("groupTabs")->setCurrentWidget(list->parentWidget());
                    list->setFixedHeight(list->sizeHintForRow(0) + 2 * list->frameWidth());
                    list->scrollToBottom();
                }
                auto describe = [](QListWidget* entries)
                {
                    QJsonArray rows;
                    for (int row = 0; row < entries->count(); ++row)
                    {
                        auto const* item = entries->item(row);
                        rows.append(QJsonObject{{"id", item->data(Qt::UserRole).toLongLong()},
                                                {"status", item->statusTip()},
                                                {"accessible", item->data(Qt::AccessibleTextRole).toString()},
                                                {"tooltip", item->toolTip()}});
                    }
                    return rows;
                };
                out["list"] = describe(list);
                out["preview"] = describe(dialog->findChild<QListWidget*>("groupMemberPreview"));
                out["selected"] = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : 0;
                out["scroll"] = list->verticalScrollBar()->value();
                for (auto* control : dialog->findChildren<QPushButton*>())
                {
                    if (control->text() == "只上传到群文件")
                    {
                        out["publish"] = control->isEnabled();
                    }
                }
            }
            if (cmd.contains("member"))
            {
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                for (int row = 0; row < list->count(); ++row)
                {
                    if (list->item(row)->data(Qt::UserRole).toLongLong() == cmd["member"].toInteger())
                    {
                        list->setCurrentRow(row);
                        auto* control = dialog->findChild<QPushButton*>("groupMemberMuteButton");
                        out["member_enabled"] = control->isEnabled();
                        if (control->isEnabled() && cmd.contains("duration"))
                        {
                            QTimer::singleShot(0,
                                               control,
                                               [choice = cmd["duration"].toInt()]
                                               {
                                                   if (auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                                                   {
                                                       input->findChild<QComboBox*>()->setCurrentIndex(choice);
                                                       input->accept();
                                                   }
                                               });
                            control->click();
                        }
                        break;
                    }
                }
            }
            if (cmd["dissolve"].toBool() && dissolve->isEnabled())
            {
                QTimer::singleShot(0,
                                   dissolve,
                                   [dissolve]
                                   {
                                       QTimer::singleShot(0,
                                                          dissolve,
                                                          []
                                                          {
                                                              if (auto* confirmation = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                                                              {
                                                                  confirmation->button(QMessageBox::Yes)->click();
                                                              }
                                                          });
                                       dissolve->click();
                                   });
            }
            if (cmd["close"].toBool())
            {
                dialog->reject();
            }
        }
    }
    else if (op == "outbox_retry")
    {
        emit chat->pending_text_action_requested(cmd["id"].toInteger(), false);
    }
    else if (op == "recover_local")
    {
        auto* menu = window.findChild<QMenu*>("unavailableLocalMessages");
        auto* parent_menu = qobject_cast<QMenu*>(menu->parentWidget());
        QMetaObject::invokeMethod(parent_menu, "aboutToShow");
        QJsonArray rows;
        for (auto* action : menu->actions())
        {
            rows.append(action->text());
            if (cmd["row"].toString() == action->text() && action->menu())
            {
                for (auto* child : action->menu()->actions())
                {
                    if (child->text() == cmd["action"].toString() && child->isEnabled())
                    {
                        child->trigger();
                        out["triggered"] = true;
                    }
                }
            }
        }
        out["rows"] = rows;
        out["clipboard"] = QApplication::clipboard()->text();
    }
    else if (op == "pin")
    {
        auto* dialog = window.findChild<QDialog*>("groupDialog");
        if (dialog)
        {
            out["error"] = dialog->findChild<QLabel*>("groupStatus")->text();
            out["pin"] = dialog->findChild<QLabel*>("groupOverviewPinned")->text();
            out["announcement"] = dialog->findChild<QLabel*>("groupOverviewAnnouncement")->text();
            if (cmd["close"].toBool())
            {
                dialog->reject();
            }
        }
    }
    else if (op == "upload")
    {
        auto path = cmd["path"].toString();
        QTimer::singleShot(40,
                           chat,
                           [path]
                           {
                               auto* f = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                               if (f)
                               {
                                   f->findChild<QLineEdit*>("fileNameEdit")->setText(path);
                                   QMetaObject::invokeMethod(f, "accept");
                               }
                           });
        chat->choose_attachment(cmd["conv"].toInteger(), cmd["group"].toBool(), chat);
    }
    else if (op == "transfers")
    {
        QJsonArray labels;
        for (auto* menu : chat->findChildren<QMenu*>())
        {
            if (menu->title() != "附件上传")
            {
                continue;
            }
            QMetaObject::invokeMethod(menu, "aboutToShow");
            for (auto* action : menu->actions())
            {
                if (!action->menu())
                {
                    continue;
                }
                labels << action->text();
                if (action->text() == cmd["label"].toString())
                {
                    for (auto* button : action->menu()->actions())
                    {
                        if (button->text() == cmd["action"].toString() && button->isEnabled())
                        {
                            button->trigger();
                            out["triggered"] = true;
                        }
                    }
                }
            }
        }
        out["labels"] = labels;
    }
    else if (op == "clear")
    {
        auto id = cmd["conv"].toInteger();
        emit chat->history_view_requested(id, false);
    }
    else if (op == "confirm")
    {
        auto* box = window.findChild<QMessageBox*>("confirmationDialog");
        out["open"] = box && box->isVisible();
        if (box && box->isVisible() && cmd["accept"].toBool(true))
        {
            box->button(QMessageBox::Yes)->click();
        }
    }
    else if (op == "preview")
    {
        auto id = cmd["id"].toInteger(), c = cmd["conv"].toInteger();
        auto const filename = cmd["filename"].toString("inspect.png");
        auto const inline_preview = cmd["inline"].toBool(true);
        QTimer::singleShot(0, chat, [=] { emit chat->attachment_open_requested(c, id, filename, inline_preview); });
    }
    else if (op == "dialogstate")
    {
        auto* d = window.findChild<QDialog*>("attachmentDialog");
        out["shown"] = d && d->isVisible();
        if (d)
        {
            auto* image = d->findChild<QLabel*>("attachmentImage");
            out["image"] = image->isVisible() && !image->pixmap().isNull();
            if (cmd["save"].toBool())
            {
                d->findChild<QPushButton*>("saveAttachmentButton")->click();
            }
            auto* f = d->findChild<QFileDialog*>();
            out["chooser"] = f && f->isVisible();
            if (auto path = cmd["destination"].toString(); f && !path.isEmpty())
            {
                f->findChild<QLineEdit*>("fileNameEdit")->setText(path);
                if (cmd["overwrite"].toBool())
                {
                    QTimer::singleShot(0,
                                       f,
                                       []
                                       {
                                           if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                                           {
                                               box->button(QMessageBox::Yes)->click();
                                           }
                                       });
                }
                QMetaObject::invokeMethod(f, "accept");
            }
            if (cmd["close"].toBool())
            {
                d->reject();
            }
        }
    }
    else if (op == "group_download")
    {
        auto* dialog = window.findChild<QDialog*>("groupDialog");
        if (dialog)
        {
            if (cmd["close"].toBool())
            {
                dialog->reject();
            }
            else
            {
                auto* list = dialog->findChild<QTreeWidget*>("groupFileList");
                auto* tabs = dialog->findChild<QTabWidget*>();
                if (tabs && list && tabs->indexOf(list->parentWidget()) >= 0)
                {
                    tabs->setCurrentWidget(list->parentWidget());
                }
                out["count"] = list ? list->topLevelItemCount() : 0;
                if (list && list->topLevelItemCount() && cmd.contains("destination"))
                {
                    list->setCurrentItem(list->topLevelItem(0));
                    auto const path = cmd["destination"].toString();
                    QTimer::singleShot(40,
                                       dialog,
                                       [path]
                                       {
                                           if (auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                                           {
                                               chooser->findChild<QLineEdit*>("fileNameEdit")->setText(path);
                                               QMetaObject::invokeMethod(chooser, "accept");
                                           }
                                       });
                    for (auto* button : dialog->findChildren<QPushButton*>())
                    {
                        if (button->text() == "下载")
                        {
                            button->click();
                            break;
                        }
                    }
                }
            }
        }
    }
    else if (op == "downloads")
    {
        if (cmd["open"].toBool())
        {
            for (auto* action : window.findChildren<QAction*>())
            {
                if (action->text() == "下载任务")
                {
                    action->trigger();
                }
            }
        }
        if (auto* dialog = window.findChild<QDialog*>("downloadTasksDialog"))
        {
            auto* list = dialog->findChild<QTreeWidget*>("downloadTaskList");
            if (cmd.contains("row"))
            {
                list->setCurrentItem(list->topLevelItem(cmd["row"].toInt()));
            }
            if (cmd.contains("enabled"))
            {
                for (auto* button : dialog->findChildren<QPushButton*>())
                {
                    if (button->text() == cmd["enabled"].toString())
                    {
                        out["enabled"] = button->isEnabled();
                    }
                }
            }
            if (cmd.contains("action"))
            {
                for (auto* button : dialog->findChildren<QPushButton*>())
                {
                    if (button->text() == cmd["action"].toString() && button->isEnabled())
                    {
                        button->click();
                        out["triggered"] = true;
                    }
                }
            }
            QJsonArray rows;
            for (int i = 0; i < list->topLevelItemCount(); ++i)
            {
                auto* row = list->topLevelItem(i);
                rows.append(QJsonObject{{"id", row->data(0, Qt::UserRole).toString()},
                                        {"filename", row->text(0)},
                                        {"source", row->text(1)},
                                        {"path", row->text(2)},
                                        {"progress", row->text(3)},
                                        {"status", row->text(4)}});
            }
            out["rows"] = rows;
        }
    }
    else if (op == "history")
    {
        auto* dialog = window.findChild<QDialog*>("chatHistoryDialog");
        if (!dialog && cmd["open"].toBool())
        {
            auto const conversation = cmd["conv"].toInteger();
            QTimer::singleShot(0, chat, [chat, conversation] { emit chat->chat_history_requested(conversation, "QA"); });
        }
        if (dialog)
        {
            auto* tabs = dialog->findChild<QTabBar*>("historyTabs");
            auto* list = dialog->findChild<QListWidget*>("historyList");
            auto* more = dialog->findChild<QPushButton*>("historyMoreButton");
            if (cmd.contains("tab"))
            {
                tabs->setCurrentIndex(cmd["tab"].toInt());
            }
            if (cmd["more"].toBool())
            {
                more->click();
            }
            if (cmd.contains("query"))
            {
                auto* query = dialog->findChild<QLineEdit*>("historySearchEdit");
                query->setText(cmd["query"].toString());
                emit query->returnPressed();
            }
            if (cmd.contains("select"))
            {
                list->setCurrentRow(cmd["select"].toInt());
            }
            if (cmd.contains("scroll"))
            {
                list->verticalScrollBar()->setValue(cmd["scroll"].toInt());
            }
            if (cmd.contains("copy"))
            {
                if (auto* item = list->item(cmd["copy"].toInt()))
                {
                    list->scrollToItem(item);
                    QApplication::clipboard()->clear();
                    choose_menu_action(list, list->visualItemRect(item).center(), QStringLiteral("复制"));
                    out["copied"] = QApplication::clipboard()->text();
                }
            }
            QJsonArray rows;
            for (int i = 0; i < list->count(); ++i)
            {
                auto* item = list->item(i);
                rows.append(QJsonObject{{"id", item->data(Qt::UserRole).toLongLong()}, {"text", item->data(Qt::UserRole + 3).toString()}});
            }
            out["rows"] = rows;
            out["selected"] = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : 0;
            out["scroll"] = list->verticalScrollBar()->value();
            if (cmd["close"].toBool())
            {
                dialog->reject();
            }
            out["status"] = dialog->findChild<QLabel*>("historyStatus")->text();
            out["count"] = list->count();
            out["more"] = more->isVisible();
            if (cmd["activate"].toBool() && list->count())
            {
                emit list->itemActivated(list->item(0));
            }
        }
    }
    else if (op == "searchui")
    {
        if (cmd["open"].toBool())
        {
            for (auto* a : window.findChildren<QAction*>())
            {
                if (a->text() == "全局消息搜索")
                {
                    QTimer::singleShot(0, a, &QAction::trigger);
                }
            }
        }
        auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (d)
        {
            auto* l = d->findChild<QTreeWidget*>();
            auto* q = d->findChild<QLineEdit*>("globalSearchQuery");
            auto* ctx = d->findChild<QListWidget*>();
            if (l && q && ctx)
            {
                out["open"] = true;
                out["status"] = d->findChild<QLabel*>()->text();
                for (auto* b : d->findChildren<QPushButton*>())
                {
                    if (b->text() == "加载更多")
                    {
                        out["more"] = b->isVisible();
                    }
                }
                if (cmd.contains("category"))
                {
                    d->findChild<QComboBox*>("searchCategory")->setCurrentIndex(cmd["category"].toInt());
                }
                auto* scopes = d->findChild<QComboBox*>("searchConversation");
                if (cmd.contains("conversation"))
                {
                    auto const id = cmd["conversation"].toInteger();
                    auto const index = id ? scopes->findData(id) : 0;
                    if (index >= 0)
                    {
                        scopes->setCurrentIndex(index);
                        out["scope"] = true;
                    }
                }
                QJsonArray scope_ids;
                for (int i = 0; i < scopes->count(); ++i)
                {
                    scope_ids << scopes->itemData(i).toLongLong();
                }
                out["scopes"] = scope_ids;
                out["selected_scope"] = scopes->currentData().toLongLong();
                if (cmd.contains("query"))
                {
                    q->setText(cmd["query"].toString());
                }
                if ((cmd.contains("query") && !cmd["type_only"].toBool()) || cmd["submit"].toBool())
                {
                    QMetaObject::invokeMethod(q, "returnPressed");
                }
                if (cmd["more"].toBool() || cmd["back"].toBool())
                {
                    for (auto* b : d->findChildren<QPushButton*>())
                    {
                        if (b->isVisible() && b->text() == (cmd["more"].toBool() ? "加载更多" : "返回搜索结果"))
                        {
                            b->click();
                        }
                    }
                }
                if (cmd.contains("row"))
                {
                    int i = cmd["row"].toInt();
                    if (i < l->topLevelItemCount())
                    {
                        auto* it = l->topLevelItem(i);
                        l->scrollToItem(it);
                        l->setCurrentItem(it);
                        if (cmd["enter"].toBool())
                        {
                            emit l->itemClicked(it, 0);
                        }
                    }
                }
                QJsonArray ids, snips;
                for (int i = 0; i < l->topLevelItemCount(); ++i)
                {
                    ids << l->topLevelItem(i)->data(1, Qt::UserRole).toLongLong();
                    snips << l->topLevelItem(i)->text(0);
                }
                out["ids"] = ids;
                out["snips"] = snips;
                out["scroll"] = l->verticalScrollBar()->value();
                out["row"] = l->indexOfTopLevelItem(l->currentItem());
                out["ctx"] = ctx->isVisible();
                out["count"] = ctx->count();
                if (cmd["close"].toBool())
                {
                    d->reject();
                }
            }
        }
    }
    else if (op == "filter")
    {
        if (cmd.contains("query"))
        {
            window.findChild<QLineEdit*>("conversationSearchEdit")->setText(cmd["query"].toString());
        }
        QJsonArray names;
        for (int i = 0; i < conv->model()->rowCount(); ++i)
        {
            if (!conv->isRowHidden(i))
            {
                names << conv->model()->index(i, 0).data().toString();
            }
        }
        out["names"] = names;
    }
    else if (op == "download_barrier")
    {
        auto& barrier = controls.download;
        if (cmd["hold"].toBool())
        {
            barrier.held = true;
        }
        if (cmd["release"].toBool())
        {
            barrier.release([&window](qint64 id, quint64 attempt, QString const& error) { window.downloads_->finish(id, attempt, error); });
        }
        out["pending"] = barrier.completion.has_value();
    }
    else if (op == "text_save_barrier")
    {
        auto& barrier = controls.text_save;
        if (cmd["hold"].toBool())
        {
            barrier.held = true;
        }
        if (cmd["release"].toBool())
        {
            barrier.release([chat](qint64 id, QString error) { chat->finish_text_save(id, std::move(error)); });
        }
        out["pending"] = barrier.completion.has_value();
        if (barrier.completion)
        {
            out["error"] = std::get<1>(*barrier.completion);
        }
    }
    else if (op == "logout")
    {
        window.logout();
    }
    else if (op == "quit")
    {
        QTimer::singleShot(0, qApp, &QApplication::quit);
    }
    else if (op == "reply_to")
    {
        for (int i = 0; i < model->rowCount(); ++i)
        {
            auto const idx = model->index(i, 0);
            if (idx.data(message_model::id_role).toLongLong() == cmd["id"].toInteger())
            {
                view->scrollTo(idx);
                choose_menu_action(view, view->visualRect(idx).center(), QStringLiteral("回复"));
                break;
            }
        }
        out["reply"] = button("cancelReplyButton")->isVisible();
    }
    else if (op == "draft")
    {
        edit->setPlainText(cmd["text"].toString());
    }
    else if (op == "state")
    {
        out["user"] = chat->self_user();
        out["conv"] = chat->active_conversation();
        out["enabled"] = edit->isEnabled();
        out["sendEnabled"] = button("sendButton")->isEnabled();
        auto const* notice = window.findChild<QLabel*>("groupSpeechNotice");
        out["speech"] = notice->isVisible() ? notice->text() : QString{};
        out["draft"] = edit->toPlainText();
        out["reply"] = button("cancelReplyButton")->isVisible();
        out["preview"] = window.findChild<QLabel*>("replyPreview")->text();
        QJsonArray rows;
        for (int i = 0; i < model->rowCount(); ++i)
        {
            auto idx = model->index(i, 0);
            rows.append(QJsonObject{{"text", idx.data(message_model::text_role).toString()}, {"id", idx.data(message_model::id_role).toLongLong()}});
        }
        out["rows"] = rows;
    }
    return out;
}
}    // namespace

int main(int argc, char** argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (argc != 2)
    {
        return 2;
    }
    QApplication::setOrganizationName("chat");
    QApplication::setApplicationName("chat_qt");
    theme_manager::instance().load_settings();
    if (!theme_manager::instance().valid())
    {
        return 2;
    }
    main_window window(QString::fromUtf8(argv[1]));
    window.show();
    auto* chat = window.findChild<chat_widget*>();
    auto* bridge = window.client_.get();
    probe_controls controls;
    auto* downloads = window.downloads_;
    QObject::disconnect(bridge, &client_bridge::text_saved, chat, &chat_widget::finish_text_save);
    QObject::connect(
        bridge,
        &client_bridge::text_saved,
        chat,
        [&](qint64 id, QString const& error)
        { controls.text_save.receive([chat](qint64 task, QString result) { chat->finish_text_save(task, std::move(result)); }, id, error); });
    QObject::disconnect(bridge, &client_bridge::download_finished, downloads, &download_dialog::finish);
    QObject::connect(
        bridge,
        &client_bridge::download_finished,
        downloads,
        [&](qint64 id, quint64 attempt, QString const& error)
        {
            controls.download.receive(
                [downloads](qint64 task, quint64 run, QString const& result) { downloads->finish(task, run, result); }, id, attempt, error);
        });
    QSocketNotifier input(0, QSocketNotifier::Read);
    QObject::connect(&input,
                     &QSocketNotifier::activated,
                     &app,
                     [&]
                     {
                         std::string line;
                         if (!std::getline(std::cin, line))
                         {
                             app.quit();
                             return;
                         }
                         auto cmd = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
                         auto out = dispatch(window, controls, cmd);
                         std::cout << QJsonDocument(out).toJson(QJsonDocument::Compact).constData() << std::endl;
                     });
    return app.exec();
}
