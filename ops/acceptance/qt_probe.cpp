#include <iostream>
#include <QListWidget>
#include <QScrollBar>
#include <QScreen>
#include <QStyleHints>
#include <QStyleOptionSlider>
#include <QTextBlock>
#include <QTextLayout>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QDialogButtonBox>
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
#include <QToolButton>
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
#include "conversation_delegate.hpp"
#include "message_model.hpp"
#include "user_model.hpp"
#include "theme_manager.hpp"
#include "theme.hpp"
#include "settings_dialog.hpp"

namespace
{
void choose_menu_action(QWidget* view, QPoint point, QString const& label, QString const& child = {})
{
    QTimer::singleShot(0,
                       view,
                       [label, child]
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
                                   if (!child.isEmpty() && action->menu())
                                   {
                                       for (auto* item : action->menu()->actions())
                                       {
                                           if (item->text() == child && item->isEnabled())
                                           {
                                               item->trigger();
                                               break;
                                           }
                                       }
                                       menu->close();
                                       return;
                                   }
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
        out["reconnectTimers"] = window.reconnect_timer_->isActive() || window.reconnect_countdown_timer_->isActive();
        auto* connection = window.findChild<QAbstractButton*>("connectionStatusButton");
        out["connectionText"] = connection->isVisible() ? connection->text() : QString{};
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
    else if (op == "contacts")
    {
        if (cmd["open"].toBool())
        {
            chat->show_contacts_dialog();
        }
        auto* dialog = chat->contacts_dialog_;
        if (cmd.contains("filter"))
        {
            chat->contact_search_->setText(cmd["filter"].toString());
        }
        if (cmd.contains("group"))
        {
            auto* groups = chat->friend_groups_;
            groups->setCurrentIndex(groups->findText(cmd["group"].toString()));
        }
        if (cmd["requests"].toBool())
        {
            chat->contacts_requests_button_->click();
        }
        if (cmd["accept"].toBool())
        {
            button("friendRequestAcceptButton")->click();
        }
        if (cmd["reject"].toBool())
        {
            button("friendRequestRejectButton")->click();
        }
        if (cmd["selfProfile"].toBool())
        {
            QTimer::singleShot(0, chat, [chat] { chat->show_user_details(chat->self_user(), QStringLiteral("a")); });
        }
        if (cmd["profile"].toBool())
        {
            auto const index = chat->contacts_view_->model()->index(0, 0);
            if (index.isValid())
            {
                auto const id = index.data(user_model::id_role).toLongLong();
                auto const name = index.data(user_model::username_role).toString();
                QTimer::singleShot(0, chat, [chat, id, name] { chat->show_user_details(id, name); });
            }
        }
        if (auto* profile = chat->findChild<QDialog*>("profileDialog"))
        {
            out["profile"] = profile->findChild<QLabel*>("profileDialogName")->text();
            auto* editor = profile->findChild<QPushButton*>("editUserProfile");
            out["signature"] = profile->findChild<QLabel*>("profileSignatureText")->text();
            out["canEdit"] = editor->isVisible() && editor->isEnabled();
            if (cmd.contains("nickname") && out["canEdit"].toBool())
            {
                QTimer::singleShot(0,
                                   profile,
                                   [nick = cmd["nickname"].toString(), sig = cmd["signature"].toString()]
                                   {
                                       auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                                       d->findChild<QLineEdit*>("profileNickname")->setText(nick);
                                       d->findChild<QLineEdit*>("profileSignature")->setText(sig);
                                       d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
                                   });
                editor->click();
            }
            auto* remove = profile->findChild<QPushButton*>("removeContactButton");
            out["friend"] = remove && remove->isVisible();
            if (cmd["closeProfile"].toBool())
            {
                profile->reject();
            }
        }
        QJsonArray contacts;
        auto const* contacts_model = chat->contacts_view_->model();
        for (int row = 0; row < contacts_model->rowCount(); ++row)
        {
            contacts.append(contacts_model->index(row, 0).data(Qt::DisplayRole).toString());
        }
        out["names"] = contacts;
        out["group"] = chat->friend_groups_->currentText();
        out["requests"] = chat->incoming_friends_->count();
        if (cmd["close"].toBool())
        {
            dialog->reject();
        }
    }
    else if (op == "group")
    {
        auto id = cmd["id"].toInteger();
        QTimer::singleShot(0, chat, [chat, id] { emit chat->members_requested(id, chat->self_user(), "QA"); });
    }
    else if (op == "group_profile")
    {
        auto* d = window.findChild<QDialog*>("groupDialog");
        if (!d)
        {
            return out;
        }
        auto* search = d->findChild<QLineEdit*>("groupMemberSearch");
        if (cmd.contains("query"))
        {
            search->setText(cmd["query"].toString());
        }
        auto* list = d->findChild<QListWidget*>("groupMembersList");
        QJsonArray visible;
        for (int i = 0; i < list->count(); ++i)
        {
            if (!list->item(i)->isHidden())
            {
                visible.append(list->item(i)->text());
            }
        }
        out["members"] = visible;
        out["description"] = d->findChild<QLabel*>("groupDescription")->text();
        auto key = cmd["group"].toInteger();
        for (auto [name, id] : {std::pair{"groupImage", -key}, std::pair{"userImage", key}})
        {
            auto pix = chat->avatars().image(id);
            if (!pix.isNull())
            {
                out[name] = pix.toImage().pixelColor(pix.width() / 2, pix.height() / 2).name();
            }
        }
        auto* change = d->findChild<QPushButton*>("changeGroupAvatar");
        out["editable"] = change->isEnabled();
        if (cmd.contains("image") && change->isEnabled())
        {
            QTimer::singleShot(0,
                               d,
                               [d, change, path = cmd["image"].toString()]
                               {
                                   QTimer::singleShot(80,
                                                      d,
                                                      [path]
                                                      {
                                                          if (auto* f = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                                                          {
                                                              f->findChild<QLineEdit*>("fileNameEdit")->setText(path);
                                                              QMetaObject::invokeMethod(f, "accept");
                                                          }
                                                      });
                                   change->click();
                               });
        }
        if (cmd.contains("description") && change->isEnabled())
        {
            QTimer::singleShot(0,
                               d,
                               [d, text = cmd["description"].toString()]
                               {
                                   QTimer::singleShot(40,
                                                      d,
                                                      [text]
                                                      {
                                                          if (auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
                                                          {
                                                              input->setTextValue(text);
                                                              input->accept();
                                                          }
                                                      });
                                   d->findChild<QPushButton*>("editGroupDescription")->click();
                               });
        }
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
    else if (op == "queue_file")
    {
        chat->send_files(cmd["conv"].toInteger(), {cmd["path"].toString()});
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
                if (cmd["choose"].toBool() && list && list->topLevelItemCount())
                {
                    list->setCurrentItem(list->topLevelItem(0));
                    for (auto* button : dialog->findChildren<QPushButton*>())
                    {
                        if (button->text() == "下载")
                        {
                            QTimer::singleShot(0, button, &QPushButton::click);
                            break;
                        }
                    }
                }
                if (cmd.contains("remove_key"))
                {
                    window.client_->change_file(cmd["group"].toInteger(), cmd["remove_key"].toString(), false);
                }
                if (auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                {
                    out["choosing"] = true;
                    if (cmd.contains("accept_path"))
                    {
                        chooser->findChild<QLineEdit*>("fileNameEdit")->setText(cmd["accept_path"].toString());
                        QTimer::singleShot(0, chooser, [chooser] { QMetaObject::invokeMethod(chooser, "accept"); });
                    }
                }

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
    else if (op == "multi")
    {
        auto* bar = chat->selection_bar_;
        auto action_button = [bar](QString const& label) -> QPushButton*
        {
            for (auto* action : bar->findChildren<QPushButton*>())
            {
                if (action->text() == label)
                {
                    return action;
                }
            }
            return nullptr;
        };
        auto* copy = action_button(QStringLiteral("复制"));
        auto* forward = action_button(QStringLiteral("转发"));
        if (cmd.contains("seed"))
        {
            QApplication::clipboard()->setText(cmd["seed"].toString());
        }
        if (cmd.contains("cursor"))
        {
            auto cursor = edit->textCursor();
            cursor.setPosition(cmd["cursor"].toInt());
            edit->setTextCursor(cursor);
        }
        if (cmd["older"].toBool())
        {
            view->scrollToTop();
        }
        auto find = [model](QJsonValue const& target)
        {
            if (target.toString() == "pending")
            {
                return model->index(model->rowCount() - 1, 0);
            }
            for (int i = 0; i < model->rowCount(); ++i)
            {
                auto const ix = model->index(i, 0);
                if (ix.data(message_model::id_role).toLongLong() == target.toInteger())
                {
                    return ix;
                }
            }
            return QModelIndex{};
        };
        if (cmd.contains("start"))
        {
            auto const ix = find(cmd["start"]);
            view->scrollTo(ix);
            choose_menu_action(view, view->visualRect(ix).center(), QStringLiteral("多选"));
        }
        if (cmd.contains("click"))
        {
            auto const ix = find(cmd["click"]);
            if (ix.isValid())
            {
                view->scrollTo(ix);
                QPointF pos(view->visualRect(ix).center());
                auto const mod = cmd["modifier"].toString() == "shift"  ? Qt::ShiftModifier
                                 : cmd["modifier"].toString() == "ctrl" ? Qt::ControlModifier
                                                                        : Qt::NoModifier;
                QMouseEvent press(QEvent::MouseButtonPress, pos, view->viewport()->mapToGlobal(pos.toPoint()), Qt::LeftButton, Qt::LeftButton, mod);
                QApplication::sendEvent(view->viewport(), &press);
                QMouseEvent release(QEvent::MouseButtonRelease, pos, view->viewport()->mapToGlobal(pos.toPoint()), Qt::LeftButton, Qt::NoButton, mod);
                QApplication::sendEvent(view->viewport(), &release);
            }
        }
        if (cmd["button"].toBool())
        {
            QApplication::setActiveWindow(&window);
            copy->setFocus();
            copy->click();
            out["buttonFocused"] = copy->hasFocus();
        }
        if (cmd.contains("key"))
        {
            auto const escape = cmd["key"].toString() == "esc";
            auto* recipient = cmd["focused"].toBool() ? QApplication::focusWidget() : view;
            if (recipient)
            {
                QKeyEvent key(QEvent::KeyPress, escape ? Qt::Key_Escape : Qt::Key_C, escape ? Qt::NoModifier : Qt::ControlModifier);
                QApplication::sendEvent(recipient, &key);
            }
        }
        QJsonArray selected, valid;
        for (auto const& index : view->selectionModel()->selectedIndexes())
        {
            selected.append(index.data(message_model::id_role).toLongLong());
        }
        for (auto const& index : chat->selected_messages())
        {
            valid.append(index.data(message_model::id_role).toLongLong());
        }
        out["selected"] = selected;
        out["valid"] = valid;
        out["count"] = bar->findChild<QLabel*>("selectedMessageCount")->text();
        out["connected"] = chat->connection_available_;
        out["mode"] = view->selectionMode() == QAbstractItemView::ExtendedSelection;
        out["copy"] = copy->isEnabled();
        out["forward"] = forward->isEnabled();
        out["clipboard"] = QApplication::clipboard()->text();
        out["cursor"] = edit->textCursor().position();
        out["draft"] = edit->toPlainText();
        out["reply"] = chat->reply_to_;
        out["first"] = chat->messages_->first_message_id();
        out["pending"] = model->index(model->rowCount() - 1, 0).data(message_model::send_state_role).toInt();
        if (cmd.contains("inspect"))
        {
            auto const index = find(cmd["inspect"]);
            out["selectable"] = index.flags().testFlag(Qt::ItemIsSelectable);
            out["marked"] = view->selectionModel()->isSelected(index);
            out["deleted"] = index.data(message_model::deleted_role).toBool();
        }
    }
    else if (op == "forward_picker")
    {
        out["open"] = false;
        if (cmd["open"].toBool())
        {
            QTimer::singleShot(0, chat->selection_forward_, &QPushButton::click);
        }
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            dialog && dialog->isVisible() && dialog->windowTitle() == QStringLiteral("选择转发目标"))
        {
            out["open"] = true;
            if (cmd.contains("target"))
            {
                auto* list = dialog->findChild<QListWidget*>();
                for (int i = 0; i < list->count(); ++i)
                {
                    if (list->item(i)->data(Qt::UserRole).toLongLong() == cmd["target"].toInteger())
                    {
                        list->setCurrentRow(i);
                        auto* confirm = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
                        out["confirmed"] = confirm->isEnabled();
                        QTimer::singleShot(0, confirm, &QPushButton::click);
                        break;
                    }
                }
            }
            if (cmd["cancel"].toBool())
            {
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
            }
        }
        if (cmd.contains("available"))
        {
            auto* conversations = static_cast<conversation_model*>(conv->model());
            out["available"] = conversations->index_for_conversation(cmd["available"].toInteger()).isValid();
        }
        if (cmd.contains("fence"))
        {
            window.client_->set_typing(cmd["fence"].toInteger(), false);
        }
    }
    else if (op == "message_menu")
    {
        if (cmd.contains("open"))
        {
            QModelIndex index;
            for (int row = 0; row < model->rowCount(); ++row)
            {
                auto const candidate = model->index(row, 0);
                if (candidate.data(message_model::id_role).toLongLong() == cmd["open"].toInteger())
                {
                    index = candidate;
                    break;
                }
            }
            view->scrollTo(index);
            QTimer::singleShot(0, view, [view, pos = view->visualRect(index).center()] { emit view->customContextMenuRequested(pos); });
        }
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        {
            out["open"] = true;
            auto trigger = [menu](QString const& label)
            {
                for (auto* action : menu->actions())
                {
                    if (action->text() == label)
                    {
                        menu->setActiveAction(action);
                        QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                        QApplication::sendEvent(menu, &key);
                        break;
                    }
                }
            };
            if (cmd["forward"].toBool())
            {
                QTimer::singleShot(0, menu, [trigger] { trigger(QStringLiteral("转发")); });
            }
            else if (cmd["copy"].toBool())
            {
                trigger(QStringLiteral("复制"));
            }
        }
        out["clipboard"] = QApplication::clipboard()->text();
    }
    else if (op == "bookmark_menu")
    {
        for (int i = 0; i < model->rowCount(); ++i)
        {
            auto const index = model->index(i, 0);
            if (index.data(message_model::id_role).toLongLong() != cmd["message"].toInteger())
            {
                continue;
            }
            auto const label = cmd["action"].toString();
            view->scrollTo(index);
            QTimer watch;
            watch.setInterval(10);
            QObject::connect(&watch,
                             &QTimer::timeout,
                             view,
                             [&out, label]
                             {
                                 if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
                                 {
                                     for (auto* action : menu->actions())
                                     {
                                         if (action->text() == label && action->isEnabled())
                                         {
                                             out["action"] = label;
                                             menu->setActiveAction(action);
                                             QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                                             QApplication::sendEvent(menu, &key);
                                             return;
                                         }
                                     }
                                 }
                             });
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&deadline,
                             &QTimer::timeout,
                             view,
                             []
                             {
                                 if (auto* menu = QApplication::activePopupWidget())
                                 {
                                     menu->close();
                                 }
                             });
            deadline.start(3000);
            watch.start();
            emit view->customContextMenuRequested(view->visualRect(index).center());
            break;
        }
    }
    else if (op == "bookmarks")
    {
        if (cmd["open"].toBool())
        {
            for (auto* action : window.findChildren<QAction*>())
            {
                if (action->text() == "消息收藏")
                {
                    QTimer::singleShot(0, action, &QAction::trigger);
                }
            }
        }
        auto* dialog = window.findChild<QDialog*>("bookmarksDialog");
        if (dialog && dialog->isVisible())
        {
            auto* list = dialog->findChild<QTreeWidget*>("bookmarkResults");
            if (cmd.contains("query"))
            {
                dialog->findChild<QLineEdit*>("bookmarkQuery")->setText(cmd["query"].toString());
            }
            if (cmd.contains("select"))
            {
                for (int i = 0; i < list->topLevelItemCount(); ++i)
                {
                    auto* item = list->topLevelItem(i);
                    if (item->data(0, Qt::UserRole).toLongLong() == cmd["select"].toInteger())
                    {
                        list->setCurrentItem(item);
                        break;
                    }
                }
            }
            auto* more = button("moreBookmarks");
            auto* locate = button("locateBookmark");
            auto* remove = button("removeBookmark");
            if (cmd["more"].toBool())
            {
                more->click();
            }
            if (cmd["locate"].toBool())
            {
                QTimer::singleShot(0, locate, &QAbstractButton::click);
            }
            if (cmd["remove"].toBool())
            {
                remove->click();
            }
            QJsonArray rows;
            for (int i = 0; i < list->topLevelItemCount(); ++i)
            {
                auto* item = list->topLevelItem(i);
                rows.append(QJsonObject{{"id", item->data(0, Qt::UserRole).toLongLong()},
                                        {"conversation", item->data(0, Qt::UserRole + 1).toLongLong()},
                                        {"summary", item->text(4)}});
            }
            out["open"] = true;
            out["rows"] = rows;
            out["more"] = more->isVisible();
            out["locate"] = locate->isEnabled();
            out["selected"] = list->currentItem() ? list->currentItem()->data(0, Qt::UserRole).toLongLong() : 0;
            out["status"] = dialog->findChild<QLabel*>("bookmarkStatus")->text();
            if (cmd["close"].toBool())
            {
                dialog->reject();
            }
        }
    }
    else if (op == "searchui")
    {
        if (cmd["open"].toBool() || cmd["shortcut"].toBool())
        {
            for (auto* a : window.findChildren<QAction*>())
            {
                if (cmd["shortcut"].toBool() ? a->objectName() == "mentionsMeAction" : a->text() == "全局消息搜索")
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
                auto* mentions = d->findChild<QCheckBox*>("searchMentionsMe");
                if (cmd.contains("mentions"))
                {
                    mentions->setChecked(cmd["mentions"].toBool());
                }
                out["mentions"] = mentions->isChecked();
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
                out["anchor"] = ctx->currentItem() ? ctx->currentItem()->data(Qt::UserRole).toLongLong() : 0;
                out["selected"] = l->currentItem() ? l->currentItem()->data(1, Qt::UserRole).toLongLong() : 0;
                if (cmd["close"].toBool())
                {
                    d->reject();
                }
            }
        }
    }
    else if (op == "sidebar_action")
    {
        auto* cm = static_cast<conversation_model*>(conv->model());
        auto label = cmd["action"].toString();
        auto folder = cmd["folder"];
        auto index = cm->index_for_conversation(cmd["conversation"].toInteger());
        if (!folder.isUndefined())
        {
            for (int row = 0; row < cm->rowCount(); ++row)
            {
                auto ix = cm->index(row, 0);
                if (ix.data(conversation_model::folder_header_role).toBool() &&
                    ix.data(conversation_model::folder_role).toLongLong() == folder.toInteger())
                {
                    index = ix;
                    break;
                }
            }
        }
        if (index.isValid())
        {
            auto pos = conv->visualRect(index).center();
            auto global = conv->viewport()->mapToGlobal(pos);
            out["point"] = QJsonArray{global.x(), global.y()};
            out["membership"] = index.data(conversation_model::folder_role).toLongLong();
            if (!label.isEmpty())
            {
                choose_menu_action(conv, pos, label, cmd["destination"].toString());
            }
        }
        else if (!label.isEmpty())
        {
            auto* menu = window.findChild<QMenu*>("mainNavigationMenu");
            for (auto* action : menu->actions())
            {
                if (action->text() == label)
                {
                    action->trigger();
                    break;
                }
            }
        }
    }
    else if (op == "mention")
    {
        auto* menu = window.findChild<QToolButton*>("mentionMemberButton")->menu();
        menu->aboutToShow();
        QJsonArray labels;
        for (auto* item : menu->actions())
        {
            labels.append(item->text());
            if (cmd["choose"].toString() == item->text())
            {
                item->trigger();
            }
        }
        out["labels"] = labels;
    }
    else if (op == "toggle_folder")
    {
        auto* cm = static_cast<conversation_model*>(conv->model());
        for (int row = 0; row < cm->rowCount(); ++row)
        {
            auto index = cm->index(row, 0);
            if (index.data(conversation_model::folder_header_role).toBool() &&
                index.data(conversation_model::folder_role).toLongLong() == cmd["folder"].toInteger())
            {
                auto* delegate = static_cast<conversation_delegate*>(conv->itemDelegate());
                emit delegate->body_clicked(index);
                break;
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
    else if (op == "desktop_environment")
    {
        auto geometry = [](QWidget const* widget) -> QJsonArray
        {
            if (!widget || !widget->isVisible())
            {
                return {};
            }
            auto const point = widget->mapToGlobal(QPoint{});
            return {point.x(), point.y(), widget->width(), widget->height()};
        };
        out["platform"] = QGuiApplication::platformName();
        out["dpr"] = window.devicePixelRatioF();
        out["qt"] = QString::fromLatin1(qVersion());
        out["im"] = qEnvironmentVariable("QT_IM_MODULE");
        out["preedit"] = edit->textCursor().block().layout()->preeditAreaText();
        out["window"] = geometry(&window);
        out["windowId"] = static_cast<qint64>(window.winId());
        out["minimum"] = QJsonArray{window.minimumWidth(), window.minimumHeight()};
        out["minimized"] = window.isMinimized();
        if (auto* modal = QApplication::activeModalWidget())
        {
            out["modalId"] = static_cast<qint64>(modal->winId());
            out["modalType"] = modal->objectName();
            out["modalTitle"] = modal->windowTitle();
            out["saveAttachment"] = geometry(modal->findChild<QWidget*>("saveAttachmentButton"));
            out["editMessage"] = geometry(modal->findChild<QWidget*>("editMessageText"));
            out["saveEdit"] = geometry(modal->findChild<QWidget*>("editMessageButton"));
            out["screenshotPreview"] = geometry(modal->findChild<QWidget*>("screenshotPreview"));
            if (auto* preview = modal->findChild<QLabel*>("screenshotPreview"); preview && !preview->pixmap().isNull())
            {
                out["previewSize"] = QJsonArray{preview->pixmap().width(), preview->pixmap().height()};
            }
            for (auto* button : modal->findChildren<QPushButton*>())
            {
                if (button->text() == "发送截图")
                {
                    out["confirmCapture"] = geometry(button);
                }
            }
        }
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        {
            QJsonObject actions;
            for (auto* action : menu->actions())
            {
                auto const rect = menu->actionGeometry(action);
                auto const point = menu->mapToGlobal(rect.topLeft());
                actions[action->text()] = QJsonArray{point.x(), point.y(), rect.width(), rect.height()};
            }
            out["actions"] = actions;
        }
        if (auto* popup = QApplication::activePopupWidget(); popup && popup->objectName() == "emojiPicker")
        {
            out["emojiFirst"] = geometry(popup->findChild<QWidget*>("emojiChoice"));
        }
        out["focus"] = QApplication::focusWidget() ? QApplication::focusWidget()->objectName() : QString{};
        out["colorScheme"] = static_cast<int>(QGuiApplication::styleHints()->colorScheme());
        out["themeCanvas"] = theme_manager::instance().color(QStringLiteral("canvas")).name();
        for (auto const* name : {"entryUnreadButton",
                                 "returnToLatestButton",
                                 "chatHeaderButton",
                                 "mainMenuButton",
                                 "windowMinimizeButton",
                                 "windowResizeGrip",
                                 "screenshotButton",
                                 "sendAttachmentButton",
                                 "emojiButton"})
        {
            out[name] = geometry(window.findChild<QWidget*>(name));
        }
        out["viewport"] = geometry(view->viewport());
        auto const* scrollbar = view->verticalScrollBar();
        out["scrollbar"] = geometry(scrollbar);
        if (scrollbar->isVisible())
        {
            QStyleOptionSlider option;
            scrollbar->initStyleOption(&option);
            auto const rect = scrollbar->style()->subControlRect(QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, scrollbar);
            auto const point = scrollbar->mapToGlobal(rect.topLeft());
            out["thumb"] = QJsonArray{point.x(), point.y(), rect.width(), rect.height()};
        }
        out["editor"] = geometry(edit);
        if (auto const index = view->indexAt(view->viewport()->rect().center()); index.isValid())
        {
            auto const rect = view->visualRect(index).intersected(view->viewport()->rect());
            auto const point = view->viewport()->mapToGlobal(rect.topLeft());
            out["message"] = QJsonArray{point.x(), point.y(), rect.width(), rect.height()};
            out["messageId"] = index.data(message_model::id_role).toLongLong();
        }
        for (int i = model->rowCount() - 1; i >= 0; --i)
        {
            auto const index = model->index(i, 0);
            if (index.data(message_model::id_role).toLongLong() > 0)
            {
                auto const rect = view->visualRect(index).intersected(view->viewport()->rect());
                if (!rect.isEmpty())
                {
                    auto const point = view->viewport()->mapToGlobal(rect.topLeft());
                    out["tailMessage"] = QJsonArray{point.x(), point.y(), rect.width(), rect.height()};
                }
                break;
            }
        }
        if (auto index = static_cast<conversation_model*>(conv->model())->index_for_conversation(cmd["conversation"].toInteger()); index.isValid())
        {
            auto const rect = conv->visualRect(index);
            auto const point = conv->viewport()->mapToGlobal(rect.topLeft());
            out["conversation"] = QJsonArray{point.x(), point.y(), rect.width(), rect.height()};
        }
    }
    else if (op == "reading")
    {
        auto* unread = button("entryUnreadButton");
        auto* latest = button("returnToLatestButton");
        auto* list = window.findChild<QListView*>("messageList");
        if (cmd["activate"].toBool())
        {
            window.activateWindow();
        }
        if (cmd["up"].toBool())
        {
            auto* scroll = list->verticalScrollBar();
            scroll->setValue(scroll->maximum() / 2);
        }
        if (cmd["older"].toBool())
        {
            list->verticalScrollBar()->setValue(0);
        }
        if (cmd["bottom"].toBool())
        {
            list->scrollToBottom();
        }
        if (cmd["latest"].toBool() && latest->isVisible())
        {
            latest->click();
        }
        if (cmd["unread"].toBool() && unread->isVisible())
        {
            QTimer::singleShot(0, unread, &QPushButton::click);
        }
        out["unread"] = unread->isVisible();
        out["latest"] = latest->isVisible();
        out["ready"] = chat->messages_ready();
        out["viewingLatest"] = chat->viewing_latest();
        out["active"] = window.isActiveWindow();
        out["popup"] = QApplication::activePopupWidget() != nullptr;
        out["scroll"] = list->verticalScrollBar()->value();
        out["maximum"] = list->verticalScrollBar()->maximum();
        out["last"] = chat->latest_message_id();
        out["first"] = chat->messages_->first_message_id();
        out["read"] = chat->messages_->read_position(chat->self_user());
        out["status"] = chat->message_status_->text();
    }
    else if (op == "state")
    {
        out["user"] = chat->self_user();
        out["conv"] = chat->active_conversation();
        out["enabled"] = edit->isEnabled();
        out["sendEnabled"] = button("sendButton")->isEnabled();
        auto const* notice = window.findChild<QLabel*>("groupSpeechNotice");
        out["speech"] = notice->isVisible() ? notice->text() : QString{};
        out["notice"] = window.findChild<QLabel*>("sidebarNotice")->text();
        out["draft"] = edit->toPlainText();
        out["reply"] = button("cancelReplyButton")->isVisible();
        out["preview"] = window.findChild<QLabel*>("replyPreview")->text();
        QJsonArray rows;
        for (int i = 0; i < model->rowCount(); ++i)
        {
            auto idx = model->index(i, 0);
            QJsonObject row{{"text", idx.data(message_model::text_role).toString()}, {"id", idx.data(message_model::id_role).toLongLong()}};
            if (cmd["authors"].toBool())
            {
                row["author"] = idx.data(message_model::sender_name_role).toString();
                row["forward"] = idx.data(message_model::forwarded_from_role).toString();
                row["quote"] = idx.data(message_model::reply_text_role).toString();
            }
            rows.append(row);
        }
        out["rows"] = rows;
    }
    return out;
}
}    // namespace

int main(int argc, char** argv)
{
    if (!qEnvironmentVariableIsSet("CHAT_TEST_NATIVE_DESKTOP"))
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }
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
