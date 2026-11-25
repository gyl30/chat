// Exercise the actual bubble delegate and context menu without opening an external browser.
#include "chat_widget.hpp"
#include "message_delegate.hpp"
#include "message_links.hpp"
#include "message_markdown.hpp"
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include "message_model.hpp"
#include "theme_manager.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QPlainTextEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QTextBrowser>
#include <QTimer>
#include <cassert>
#include <iostream>

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
    m.deleted = true;
    assert(check("问题") == "消息已删除");
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
                assert(!local.data(message_model::read_role).isValid() && !local.data(message_model::read_count_role).isValid());
            }
        }
        model.set_pending({});
        m.id = 100;
        assert(model.add_message(m));
        auto sent = model.index(0);
        assert(sent.data(message_model::send_state_role).toInt() == message_model::sent);
        assert(sent.data(message_model::read_role).isValid() && !sent.data(message_model::read_role).toBool());
        model.set_read_message(2, m.id);
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
    QInputMethodEvent cancel;
    QApplication::sendEvent(edit, &cancel);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    check_display_roles();
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
    chat.set_messages(12, {item}, {}, false, false, false, {});
    chat.show();
    app.processEvents();
    chat.activateWindow();
    app.processEvents();
    check_editor_reflow(chat);
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
    check_code_cards(view->font());
    QDesktopServices::unsetUrlHandler("http");
    QDesktopServices::unsetUrlHandler("https");
    std::cout
        << "PASS Qt HTTP/HTTPS anchors, Unicode/punctuation, wrapped hit testing, both themes, system URL dispatch, clipboard and multiselect\n";
}
