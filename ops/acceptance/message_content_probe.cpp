// Exercise the actual bubble delegate and context menu without opening an external browser.
#include "chat_widget.hpp"
#include "conversation_model.hpp"
#include "client_bridge.hpp"
#include "download_tasks.hpp"
#include "message_delegate.hpp"
#include "message_card.hpp"
#include <QPainter>
#include "message_links.hpp"
#include "message_markdown.hpp"
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include "message_model.hpp"
#include "message_search_dialog.hpp"
#include "theme_manager.hpp"
#include "theme.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QTextBrowser>
#include <QTimer>
#include <cassert>
#include <iostream>

static void check_sidebar_status(chat_widget& chat, conversation_data item)
{
    item.unread = 120;
    item.pinned = true;
    item.unread_mention = true;
    item.notifications = chat::notification_level::muted;
    item.mute_until = QDateTime::currentMSecsSinceEpoch() + 3600000;
    chat.set_conversations({item});
    auto* list = chat.findChild<QListView*>("conversationList");
    auto index = list->model()->index(0, 0);
    auto description = index.data(Qt::AccessibleTextRole).toString();
    for (QString label : {"已置顶", "免打扰", "有人提及你", "120 条未读消息"})
    {
        assert(description.contains(label));
        assert(index.data(Qt::ToolTipRole).toString().contains(label));
    }
    QTimer::singleShot(0,
                       list,
                       [&]
                       {
                           auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                           assert(menu);
                           auto actions = menu->actions();
                           auto notify = std::ranges::find_if(actions, [](auto* action) { return action->text() == "通知"; });
                           assert(notify != actions.end());
                           QMenu* levels = (*notify)->menu();
                           levels->aboutToShow();
                           assert(levels->actions().last()->isChecked() && levels->actions().last()->text().startsWith("免打扰至"));
                           chat.set_notifications(item.id, chat::notification_level::muted, QDateTime::currentMSecsSinceEpoch() - 1);
                           assert(levels->actions().first()->isChecked() && !levels->actions().last()->isVisible());
                           assert(!index.data(conversation_model::muted_role).toBool());
                           menu->close();
                       });
    emit list->customContextMenuRequested(list->visualRect(index).center());
    item.unread = 0;
    item.unread_mention = false;
    item.marked_unread = true;
    chat.set_conversations({item});
    description = list->model()->index(0, 0).data(Qt::AccessibleTextRole).toString();
    assert(description.contains("已标记未读") && !description.contains("条未读消息"));
}

static void check_search_menu_copy()
{
    message_search_dialog dialog(7, 1, true, "群聊", {}, nullptr);
    message_data message;
    message.id = 12;
    message.conversation = 7;
    message.text = "original";
    dialog.set_results(7, {}, 0, {message}, {}, false, {});
    dialog.show();
    QApplication::processEvents();
    auto* view = dialog.findChild<QListView*>();
    for (bool withdrawn : {false, true})
    {
        QApplication::clipboard()->setText("untouched");
        QTimer::singleShot(0,
                           &dialog,
                           [&]
                           {
                               auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                               assert(menu);
                               message.text = "edited";
                               message.edited_at = 1;
                               message.deleted = withdrawn;
                               dialog.update_message(7, message, {});
                               menu->setActiveAction(menu->actions().first());
                               QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                               QApplication::sendEvent(menu, &enter);
                           });
        emit view->customContextMenuRequested(view->visualRect(view->model()->index(0, 0)).center());
        assert(QApplication::clipboard()->text() == (withdrawn ? "untouched" : "edited"));
    }
}

