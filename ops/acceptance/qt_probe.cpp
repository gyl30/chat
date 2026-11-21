#include <iostream>
#include <QListWidget>
#include <QScrollBar>
#include <QApplication>
#include <QLabel>
#include <QAbstractButton>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSocketNotifier>
#include <QTimer>
#include <QTreeWidget>
#include <QMessageBox>
#include <QPushButton>
#include "main_window.hpp"
#include "chat_widget.hpp"
#include "conversation_model.hpp"
#include "message_model.hpp"
#include "theme_manager.hpp"

namespace
{
QJsonObject dispatch(main_window& window, QJsonObject const& cmd)
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
        window.findChild<QLineEdit*>("loginPasswordEdit")->setText("pass");
        button("loginButton")->click();
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
        edit->setPlainText(cmd["text"].toString());
        out["enabled"] = button("sendButton")->isEnabled();
        button("sendButton")->click();
    }
    else if (op == "group")
    {
        auto id = cmd["id"].toInteger();
        QTimer::singleShot(0, chat, [chat, id] { emit chat->members_requested(id, chat->self_user(), "QA"); });
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
        if (box && box->isVisible())
        {
            box->button(QMessageBox::Yes)->click();
        }
    }
    else if (op == "preview")
    {
        auto id = cmd["id"].toInteger(), c = cmd["conv"].toInteger();
        QTimer::singleShot(0, chat, [=] { emit chat->attachment_open_requested(c, id, "inspect.png", true); });
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
            if (cmd["close"].toBool())
            {
                d->reject();
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
            auto* q = d->findChild<QLineEdit*>();
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
                if (cmd.contains("query"))
                {
                    q->setText(cmd["query"].toString());
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
    else if (op == "draft")
    {
        edit->setPlainText(cmd["text"].toString());
    }
    else if (op == "state")
    {
        out["user"] = chat->self_user();
        out["conv"] = chat->active_conversation();
        out["enabled"] = edit->isEnabled();
        out["draft"] = edit->toPlainText();
        out["reply"] = button("cancelReplyButton")->isVisible();
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
                         auto out = dispatch(window, cmd);
                         std::cout << QJsonDocument(out).toJson(QJsonDocument::Compact).constData() << std::endl;
                     });
    return app.exec();
}
