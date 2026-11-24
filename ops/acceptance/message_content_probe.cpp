// Exercise the actual bubble delegate and context menu without opening an external browser.
#include "chat_widget.hpp"
#include "message_delegate.hpp"
#include "message_links.hpp"
#include "message_model.hpp"
#include "theme_manager.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QTextBrowser>
#include <QTimer>
#include <cassert>
#include <iostream>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
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
        QStyleOptionViewItem option;
        option.initFrom(view);
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
    QDesktopServices::unsetUrlHandler("http");
    QDesktopServices::unsetUrlHandler("https");
    std::cout
        << "PASS Qt HTTP/HTTPS anchors, Unicode/punctuation, wrapped hit testing, both themes, system URL dispatch, clipboard and multiselect\n";
}