static void check_history_merge_notifications()
{
    auto make_message = [](qint64 id)
    {
        message_data message;
        message.id = id;
        message.conversation = 7;
        message.from = 2;
        message.text = QString::number(id);
        return message;
    };
    message_model model;
    model.reset(7);
    QList<message_data> recent;
    for (int id = 101; id <= 340; ++id)
    {
        recent.push_back(make_message(id));
    }
    model.merge_messages(std::move(recent));
    QPersistentModelIndex selected(model.message_index(150));
    int insertions = 0, inserted = 0, changed = 0;
    QObject::connect(&model,
                     &QAbstractItemModel::rowsInserted,
                     [&insertions, &inserted](auto const&, int first, int last)
                     {
                         ++insertions;
                         inserted += last - first + 1;
                     });
    QObject::connect(
        &model, &QAbstractItemModel::dataChanged, [&changed](auto const& first, auto const& last) { changed += last.row() - first.row() + 1; });
    QList<message_data> previous;
    for (int id = 61; id <= 120; ++id)
    {
        previous.push_back(make_message(id));
    }
    model.merge_messages(std::move(previous));
    std::cout << "History: 240 loaded + 60 page, insert notifications=" << insertions << ", rows inserted=" << inserted
              << ", notified existing rows=" << changed << std::endl;
    assert(insertions == 1 && inserted == 40 && changed <= 2);
    assert(selected.isValid() && selected.row() == 89 && selected.data(message_model::id_role).toLongLong() == 150);

    auto reply = make_message(400);
    reply.reply = {150, "peer", "150", 0, false, 2};
    assert(model.add_message(reply));
    auto edit = make_message(150);
    edit.edited_at = 20;
    edit.text = "new quote";
    model.begin_page();
    model.update_message(edit);
    changed = 0;
    auto stale = make_message(150);
    model.merge_messages({stale, reply});
    assert(changed == 0 && selected.data(message_model::text_role) == "new quote");
    assert(model.message_index(400).data(message_model::reply_text_role).toString().endsWith("new quote"));
}

static void check_download_notification_scope()
{
    client_bridge client;
    auto& tasks = client.downloads();
    message_model model;
    model.reset(9);
    model.set_downloads(tasks);
    for (int id = 1; id <= 3; ++id)
    {
        message_data message;
        message.id = id;
        message.conversation = 9;
        message.attachment = attachment_data{QStringLiteral("file.txt"), QStringLiteral("application/octet-stream"), 1024, QString{}};
        assert(model.add_message(std::move(message)));
    }
    download_tasks::task running{};
    running.source = {9, 2, 0, {}};
    running.path = "same.txt";
    auto completed = running;
    completed.source.message = 1;
    completed.status = download_tasks::state::completed;
    tasks.tasks_.insert(1, completed);
    tasks.tasks_.insert(2, running);
    QList<qint64> notified;
    QObject::connect(&model,
                     &QAbstractItemModel::dataChanged,
                     [&notified](auto const& from, auto const&) { notified.push_back(from.data(message_model::id_role).toLongLong()); });

    tasks.progress(2, 0, 10'000'000, 900'000'000);
    tasks.progress(2, 0, 10'000'001, 900'000'000);
    std::cout << "Downloads: 3 file rows, notifications for two equivalent samples=" << notified.size() << std::endl;
    assert(notified == QList<qint64>{2});
    notified.clear();
    tasks.finish(2, 0, {});
    std::ranges::sort(notified);
    assert((notified == QList<qint64>{1, 2}));
    assert(tasks.tasks_.value(1).status == download_tasks::state::replaced);
    // A bundled child or a group file never causes a message bubble to relayout.
    running.source.item = 1;
    tasks.tasks_.insert(3, running);
    running.source = {9, 0, 0, QStringLiteral("group-file")};
    tasks.tasks_.insert(4, running);
    notified.clear();
    for (qint64 id : {3, 4})
    {
        tasks.progress(id, 0, 100, 1000);
    }
    assert(notified.isEmpty());
}

static void check_display_roles()
{
    message_model model;
    model.set_self_user(1);
    model.reset(1);
    message_data m;
    m.conversation = 1;
    auto check = [&](QString text)
    {
        ++m.id;
        m.text = text;
        assert(model.add_message(m));
        auto index = model.index(model.rowCount() - 1);
        auto body = index.data(message_model::text_role);
        assert(body.isValid() && index.data(Qt::DisplayRole) == body);
        assert(index.data(message_model::poll_role).isValid() == (m.poll.has_value() && !m.deleted));
        return body.toString();
    };
    assert(check("普通文本") == "普通文本");
    model.set_members({member_data{.id = 2, .username = "bob", .avatar = {}, .nickname = "小波"}});
    assert(model.set_reactions(m.id, 1, {reaction_data{QStringLiteral("👍"), {1, 2, 99}}}));
    assert(model.message_index(m.id).data(message_model::reaction_names_role).toStringList() == QStringList{QStringLiteral("我、小波、用户 99")});
    m.markdown = true;
    assert(check("**源文**") == "**源文**");
    m.markdown = false;
    m.attachment = attachment_data{"photo.png", "image/png", 1024, "image"};
    assert(check("photo.png") == "图片：photo.png\n1.0 KiB");
    m.attachment = attachment_data{"file.bin", "application/octet-stream", 2048, "file"};
    assert(check("file.bin") == "文件：file.bin\n2.0 KiB");
    m.attachment.reset();
    m.content = "bundle";
    assert(check("聊天记录") == "聊天记录");
    m.content = "poll";
    m.poll = chat::poll_summary{1, false, {{1, 0, "选项"}}};
    assert(check("问题") == "问题");
    m.poll->multiple = true;
    m.poll->selection = 1;
    m.poll->revision = 2;
    model.set_poll(m.id, *m.poll);
    auto retracted = *m.poll;
    retracted.selection = 0;
    retracted.multiple = true;
    retracted.revision++;
    model.set_poll(m.id, retracted);
    model.set_poll_draft(m.id, 1);
    model.merge_messages({m});
    assert(model.message_index(m.id).data(message_model::poll_draft_role).toLongLong() == 1);

    m.deleted = true;
    assert(check("问题") == "消息已撤回");
    m = {};
    m.client_id = 42;
    m.text = "本地正文";
    for (bool failed : {false, true})
    {
        model.set_pending({{m, failed, {}, !failed}});
        auto index = model.index(model.rowCount() - 1);
        assert(index.data(Qt::DisplayRole).toString() == m.text && index.data(message_model::text_role).toString() == m.text);
        assert(!index.data(message_model::poll_role).isValid());
    }
    for (bool group : {false, true})
    {
        model.reset(1, group);
        model.set_members({member_data{.id = 2, .username = "bob", .avatar = {}}, member_data{.id = 3, .username = "carol", .avatar = {}}});
        message_data outgoing_message;
        outgoing_message.id = 1;
        outgoing_message.conversation = 1;
        outgoing_message.from = 1;
        outgoing_message.username = QStringLiteral("alice");
        outgoing_message.timestamp = 1736942400000;
        outgoing_message.text = QStringLiteral("正文末行较短");
        assert(model.add_message(outgoing_message));
        auto incoming_message = outgoing_message;
        incoming_message.id = 2;
        incoming_message.from = 2;
        incoming_message.username = QStringLiteral("bob");
        incoming_message.text = QStringLiteral("对方的消息");
        assert(model.add_message(incoming_message));
        message_delegate delegate;
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 640, 400);
        const auto outgoing = model.index(0);
        const auto incoming = model.index(1);
        const auto original_size = delegate.sizeHint(option, outgoing);
        assert(!outgoing.data(message_model::read_role).toBool());
        assert(!incoming.data(message_model::read_role).isValid());
        model.set_read_message(2, 1);
        assert(outgoing.data(message_model::read_role).toBool());
        assert(delegate.sizeHint(option, outgoing) == original_size);
        assert(!outgoing.data(Qt::AccessibleTextRole).toString().contains(QStringLiteral("已读 1 人")));
        assert(outgoing.data(message_model::read_members_role).toBool() == group);
        if (group)
        {
            assert(model.read_members(1).size() == 1 && model.read_members(1).front().id == 2);
        }
    }
    m.from = 1;
    m.conversation = 1;
    for (bool group : {false, true})
    {
        model.reset(1, group);
        for (auto state : {message_model::waiting, message_model::sending, message_model::failed})
        {
            for (bool attachment : {false, true})
            {
                m.attachment = attachment ? std::optional{attachment_data{"photo.png", "image/png", 1024, "image"}} : std::nullopt;
                model.set_pending({{m, state == message_model::failed, {}, state == message_model::waiting}});
                auto local = model.index(0);
                assert(local.data(message_model::send_state_role).toInt() == state);
                assert(!local.data(message_model::read_role).isValid() && !local.data(message_model::read_members_role).isValid());
            }
        }
        model.set_pending({});
        m.id = 100;
        assert(model.add_message(m));
        auto sent = model.index(0);
        assert(sent.data(message_model::send_state_role).toInt() == message_model::sent);
        assert(sent.data(message_model::read_role).isValid() && !sent.data(message_model::read_role).toBool());
        model.set_read_message(2, m.id);
        if (group)
        {
            assert(!sent.data(message_model::read_role).toBool());
            model.set_members({member_data{.id = 2, .username = "bob", .avatar = {}}});
        }
        assert(sent.data(message_model::read_role).toBool());
    }
}

// Check both hit targets against the native text layout, including at fractional display scales.
static void check_code_cards(QFont const& font)
{
    QString const source = QStringLiteral(
                               "    indented code\n\n~~~cpp\nfirst\n- ~~~\n    ~~~\n~~~\n~~~cpp\n  second 中文\n\nlast\n~~~\n"
                               "> ```sh\n> echo foo\n> ```\n\n- list\n  ```\n  nested\n  ```\n\n"
                               "````text\n```\n%1\n````\n[伪造入口](chat-copy:1)")
                               .arg(QString(180, u'x'));
    message_markdown doc(source, font, 240, {});
    QStringList const expected{
        "indented code", "first\n- ~~~\n    ~~~", "  second 中文\n\nlast", "echo foo", "nested", QStringLiteral("```\n") + QString(180, u'x')};
    QStringList const headers{"", "cpp", "cpp", "sh", "", "text"};
    int number = 0;
    for (auto b = doc.begin(); b.isValid(); b = b.next())
    {
        for (auto it = b.begin(); !it.atEnd(); ++it)
        {
            auto f = it.fragment();
            if (!f.charFormat().isAnchor())
            {
                continue;
            }
            assert(number < expected.size() && f.text() == QStringLiteral("复制代码"));
            assert(b.text().chopped(f.length()).trimmed() == headers[number]);
            auto rect = doc.documentLayout()->blockBoundingRect(b);
            auto line = b.layout()->lineForTextPosition(f.position() - b.position());
            QPointF point = rect.topLeft() + QPointF(line.cursorToX(f.position() - b.position() + 1), line.y() + line.height() / 2);
            QUrl link(doc.documentLayout()->anchorAt(point));
            assert(doc.code_for_link(link) == expected[number]);
            auto code = doc.documentLayout()->blockBoundingRect(b.next());
            assert(doc.code_at(code.topLeft() + QPointF(3, 5)) == expected[number]);
            ++number;
        }
    }
    assert(number == expected.size() && doc.size().width() <= 240);
}

static void check_message_avatars(QListView& view, message_delegate& delegate)
{
    int clicks = 0;
    auto connection = QObject::connect(&delegate, &message_delegate::avatar_clicked, [&] { ++clicks; });
    view.setSelectionMode(QAbstractItemView::NoSelection);
    for (bool group : {false, true})
    {
        message_model model;
        model.set_self_user(1);
        model.reset(7, group);
        for (int i = 0; i < 5; ++i)
        {
            message_data item;
            item.id = i + 1;
            item.conversation = 7;
            item.from = i < 2 || i == 4 ? 1 : 2;
            item.timestamp = 1700000000000LL + i;
            item.username = item.from == 1 ? QStringLiteral("alice") : QStringLiteral("bob");
            item.text = QStringLiteral("连续消息");
            item.deleted = i == 4;
            assert(model.add_message(item));
        }
        for (int i = 0; i < model.rowCount(); ++i)
        {
            const auto index = model.index(i, 0);
            QStyleOptionViewItem option;
            option.initFrom(&view);
            option.widget = &view;
            option.rect = QRect(0, 0, 420, 1);
            option.rect.setHeight(delegate.sizeHint(option, index).height());
            const int half = chat_theme::message_avatar_size / 2;
            const int left = chat_theme::message_side_margin + half;
            const QPoint point(index.data(message_model::outgoing_role).toBool() ? option.rect.right() - left : left,
                               option.rect.bottom() - chat_theme::message_margin_bottom - half);
            const bool visible = i == 1 || i == 3;
            const auto before = clicks;
            QMouseEvent release(QEvent::MouseButtonRelease, point, view.viewport()->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            assert(delegate.editorEvent(&release, &model, option, index) == visible && clicks == before + visible);

            QImage image(option.rect.size(), QImage::Format_RGB32);
            image.fill(themed("canvas"));
            QPainter painter(&image);
            delegate.paint(&painter, option, index);
            painter.end();
            assert((image.pixelColor(point) != themed("canvas")) == visible);
        }
    }
    QObject::disconnect(connection);
}

static void check_card_actions(QListView& view, message_delegate& delegate)
{
    message_model model;
    model.set_self_user(1);
    model.reset(7, true);
    message_data item;
    item.conversation = 7;
    item.from = 2;
    for (auto kind : {QStringLiteral("bundle"), QStringLiteral("poll")})
    {
        item.id++;
        item.content = kind;
        item.text = kind == QStringLiteral("poll") ? QStringLiteral("测试问题") : QStringLiteral("聊天记录\nA：正文\n共 2 条聊天记录");
        if (kind == QStringLiteral("poll"))
        {
            item.poll = chat::poll_summary{1, true, {{1, 1, "A"}, {2, 0, "B"}}};
            item.poll->anonymous = false;
            item.poll->participants = 1;
        }
        assert(model.add_message(item));
        auto index = model.index(model.rowCount() - 1);
        QStyleOptionViewItem option;
        option.initFrom(&view);
        option.widget = &view;
        option.rect = QRect(0, 0, 620, 1);
        option.rect.setHeight(delegate.sizeHint(option, index).height());
        QImage image(option.rect.size(), QImage::Format_RGB32);
        image.fill(themed("canvas"));
        QPainter painter(&image);
        delegate.paint(&painter, option, index);
        painter.end();
        QPoint link(-1, -1);
        for (int y = image.height() - 1; y >= 0 && link.x() < 0; --y)
        {
            for (int x = image.width() - 1; x >= 0; --x)
            {
                if (image.pixelColor(x, y) == themed("link"))
                {
                    link = {x, y};
                    break;
                }
            }
        }
        assert(link.x() >= 0);
        QMouseEvent release(QEvent::MouseButtonRelease, link, view.viewport()->mapToGlobal(link), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        int opened = 0;
        auto const signal = QObject::connect(&delegate,
                                             &message_delegate::content_clicked,
                                             [&](QModelIndex const& clicked)
                                             {
                                                 assert(clicked == index);
                                                 ++opened;
                                             });
        view.setSelectionMode(QAbstractItemView::ExtendedSelection);
        assert(!delegate.editorEvent(&release, &model, option, index) && opened == 0);
        view.setSelectionMode(QAbstractItemView::NoSelection);
        assert(delegate.editorEvent(&release, &model, option, index) && opened == 1);
        QObject::disconnect(signal);
    }
}

static void check_editor_reflow(chat_widget& chat)
{
    auto* edit = chat.findChild<QPlainTextEdit*>("messageEdit");
    edit->setPlainText(QStringLiteral("第一行\n第二行\n第三行"));
    edit->setFocus();
    auto cursor = edit->textCursor();
    cursor.setPosition(0);
    cursor.setPosition(3, QTextCursor::KeepAnchor);
    edit->setTextCursor(cursor);
    for (int width : {1280, 980, 1080, 1280})
    {
        chat.resize(width, 640);
        QApplication::processEvents();
        assert(edit->width() >= 240 && edit->hasFocus() && edit->textCursor().selectedText() == QStringLiteral("第一行"));
    }
    cursor.clearSelection();
    edit->setTextCursor(cursor);
    QInputMethodEvent preedit(QStringLiteral("pinyin"), {});
    QApplication::sendEvent(edit, &preedit);
    chat.resize(980, 640);
    QApplication::processEvents();
    assert(edit->textCursor().block().layout()->preeditAreaText() == QStringLiteral("pinyin") && edit->hasFocus());
    int sends = 0;
    auto connection = QObject::connect(&chat, &chat_widget::send_message_requested, [&] { ++sends; });
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    auto before = edit->toPlainText();
    QApplication::sendEvent(edit, &enter);
    assert(sends == 0 && edit->toPlainText() == before);
    QObject::disconnect(connection);
    QInputMethodEvent cancel;
    QApplication::sendEvent(edit, &cancel);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    check_display_roles();
    check_history_merge_notifications();
    check_download_notification_scope();
    check_search_menu_copy();
    QCoreApplication::setOrganizationName("acceptance");
    QCoreApplication::setApplicationName("message-content");
    theme_manager::instance().load_settings();
    chat_widget chat;
    chat.resize(1040, 700);
    chat.set_user("reader", 1);
    chat.set_connection_available(true);
    conversation_data conversation;
    conversation.id = 12;
    conversation.username = "target";
    chat.set_conversations({conversation});
    chat.open_conversation(conversation);
    message_data item;
    item.id = 45;
    item.conversation = 12;
    item.timestamp = 1600000000;
    item.from = 2;
    item.username = "target";
    QString const long_url = "https://example.test/" + QString(95, 'a');
    item.text = QStringLiteral("看http://例子.测试/猫。\n") + long_url + QStringLiteral("，还有 file:///tmp/f javascript:void(0) data:text/plain,x");
    chat.set_messages(12, {item}, {}, history_page::latest, false, false, {});
    chat.show();
    app.processEvents();
    chat.activateWindow();
    app.processEvents();
    check_editor_reflow(chat);
    int global = 0, local = 0;
    chat.findChild<QLineEdit*>("conversationSearchEdit")->setText("needle");
    QObject::connect(&chat,
                     &chat_widget::global_search_requested,
                     [&](QString const& query)
                     {
                         assert(query == "needle");
                         ++global;
                     });
    QObject::connect(&chat, &chat_widget::message_search_requested, [&] { ++local; });
    auto* editor = chat.findChild<QPlainTextEdit*>("messageEdit");
    for (auto modifiers : {Qt::KeyboardModifiers(Qt::ControlModifier), Qt::ControlModifier | Qt::ShiftModifier})
    {
        QKeyEvent key(QEvent::KeyPress, Qt::Key_F, modifiers);
        QApplication::sendEvent(editor, &key);
    }
    assert(local == 1 && global == 1);
    chat.findChild<QLineEdit*>("conversationSearchEdit")->clear();
    chat.set_control_enter_send(true);
    assert(editor->toolTip().contains("Ctrl+Enter") && editor->toolTip() == editor->accessibleDescription());
    chat.set_control_enter_send(false);
    auto* view = chat.findChild<QListView*>("messageList");
    auto* delegate = qobject_cast<message_delegate*>(view->itemDelegate());
    auto index = view->model()->index(0, 0);
    assert(index.data(message_model::text_role).toString() == item.text);
    QTextBrowser receiver;
    QUrl opened;
    QObject::connect(&receiver, &QTextBrowser::sourceChanged, [&](QUrl const& url) { opened = url; });
    QDesktopServices::setUrlHandler("http", &receiver, "sourceChanged");
    QDesktopServices::setUrlHandler("https", &receiver, "sourceChanged");
    for (auto theme : {chat_appearance::light, chat_appearance::dark})
    {
        theme_manager::instance().set_appearance(theme);
        auto ranges = message_links(item.text);
        assert(ranges.size() == 2 && ranges.front().format.foreground().color() == themed("link"));
        assert(message_links("xhttps://bad http:// https:///missing file:///x").empty());
        assert(message_links("https://example.test/a(b)).").front().format.anchorHref() == "https://example.test/a(b)");
        auto punctuated = message_links(QStringLiteral("https://例子.测试/路径：补充说明；https://example.test/end…"));
        assert(punctuated.size() == 2 && QUrl(punctuated.front().format.anchorHref()).path() == QStringLiteral("/路径") &&
               QUrl(punctuated.back().format.anchorHref()).path() == QStringLiteral("/end"));
        QStyleOptionViewItem option;
        option.initFrom(view);
        option.font = view->font();
        option.widget = view;
        option.rect = view->visualRect(index);
        QPoint target;
        QSet<int> lines;
        for (int y = option.rect.top(); y <= option.rect.bottom(); y += 3)
        {
            for (int x = option.rect.left(); x <= option.rect.right(); x += 11)
            {
                auto url = message_delegate::link_at(option, index, {x, y});
                if (url == QUrl(long_url))
                {
                    lines.insert(y);
                    target = {x, y};
                }
            }
        }
        assert(!lines.empty() &&
               (*std::max_element(lines.begin(), lines.end()) - *std::min_element(lines.begin(), lines.end())) > view->fontMetrics().height());
        QMouseEvent release(QEvent::MouseButtonRelease, target, view->viewport()->mapToGlobal(target), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        opened.clear();
        assert(delegate->editorEvent(&release, view->model(), option, index) && opened == QUrl(long_url));
        chat.set_message_selection(true);
        opened.clear();
        assert(!delegate->editorEvent(&release, view->model(), option, index) && opened.isEmpty());
        chat.set_message_selection(false);
        QTimer::singleShot(0,
                           []
                           {
                               auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                               assert(menu);
                               for (auto* action : menu->actions())
                               {
                                   if (action->text() == QStringLiteral("复制链接"))
                                   {
                                       menu->setActiveAction(action);
                                       QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                                       QApplication::sendEvent(menu, &enter);
                                       return;
                                   }
                               }
                               assert(false);
                           });
        chat.show_message_menu(target);
        assert(QApplication::clipboard()->text() == long_url);
    }
    auto const md = QStringLiteral(
        "# 题目\n\n**粗体** *斜体* ~~删除~~ 😀\n\n[网页](https://example.test) [本地](file:///tmp/test)\n\n![远程](https://example.test/img.png) "
        "![本地图片](file:///tmp/image.png)\n\n<b>HTML</b>\n\n```cpp\nreturn 7;\n// 中文\n```");
    message_markdown doc(md, view->font(), 240, {});
    assert(doc.toPlainText().contains("粗体") && doc.toPlainText().contains("<b>HTML</b>"));
    bool bold = false, code = false, link = false;
    for (auto block = doc.begin(); block.isValid(); block = block.next())
    {
        if (block.text() == "return 7;")
        {
            auto rect = doc.documentLayout()->blockBoundingRect(block);
            assert(doc.code_at(rect.topLeft() + QPointF(3, 5)) == QStringLiteral("return 7;\n// 中文"));
            code = true;
        }
        for (auto it = block.begin(); !it.atEnd(); ++it)
        {
            auto f = it.fragment();
            assert(!f.charFormat().isImageFormat());
            if (f.text() == "粗体")
            {
                bold = f.charFormat().fontWeight() > QFont::Normal;
            }
            if (f.text() == QStringLiteral("复制代码"))
            {
                assert(f.charFormat().isAnchor() && doc.code_for_link(QUrl(f.charFormat().anchorHref())) == QStringLiteral("return 7;\n// 中文"));
            }
            else if (f.charFormat().isAnchor())
            {
                assert(f.charFormat().anchorHref() == "https://example.test");
                link = true;
            }
        }
    }
    assert(bold && code && link);
    auto const fences = QStringLiteral("```cpp\nline 1\nline 2\n```\n\n```cpp\nnext block\n```");
    message_markdown adjacent(fences, view->font(), 240, {});
    for (auto b = adjacent.begin(); b.isValid(); b = b.next())
    {
        if (b.text() == "line 2" || b.text() == "next block")
        {
            auto point = adjacent.documentLayout()->blockBoundingRect(b).topLeft() + QPointF(3, 5);
            assert(adjacent.code_at(point) == (b.text() == "line 2" ? QStringLiteral("line 1\nline 2") : QStringLiteral("next block")));
        }
    }
    for (auto theme : {chat_appearance::light, chat_appearance::dark})
    {
        theme_manager::instance().set_appearance(theme);
        chat::poll_summary poll{1, false, {{1, 0, "A"}, {2, 0, "B"}}};
        int const width = 270;
        auto draw = [&]
        {
            auto const height = layout_poll_card(nullptr, view->font(), width, QStringLiteral("投票问题"), poll);
            QImage image(width, height, QImage::Format_RGB32);
            image.fill(themed("bubble_in"));
            QPainter painter(&image);
            assert(layout_poll_card(&painter, view->font(), width, QStringLiteral("投票问题"), poll) == height);
            painter.end();
            return image;
        };
        auto ballot = draw();
        poll.closed = true;
        auto zero = draw();
        assert(ballot.height() != zero.height());
        poll.options[0].votes = 1;
        poll.participants = 1;
        auto voted = draw();
        assert(voted.size() == zero.size());
        auto check_bars = [&](QList<int> votes, int participants, QList<int> percentages, bool multiple = false)
        {
            poll.participants = participants;
            poll.multiple = multiple;
            poll.options.clear();
            for (int i = 0; i < votes.size(); ++i)
            {
                poll.options.push_back({i + 1, votes[i], "选项"});
            }
            assert(poll_percentages(poll) == percentages);
            auto image = draw();
            QFont percent_font(view->font());
            percent_font.setWeight(QFont::DemiBold);
            const int left = QFontMetrics(percent_font).horizontalAdvance(QStringLiteral("100%")) + 12;
            QList<int> widths;
            int longest = 0;
            for (int y = 0; y < image.height(); ++y)
            {
                int length = 0;
                for (int x = left; x < width; ++x)
                {
                    length += image.pixelColor(x, y) == themed("accent");
                }
                if (length > 0)
                {
                    longest = std::max(longest, length);
                }
                else if (longest > 0)
                {
                    widths.append(std::exchange(longest, 0));
                }
            }
            assert(widths.size() == percentages.size());
            for (int i = 0; i < widths.size(); ++i)
            {
                const int expected = qRound(4 + (width - left - 8) * percentages[i] / 100.);
                assert(std::abs(widths[i] - expected) <= 1);
            }
        };
        check_bars({1, 0, 1, 0}, 2, {50, 0, 50, 0});
        check_bars({4, 7, 3, 4}, 18, {22, 39, 17, 22});
        check_bars({1, 0, 0}, 1, {100, 0, 0});
        check_bars({1, 1, 1}, 3, {33, 33, 33});
        check_bars({4, 2, 2}, 4, {100, 50, 50}, true);
        check_bars({0, 0}, 0, {0, 0});
        for (int i = 3; i <= 10; ++i)
        {
            poll.options.push_back({i, 0, QString(80, QChar(0x754c)).toStdString()});
        }
        assert(draw().height() > 3 * voted.height());
    }
    message_markdown quotation(
        QStringLiteral("前言\n\n> 引用\n>\n> > 深层 `code`\n> >\n> > ~~~cpp\n> > int x;\n> > ~~~\n\n结尾"), view->font(), 300, {});
    QImage guide_image(quotation.size().toSize(), QImage::Format_RGB32);
    guide_image.fill(themed("surface"));
    QPainter guides(&guide_image);
    quotation.draw_quote_guides(guides);
    guides.end();
    auto nested = quotation.find(QStringLiteral("深层")).block();
    auto outer = quotation.find(QStringLiteral("引用")).block();
    assert(nested.blockFormat().leftMargin() > outer.blockFormat().leftMargin());
    auto nested_y = qFloor(quotation.documentLayout()->blockBoundingRect(nested).center().y());
    assert(guide_image.pixelColor(2, nested_y) == themed("border_strong") &&
           guide_image.pixelColor(qRound(outer.blockFormat().leftMargin()) + 2, nested_y) == themed("border_strong"));
    assert(quotation.find(QStringLiteral("code")).charFormat().background().color() == themed("subtle"));
    message_delegate card_delegate;
    check_message_avatars(*view, card_delegate);
    check_card_actions(*view, card_delegate);
    check_code_cards(view->font());
    check_sidebar_status(chat, conversation);
    QDesktopServices::unsetUrlHandler("http");
    QDesktopServices::unsetUrlHandler("https");
    std::cout
        << "PASS Qt HTTP/HTTPS anchors, Unicode/punctuation, wrapped hit testing, both themes, system URL dispatch, clipboard and multiselect\n";
}
