#include <QApplication>
#include <QAccessible>
#include <QBuffer>
#include <QAction>
#include <QFrame>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHeaderView>
#include <QImage>
#include <QPainter>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMenu>
#include <QtMath>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QInputDialog>
#include <QInputMethodEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QThread>
#include <QTemporaryDir>
#include <QSortFilterProxyModel>
#include <QScrollBar>
#include <QScrollArea>
#include <QSystemTrayIcon>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextLayout>
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
#include "message_search_dialog.hpp"
#include "attachment_dialog.hpp"
#include "conversation_model.hpp"
#include "user_delegate.hpp"
#include "message_delegate.hpp"
#include "theme.hpp"
#include "avatar.hpp"

void check(bool v, char const* text)
{
    if (!v)
    {
        std::cerr << "FAIL Qt assertion: " << text << '\n';
        throw std::runtime_error(text);
    }
}
QColor logical_pixel(QImage const& image, QPoint point)
{
    auto const ratio = image.devicePixelRatio();
    QPoint const pixel(qFloor(point.x() * ratio), qFloor(point.y() * ratio));
    check(image.rect().contains(pixel), "A logical widget sample lies inside its device-pixel capture");
    return image.pixelColor(pixel);
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
void check_profile_avatar_click(QAbstractItemView* list, QPoint point)
{
    int opened_profiles = 0;
    QTimer close_profile;
    QObject::connect(&close_profile, &QTimer::timeout, list, [&] {
        auto* profile = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (profile && profile->objectName() == "profileDialog")
        {
            ++opened_profiles;
            profile->reject();
        }
    });
    close_profile.start(0);
    QMouseEvent press(QEvent::MouseButtonPress, point, list->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, point, list->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &press);
    QApplication::sendEvent(list->viewport(), &release);
    close_profile.stop();
    check(opened_profiles == 1, "One avatar click opens one profile rather than reopening after close");
}
void check_preedit_display(QPlainTextEdit* editor)
{
    auto const original = editor->toPlainText();
    QString const text = QStringLiteral("👩‍💻1️⃣ left é 👍️🏽 tail 👨‍👩‍👧‍👦");
    QStringList const clusters{QStringLiteral("👩‍💻"), QStringLiteral("1️⃣"),
                               QStringLiteral("👍️🏽"), QStringLiteral("👨‍👩‍👧‍👦")};
    bool const has_emoji_font = QFontDatabase::families().contains(QStringLiteral("Noto Color Emoji"));
    auto const original_font = editor->font();
    for (auto const position : {qsizetype(0), clusters[0].size(), text.indexOf(QStringLiteral("tail")), text.size()})
    {
        editor->setPlainText(text);
        auto cursor = editor->textCursor();
        cursor.setPosition(static_cast<int>(position));
        editor->setTextCursor(cursor);
        for (auto const& preedit : {QStringLiteral("nihao"), QStringLiteral("zhongwen")})
        {
            QTextCharFormat format;
            format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
            QInputMethodEvent input(preedit, {QInputMethodEvent::Attribute(
                QInputMethodEvent::TextFormat, 0, static_cast<int>(preedit.size()), format)});
            QApplication::sendEvent(editor, &input);
            QApplication::processEvents();
            if (preedit == QStringLiteral("zhongwen"))
            {
                auto font = original_font;
                font.setWeight(font.weight() == QFont::Normal ? QFont::Medium : QFont::Normal);
                editor->setFont(font);
                QApplication::processEvents();
            }
            auto const* layout = editor->textCursor().block().layout();
            check(editor->toPlainText() == text && !editor->document()->isUndoAvailable() &&
                      layout->preeditAreaPosition() == position && layout->preeditAreaText() == preedit,
                  "Preedit at either side of committed emoji preserves the original draft and undo history");
            auto const ranges = layout->formats();
            check(std::ranges::any_of(ranges, [&](auto const& range) {
                return range.start == position && range.length == preedit.size() &&
                       range.format.underlineStyle() == QTextCharFormat::SingleUnderline;
            }), "Input-method underline remains on its own preedit text");
            for (auto const& range : ranges)
            {
                if (range.format.font().family() != QStringLiteral("Noto Color Emoji")) { continue; }
                check(range.start + range.length <= position || range.start >= position + preedit.size(),
                      "Committed emoji formatting never expands into the input method's preedit");
            }
            for (auto const& cluster : clusters)
            {
                auto const start = text.indexOf(cluster);
                auto const displayed_start = start + (start >= position ? preedit.size() : 0);
                auto const covered = std::ranges::any_of(ranges, [&](auto const& range) {
                    return range.format.font().family() == QStringLiteral("Noto Color Emoji") &&
                           range.start <= displayed_start && range.start + range.length >= displayed_start + cluster.size();
                });
                check(covered == has_emoji_font,
                      "Every committed emoji retains its display font before and after a changing preedit area");
            }
            editor->setFont(original_font);
            QApplication::processEvents();
        }
        QInputMethodEvent cancel;
        QApplication::sendEvent(editor, &cancel);
        QApplication::processEvents();
        check(editor->toPlainText() == text && editor->textCursor().block().layout()->preeditAreaText().isEmpty() &&
                  !editor->document()->isUndoAvailable(), "Cancelling preedit restores display without an undo command");
    }
    editor->setPlainText(original);
    QApplication::processEvents();
}
void click_list_body(QAbstractItemView* list, QModelIndex index)
{
    list->scrollTo(index);
    QApplication::processEvents();
    auto const point = list->visualRect(index).center();
    QMouseEvent press(QEvent::MouseButtonPress, point, list->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, point, list->viewport()->mapToGlobal(point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &press);
    QApplication::sendEvent(list->viewport(), &release);
}
void check_authentication_layout()
{
    main_window window(QStringLiteral("ws://127.0.0.1:18769/ws"));
    window.resize(980, 640);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
    wait([&] { return window.isActiveWindow(); });
    auto* card = window.findChild<QFrame*>("loginCard");
    QLineEdit* username = nullptr;
    QLineEdit* password = nullptr;
    for (auto* field : card->findChildren<QLineEdit*>())
    {
        if (field->placeholderText() == QStringLiteral("用户名")) { username = field; }
        if (field->placeholderText() == QStringLiteral("密码")) { password = field; }
    }
    check(username && password && username->hasFocus(), "Login starts at username rather than server configuration");
    auto* server = card->findChild<QLineEdit*>("serverUrlEdit");
    auto* settings = card->findChild<QToolButton*>("serverSettingsButton");
    check(server && settings && server->isHidden(), "Server address is available through progressive settings");
    check(!username->accessibleName().isEmpty() && !password->accessibleName().isEmpty() &&
          !server->accessibleName().isEmpty(), "Authentication fields have accessible names");
    check(password->echoMode() == QLineEdit::Password, "Login password stays masked");
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(username, &tab);
    check(password->hasFocus(), "Tab moves from username to password");
    username->setText(QStringLiteral("张 三"));
    settings->click();
    QApplication::processEvents();
    check(server->isVisible() && server->hasFocus(), "Expanded settings focus the server address");
    server->setText(QStringLiteral("ws://localhost:18769/ws"));
    settings->click();
    QApplication::processEvents();
    check(server->isHidden() && username->hasFocus() && username->text() == QStringLiteral("张 三") &&
          server->text() == QStringLiteral("ws://localhost:18769/ws"), "Collapsing settings preserves identity and connection address");
    auto* login_focus = QApplication::focusWidget();
    window.findChild<QPushButton*>("loginButton")->click();
    check(card->findChild<QLabel*>("subtleText")->text().contains(QStringLiteral("密码")),
          "Missing credentials produce inline feedback without connecting");
    auto* login_feedback = card->findChild<QLabel*>("subtleText");
    auto* login_label_accessible = QAccessible::queryAccessibleInterface(login_feedback);
    check(login_label_accessible && login_label_accessible->role() == QAccessible::StaticText,
          "The visible login feedback retains ordinary label semantics");
    auto* login_notification = login_feedback->findChild<QObject*>("feedbackNotification");
    auto* login_accessible = QAccessible::queryAccessibleInterface(login_notification);
    check(login_accessible && login_accessible->role() == QAccessible::Notification &&
          login_accessible->text(QAccessible::Name) == login_feedback->text(),
          "Authentication errors expose their current text as an accessible notification");
    check(QApplication::focusWidget() == login_focus, "Login feedback preserves the current keyboard focus");
    auto const login_error = login_feedback->text();
    window.findChild<QPushButton*>("loginButton")->click();
    check(login_feedback->text() == login_error && login_accessible->text(QAccessible::Name) == login_error &&
          QApplication::focusWidget() == login_focus,
          "Repeating an invalid login preserves its current feedback and keyboard focus");
    login_feedback->clear();
    check(login_accessible->text(QAccessible::Name).isEmpty(), "Clearing login feedback clears its accessible name");
    login_feedback->setText(QStringLiteral("正在连接…"));
    check(login_accessible->text(QAccessible::Name) == login_feedback->text() &&
          login_accessible->text(QAccessible::Name) != login_error,
          "The same accessible interface reads current status instead of caching an old login error");
    window.findChild<QPushButton*>("loginButton")->click();
    check(login_accessible->text(QAccessible::Name) == login_error && QApplication::focusWidget() == login_focus,
          "A new login rejection restores the current error without moving keyboard focus");
    for (auto const size : {QSize(980, 640), QSize(1180, 760), QSize(1280, 800), QSize(1440, 900), QSize(1920, 1080)})
    {
        window.resize(size);
        for (int expanded = 0; expanded < 2; ++expanded)
        {
            settings->setChecked(expanded);
            QApplication::processEvents();
            auto* page = card->parentWidget();
            check(page->rect().contains(card->geometry()), "Authentication card fits every supported desktop size");
            for (auto* field : card->findChildren<QLineEdit*>())
            {
                if (field->isVisible())
                { check(card->rect().contains(field->geometry()), "Authentication fields fit both collapsed and expanded settings"); }
            }
        }
    }
    window.findChild<QPushButton*>("registerButton")->click();
    QApplication::processEvents();
    auto* registration = window.findChild<QDialog*>("registrationDialog");
    check(registration->isVisible() && registration->width() == card->width(), "Registration shares the login card geometry");
    auto* registration_username = registration->findChild<QLineEdit*>("registrationUsernameEdit");
    check(registration_username && registration_username->hasFocus(), "Registration starts at username");
    auto* registration_focus = QApplication::focusWidget();
    registration->findChild<QPushButton*>("registrationSubmitButton")->click();
    check(registration->findChild<QLabel*>("subtleText")->text().contains(QStringLiteral("用户名")),
          "Empty registration remains actionable with inline feedback");
    auto* registration_feedback = registration->findChild<QLabel*>("subtleText");
    auto* registration_label_accessible = QAccessible::queryAccessibleInterface(registration_feedback);
    check(registration_label_accessible && registration_label_accessible->role() == QAccessible::StaticText,
          "The visible registration feedback retains ordinary label semantics");
    auto* registration_notification = registration_feedback->findChild<QObject*>("feedbackNotification");
    auto* registration_accessible = QAccessible::queryAccessibleInterface(registration_notification);
    check(registration_accessible && registration_accessible->role() == QAccessible::Notification &&
          registration_accessible->text(QAccessible::Name) == registration_feedback->text(),
          "Registration errors share the accessible feedback semantics");
    check(QApplication::focusWidget() == registration_focus, "Registration feedback preserves the current keyboard focus");
    auto const registration_error = registration_feedback->text();
    registration->findChild<QPushButton*>("registrationSubmitButton")->click();
    check(registration_feedback->text() == registration_error &&
          registration_accessible->text(QAccessible::Name) == registration_error &&
          QApplication::focusWidget() == registration_focus,
          "Repeating an invalid registration preserves its current feedback and keyboard focus");
    registration_username->setText(QStringLiteral("probe_user"));
    for (auto* field : registration->findChildren<QLineEdit*>())
    {
        if (field->accessibleName() == QStringLiteral("密码")) { field->setText(QStringLiteral("one")); }
        if (field->accessibleName() == QStringLiteral("确认密码")) { field->setText(QStringLiteral("two")); }
    }
    registration->findChild<QPushButton*>("registrationSubmitButton")->click();
    check(registration_feedback->text() == QStringLiteral("两次输入的密码不一致") &&
          registration_accessible->text(QAccessible::Name) == registration_feedback->text() &&
          QApplication::focusWidget() == registration_focus,
          "A changed registration error updates the same accessible name without moving keyboard focus");
    registration->findChild<QPushButton*>("registrationCancelButton")->click();
    check(!registration->isVisible(), "Cancel returns to the login page");
    window.activateWindow();
    QApplication::processEvents();
    window.findChild<QPushButton*>("registerButton")->click();
    registration->activateWindow();
    QApplication::processEvents();
    wait([&] { return registration_username->hasFocus(); });
    check(registration->isVisible() && registration_username->hasFocus() && registration_feedback->text().isEmpty() &&
          registration_accessible->text(QAccessible::Name).isEmpty(),
          "Reopening registration resets focus to username and clears the previous accessible error");
    registration->findChild<QPushButton*>("registrationCancelButton")->click();
    std::cout << "PASS Qt authentication hierarchy, keyboard focus and responsive settings\n";
}

void check_friend_request_layout()
{
    chat_widget page;
    page.setStyleSheet(chat_style_sheet());
    page.resize(1180, 760);
    page.set_user(QStringLiteral("本人"), 1);
    page.set_connection_available(true);
    page.set_friend_requests({{200, QStringLiteral("收到 申请"), false, 0, {}}},
                             {{201, QStringLiteral("发出 申请"), false, 0, {}}}, {});
    page.show();
    page.findChild<QPushButton*>("newFriendsButton")->click();
    QApplication::processEvents();
    auto* incoming = page.findChild<QListWidget*>("incomingFriendRequests");
    auto* outgoing = page.findChild<QListWidget*>("outgoingFriendRequests");
    check(incoming->visualItemRect(incoming->item(0)).height() == 62 &&
          outgoing->visualItemRect(outgoing->item(0)).height() == 62,
          "Friend requests share the contact row rhythm");
    check(outgoing->y() - (incoming->y() + incoming->height()) < 80,
          "A single incoming request does not leave half a window before outgoing requests");
    check(incoming->verticalScrollBar()->maximum() == 0 && outgoing->verticalScrollBar()->maximum() == 0,
          "A single request fits its complete row without inner scrolling");
    QImage avatar(16, 16, QImage::Format_RGB32);
    QColor const avatar_color(QStringLiteral("#C05656"));
    avatar.fill(avatar_color);
    QByteArray avatar_bytes;
    QBuffer avatar_buffer(&avatar_bytes);
    avatar_buffer.open(QIODevice::WriteOnly);
    check(avatar.save(&avatar_buffer, "PNG"), "Friend request avatar fixture encodes");
    page.avatars().observe(200, {1, true});
    page.avatars().receive(200, {1, true}, avatar_bytes);
    QApplication::processEvents();
    auto const request_avatar = incoming->viewport()->grab().toImage();
    check(logical_pixel(request_avatar, QPoint(20, incoming->visualItemRect(incoming->item(0)).center().y())) == avatar_color,
          "A downloaded request avatar refreshes the visible row without reloading requests");
    for (auto* list : {incoming, outgoing})
    {
        auto const point = QPoint(20, list->visualItemRect(list->item(0)).center().y());
        check_profile_avatar_click(list, point);
    }
    QList<user_data> many;
    for (int row = 0; row < 80; ++row)
    { many.push_back({300 + row, QStringLiteral("申请 用户😀") + QString::number(row), false, 0, {}}); }
    page.set_friend_requests(many, {{201, QStringLiteral("发出 申请"), false, 0, {}}}, {});
    for (auto const size : {QSize(980, 640), QSize(1180, 760), QSize(1280, 800), QSize(1440, 900), QSize(1920, 1080)})
    {
        page.resize(size);
        QApplication::processEvents();
        check(incoming->verticalScrollBar()->maximum() > 0 && outgoing->verticalScrollBar()->maximum() == 0 &&
              outgoing->height() == 62, "A long incoming list scrolls without clipping the single outgoing request");
        check(incoming->parentWidget()->rect().contains(incoming->geometry()) &&
              outgoing->parentWidget()->rect().contains(outgoing->geometry()), "Both request sections fit supported window sizes");
    }
    incoming->setCurrentRow(40);
    incoming->scrollToItem(incoming->currentItem());
    many.push_back({500, QStringLiteral("另一份新申请"), false, 0, {}});
    page.set_friend_requests(many, {{201, QStringLiteral("发出 申请"), false, 0, {}}}, {});
    QApplication::processEvents();
    check(incoming->currentItem() && incoming->currentItem()->data(Qt::UserRole).toLongLong() == 340 &&
          incoming->viewport()->rect().contains(incoming->visualItemRect(incoming->currentItem())),
          "An unrelated request refresh preserves the selected user and keeps that row visible");
    incoming->verticalScrollBar()->setValue(0);
    many.prepend({501, QStringLiteral("顶部新申请"), false, 0, {}});
    page.set_friend_requests(many, {{201, QStringLiteral("发出 申请"), false, 0, {}}}, {});
    QApplication::processEvents();
    check(incoming->currentItem()->data(Qt::UserRole).toLongLong() == 340 &&
          incoming->verticalScrollBar()->value() == 0,
          "Browsing away from the selected request is not interrupted by a refresh");
    incoming->scrollToItem(incoming->currentItem());
    page.activateWindow();
    incoming->setFocus();
    QApplication::processEvents();
    check(incoming->hasFocus() && !incoming->accessibleName().isEmpty() && !outgoing->accessibleName().isEmpty(),
          "Request sections have keyboard focus and accessible names");
    auto const selected_row = incoming->visualItemRect(incoming->currentItem());
    auto const focused = incoming->viewport()->grab().toImage();
    check(logical_pixel(focused, QPoint(1, selected_row.center().y())).lightness() <
          logical_pixel(focused, QPoint(incoming->viewport()->width() - 8, selected_row.center().y())).lightness() - 40,
          "Keyboard focus has a visible outline rather than relying on selection color");
    bool opened = false;
    QTimer::singleShot(0, &page, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        opened = dialog->findChild<QLabel*>("profileDialogName")->text() == QStringLiteral("申请 用户😀40");
        dialog->reject();
    });
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(incoming, &enter);
    QApplication::processEvents();
    check(opened, "Enter opens the selected friend request's real profile action flow");
    QList<QPair<qint64, bool>> responses;
    QList<qint64> cancellations;
    auto const response_connection = QObject::connect(&page, &chat_widget::friend_request_respond_requested, &page,
        [&](qint64 user, bool accept) { responses.push_back({user, accept}); });
    auto const cancel_connection = QObject::connect(&page, &chat_widget::friend_request_cancel_requested, &page,
        [&](qint64 user) { cancellations.push_back(user); });
    for (int action = 0; action < 3; ++action)
    {
        auto* list = action == 2 ? outgoing : incoming;
        auto const user = action == 2 ? 201 : 340;
        auto const username = action == 2 ? QStringLiteral("发出 申请") : QStringLiteral("申请 用户😀40");
        int opened_profiles = 0;
        page.activateWindow();
        list->setFocus();
        QApplication::processEvents();
        if (action == 2)
        {
            QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
            QApplication::sendEvent(list, &home);
        }
        QTimer::singleShot(0, &page, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(dialog && dialog->findChild<QLabel*>("profileDialogName")->text() == username,
                  "Keyboard friend request activation preserves the exact current user");
            ++opened_profiles;
            QAbstractButton* button = nullptr;
            if (action == 0)
            {
                for (auto* candidate : dialog->findChildren<QToolButton*>())
                { if (candidate->text() == QStringLiteral("接受申请")) { button = candidate; } }
            }
            else
            {
                button = dialog->findChild<QPushButton*>(action == 1 ? "rejectFriendRequestButton" : "cancelFriendRequestButton");
            }
            check(button && button->isVisible() && button->isEnabled(), "The current pending relationship exposes its keyboard action");
            if (action == 2)
            {
                QToolButton* waiting = nullptr;
                for (auto* candidate : dialog->findChildren<QToolButton*>())
                {
                    if (candidate->text() == QStringLiteral("等待验证")) { waiting = candidate; }
                }
                check(waiting && waiting->isVisible() && !waiting->isEnabled(),
                      "An outgoing pending request exposes its status without enabling a direct chat");
                check(!dialog->findChild<QPushButton*>("rejectFriendRequestButton")->isVisible(),
                      "An outgoing request does not expose the incoming rejection action");
            }
            for (int step = 0; step < 12 && !button->hasFocus(); ++step)
            {
                QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
                QApplication::sendEvent(QApplication::focusWidget(), &tab);
                QApplication::processEvents();
            }
            check(button->hasFocus(), "Tab reaches the pending relationship action in the real profile dialog");
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
            QApplication::sendEvent(button, &press);
            QApplication::sendEvent(button, &release);
            check(!button->isEnabled(), "A keyboard request action cannot be resubmitted while its result is pending");
            page.finish_add_contact(999, QStringLiteral("其他申请的错误"));
            check(!button->isEnabled(), "Another user's response cannot reset the current pending action");
            page.finish_add_contact(user, QStringLiteral("连接中断，稍后重试"));
            check(button->isEnabled() && dialog->findChild<QLabel*>("profileContactStatus")->text().contains(QStringLiteral("连接中断")),
                  "The same user's error restores an actionable relationship with inline feedback");
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(dialog, &escape);
        });
        QKeyEvent activate(QEvent::KeyPress, action == 2 ? Qt::Key_Enter : Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(list, &activate);
        QApplication::processEvents();
        check(opened_profiles == 1 && list->currentItem()->data(Qt::UserRole).toLongLong() == user,
              "Return and keypad Enter open one profile and preserve the request's current identity after Escape");
    }
    check(responses == QList<QPair<qint64, bool>>{{340, true}, {340, false}} && cancellations == QList<qint64>{201},
          "Keyboard accept, reject and cancel each emit exactly one action for the correct pending user");
    QObject::disconnect(response_connection);
    QObject::disconnect(cancel_connection);
    page.activateWindow();
    incoming->setFocus();
    QApplication::processEvents();
    page.resize(980, 640);
    QApplication::processEvents();
    QApplication::processEvents();
    check(incoming->hasFocus() && incoming->viewport()->rect().contains(incoming->visualItemRect(incoming->currentItem())),
          "Shrinking the window keeps the keyboard's selected request fully visible");
    page.set_friend_requests({}, {}, QStringLiteral("连接中断，稍后重试"));
    QApplication::processEvents();
    check(incoming->currentItem()->data(Qt::UserRole).toLongLong() == 340 &&
          page.findChild<QLabel*>("friendRequestsStatus")->text().contains(QStringLiteral("连接中断")),
          "Request load errors leave known requests and selection available with explicit feedback");
    page.set_friend_requests({}, {}, {});
    QApplication::processEvents();
    auto* status = page.findChild<QLabel*>("friendRequestsStatus");
    check(status->isVisible() && status->text().contains(QStringLiteral("暂无好友申请")) &&
          !incoming->isVisible() && !outgoing->isVisible(),
          "No requests shows an explicit empty state instead of empty list sections");
    std::cout << "PASS Qt friend request row rhythm and empty state\n";
}

void check_group_detail_layout()
{
    avatar_cache avatars;
    group_dialog dialog(1, 1, QStringLiteral("群资料"), {}, false, nullptr, &avatars);
    dialog.setStyleSheet(chat_style_sheet());
    dialog.set_members(1, {{1, "owner", chat::member_role::owner, {}},
                           {2, "admin", chat::member_role::admin, {}},
                           {3, "member", chat::member_role::member, {}}}, {});
    dialog.show();
    QApplication::processEvents();
    QImage avatar(16, 16, QImage::Format_RGB32);
    QColor const color(QStringLiteral("#C05656"));
    avatar.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    check(avatar.save(&buffer, "PNG"), "Group member avatar fixture encodes");
    avatars.observe(3, {1, true});
    avatars.receive(3, {1, true}, bytes);
    QApplication::processEvents();
    auto* preview = dialog.findChild<QListWidget*>("groupMemberPreview");
    auto* member_rows = dialog.findChild<QListWidget*>("groupMembersList");
    auto check_accessible_rows = [](QListWidget* list, QStringList const& expected, bool native_snapshot = false) {
        QApplication::processEvents();
        // An offscreen bridge is inactive and does not receive model reset events.
        // Read actual cells at initialization; later snapshots inspect canonical
        // model roles. Live bridge delivery is verified by the native AT-SPI task.
        auto* accessible = native_snapshot ? QAccessible::queryAccessibleInterface(list) : nullptr;
        if (native_snapshot)
        { check(accessible && accessible->tableInterface(), "Group user rows expose the public Qt accessible table interface"); }
        auto* table = accessible ? accessible->tableInterface() : nullptr;
        check(list->model()->rowCount() == expected.size() && (!table || table->rowCount() == expected.size()),
              "Accessible group row count follows the actual visible data, including private row removal");
        for (int row = 0; row < expected.size(); ++row)
        {
            if (table)
            {
                auto* cell = table->cellAt(row, 0);
                check(cell && cell->role() == QAccessible::ListItem,
                      "Group user context belongs to the actual accessible list item");
                if (cell->text(QAccessible::Name) != expected[row])
                {
                    std::cerr << "Group accessible row " << row << " expected [" << expected[row].toStdString()
                              << "] actual [" << cell->text(QAccessible::Name).toStdString() << "]\n";
                }
                check(cell->text(QAccessible::Name) == expected[row],
                      "Accessible group user names include canonical username, current role/self or application status");
            }
            auto const index = list->model()->index(row, 0);
            check(index.data(Qt::AccessibleTextRole).toString() == expected[row] &&
                  index.data(Qt::AccessibleTextRole).toString() ==
                      index.data(Qt::DisplayRole).toString() + QStringLiteral(" · ") + index.data(Qt::StatusTipRole).toString(),
                  "Live accessible model context matches literal role/status and the actual displayed username/status");
        }
    };
    auto const initial_names = QStringList{QStringLiteral("owner · 群主 · 你"),
                                          QStringLiteral("admin · 管理员"),
                                          QStringLiteral("member · 成员")};
    check_accessible_rows(member_rows, initial_names, true);
    check_accessible_rows(preview, initial_names, true);
    check(member_rows->item(0)->text() == QStringLiteral("owner") &&
          member_rows->item(0)->data(Qt::UserRole).toLongLong() == 1,
          "Accessible member context preserves the original display username and action identity");
    auto const image = preview->viewport()->grab().toImage();
    check(logical_pixel(image, QPoint(20, preview->visualItemRect(preview->item(2)).center().y())) == color,
          "A downloaded member avatar refreshes the visible group overview without reopening it");
    int profile_requests = 0;
    auto profile_connection = QObject::connect(&dialog, &group_dialog::user_requested, &dialog, [&](qint64 user, QString const&) {
        check(user == 3, "Overview avatar opens the clicked member");
        ++profile_requests;
    });
    auto const avatar_point = QPoint(20, preview->visualItemRect(preview->item(2)).center().y());
    QMouseEvent press(QEvent::MouseButtonPress, avatar_point, preview->viewport()->mapToGlobal(avatar_point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, avatar_point, preview->viewport()->mapToGlobal(avatar_point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(preview->viewport(), &press);
    QApplication::sendEvent(preview->viewport(), &release);
    check(profile_requests == 1, "One overview avatar click opens one profile rather than two sequential dialogs");
    QObject::disconnect(profile_connection);
    auto* tabs = dialog.findChild<QTabWidget*>("groupTabs");
    tabs->setCurrentIndex(3);
    dialog.resize(420, 480);
    QApplication::processEvents();
    check(dialog.width() <= 420 && dialog.height() <= 480,
          "Group details fit a small desktop window without forcing a taller or wider dialog");
    auto* management = qobject_cast<QScrollArea*>(tabs->widget(3));
    check(management && management->verticalScrollBar()->maximum() > 0,
          "Small group management scrolls its content instead of clipping its actions");
    dialog.set_invite(1, QString(64, 'a'), {});
    dialog.activateWindow();
    dialog.findChild<QLineEdit*>("groupTitleEdit")->setFocus();
    QApplication::processEvents();
    auto* revoke = dialog.findChild<QPushButton*>("groupRevokeInviteButton");
    for (int step = 0; step < 12 && !revoke->hasFocus(); ++step)
    {
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(QApplication::focusWidget(), &tab);
        QApplication::processEvents();
    }
    check(revoke->hasFocus() && management->viewport()->rect().contains(revoke->mapTo(management->viewport(), QPoint{})),
          "Keyboard focus brings a lower group management action into view");
    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    check(dialog.findChild<QPushButton*>("groupReadAnnouncementButton")->isHidden(),
          "An empty announcement does not offer a redundant full announcement action");
    tabs->setCurrentIndex(2);
    dialog.set_requests(1, {}, 0, false, {});
    QApplication::processEvents();
    auto* requests_status = dialog.findChild<QLabel*>("groupJoinRequestsStatus");
    check(requests_status && requests_status->isVisible() && requests_status->text().contains(QStringLiteral("暂无待处理申请")) &&
          dialog.findChild<QPushButton*>("groupMoreRequestsButton")->isHidden(),
          "A completed empty request list explains its state without an unavailable pagination action");
    auto* requests = dialog.findChild<QListWidget*>("groupJoinRequestsList");
    dialog.set_requests(1, {{10, "first", false, 0, {}}, {11, "selected", false, 0, {}}}, 0, false, {});
    check_accessible_rows(requests, {QStringLiteral("first · 待审批"), QStringLiteral("selected · 待审批")}, true);
    requests->setCurrentRow(1);
    dialog.set_requests(1, {{12, "new", false, 0, {}}, {10, "first", false, 0, {}}, {11, "selected", false, 0, {}}}, 0, false, {});
    check(requests->currentItem() && requests->currentItem()->data(Qt::UserRole).toLongLong() == 11,
          "An unrelated new join request preserves the applicant being reviewed");
    check_accessible_rows(requests, {QStringLiteral("new · 待审批"), QStringLiteral("first · 待审批"),
                                    QStringLiteral("selected · 待审批")});
    qint64 opened_user = 0;
    QString opened_name;
    QObject::connect(&dialog, &group_dialog::user_requested, &dialog, [&](qint64 user, QString name) {
        opened_user = user;
        opened_name = std::move(name);
    });
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(requests, &enter);
    check(opened_user == 11 && opened_name == QStringLiteral("selected"),
          "Enter opens the selected applicant's original profile flow");
    conversation_data snapshot;
    snapshot.id = 1;
    snapshot.group = true;
    snapshot.username = QStringLiteral("群资料");
    snapshot.announcement = QStringLiteral("第一段公告\n") + QString(900, QChar(0x4e2d));
    dialog.set_conversations({snapshot}, {});
    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    bool full_announcement = false;
    bool keyboard_announcement = false;
    QTimer::singleShot(0, &dialog, [&] {
        auto* reader = QApplication::activeModalWidget();
        if (!reader || reader->objectName() != "groupAnnouncementDialog") { return; }
        auto* text = reader->findChild<QPlainTextEdit*>();
        full_announcement = text->toPlainText() == snapshot.announcement;
        reader->activateWindow();
        auto* close = reader->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Close);
        close->setFocus();
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(close, &tab);
        QApplication::processEvents();
        QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::ControlModifier);
        QApplication::sendEvent(QApplication::focusWidget(), &end);
        QApplication::processEvents();
        keyboard_announcement = text->hasFocus() && text->verticalScrollBar()->maximum() > 0 &&
            text->verticalScrollBar()->value() == text->verticalScrollBar()->maximum();
        qobject_cast<QDialog*>(reader)->reject();
    });
    auto* read = dialog.findChild<QPushButton*>("groupReadAnnouncementButton");
    check(!read->isHidden(), "A real announcement exposes its complete reading action");
    read->click();
    check(full_announcement, "Long announcements remain fully readable after the overview is bounded");
    check(keyboard_announcement, "The full announcement can be focused and read to its end with the keyboard");
    QList<member_data> many_members;
    for (qint64 user = 1; user <= 100; ++user)
    {
        many_members.push_back({user, QStringLiteral("member %1").arg(user),
            user == 1 ? chat::member_role::owner : chat::member_role::member, {}});
    }
    dialog.set_members(1, many_members, {});
    tabs->setCurrentIndex(1);
    dialog.resize(640, 760);
    dialog.activateWindow();
    auto* members = dialog.findChild<QListWidget*>("groupMembersList");
    members->setFocus();
    members->setCurrentRow(99);
    members->scrollToItem(members->currentItem());
    QApplication::processEvents();
    dialog.resize(420, 480);
    QApplication::processEvents();
    QApplication::processEvents();
    check(members->hasFocus() && members->viewport()->rect().contains(members->visualItemRect(members->currentItem())),
          "Resizing a focused member list keeps the selected member visible");
    dialog.set_members(1, {{1, "owner", chat::member_role::admin, {}},
                           {2, "admin", chat::member_role::owner, {}},
                           {3, "member", chat::member_role::member, {}}}, {});
    auto const promoted_names = QStringList{QStringLiteral("admin · 群主"),
                                           QStringLiteral("owner · 管理员 · 你"),
                                           QStringLiteral("member · 成员")};
    check_accessible_rows(members, promoted_names);
    check_accessible_rows(preview, promoted_names);
    members->setCurrentRow(1);
    dialog.set_requests(1, {{10, "first", false, 0, {}}}, 10, false, {});
    dialog.set_requests(1, {{11, "older", false, 0, {}}}, 0, true, {});
    check_accessible_rows(requests, {QStringLiteral("first · 待审批"), QStringLiteral("older · 待审批")});
    dialog.set_members(1, {{1, "owner", chat::member_role::member, {}},
                           {2, "admin", chat::member_role::owner, {}},
                           {3, "member", chat::member_role::member, {}}}, {});
    auto const downgraded_names = QStringList{QStringLiteral("admin · 群主"),
                                             QStringLiteral("owner · 成员 · 你"),
                                             QStringLiteral("member · 成员")};
    check_accessible_rows(members, downgraded_names);
    check_accessible_rows(preview, downgraded_names);
    check(members->currentItem() && members->currentItem()->data(Qt::UserRole).toLongLong() == 1,
          "A live role change preserves the selected member identity while updating accessible context");
    check_accessible_rows(requests, {});
    dialog.set_requests(1, {{12, "private stale applicant", false, 0, {}}}, 0, false, {});
    check_accessible_rows(requests, {});
    check(!tabs->isTabVisible(2) && !tabs->isTabVisible(3),
          "Management role loss hides private pages and stale application callbacks cannot restore accessible rows");
    std::cout << "PASS Qt group initial native row names and live role/status models without private stale rows\n";
    {
        group_dialog title_dialog(42, 1, QStringLiteral("原群名"), {}, false, nullptr);
        title_dialog.set_members(42, {{1, "owner", chat::member_role::owner, {}},
                                      {2, "admin", chat::member_role::admin, {}}}, {});
        auto* title = title_dialog.findChild<QLineEdit*>("groupTitleEdit");
        auto* rename = title_dialog.findChild<QPushButton*>("groupRenameButton");
        auto* overview = title_dialog.findChild<QLabel*>("groupOverviewTitle");
        conversation_data snapshot;
        snapshot.id = 42; snapshot.group = true; snapshot.username = QStringLiteral("远端干净更新");
        title_dialog.set_conversations({snapshot}, {});
        check(title->text() == snapshot.username && !rename->isEnabled() &&
              overview->text() == snapshot.username && title_dialog.windowTitle() == snapshot.username + QStringLiteral(" · 群资料"),
              "A clean group title follows the remote authoritative name and disables redundant saving");
        auto const draft = QStringLiteral("  本地 群 👩‍💻 é  ");
        title->setText(draft);
        for (auto const& remote : {QStringLiteral("另一管理员第一次改名"), QStringLiteral("另一管理员再次改名")})
        {
            snapshot.username = remote;
            title_dialog.set_conversations({snapshot}, {});
            check(title->text() == draft && rename->isEnabled() && overview->text() == remote &&
                  title_dialog.windowTitle() == remote + QStringLiteral(" · 群资料"),
                  "Consecutive remote renames preserve the exact Unicode/spaced local draft while updating authoritative identity");
        }
        auto foreign = snapshot; foreign.id = 99; foreign.username = QStringLiteral("其他会话更新");
        title_dialog.set_conversations({foreign, snapshot}, {});
        auto failed = snapshot; failed.username = QStringLiteral("失败请求中的群名");
        title_dialog.set_conversations({failed}, QStringLiteral("无法加载"));
        check(title->text() == draft && overview->text() == snapshot.username && rename->isEnabled(),
              "Other conversation data and failed snapshots cannot replace the current group title draft or authority");
        title->setText(snapshot.username);
        check(!rename->isEnabled(), "Restoring the latest authoritative name clears the title difference after dirty updates");
        snapshot.username = QStringLiteral("恢复干净后远端更新");
        title_dialog.set_conversations({snapshot}, {});
        check(title->text() == snapshot.username && !rename->isEnabled(),
              "A title made clean again resumes ordinary remote synchronization");
        title->setText(draft);
        int submitted = 0; QString submitted_title;
        QObject::connect(&title_dialog, &group_dialog::rename_requested, &title_dialog, [&](QString value) {
            ++submitted; submitted_title = std::move(value);
        });
        rename->click(); rename->click();
        check(submitted == 1 && submitted_title == draft && title->text() == draft &&
              !title->isEnabled() && !rename->isEnabled(),
              "Pending rename submits the exact draft once and keeps title editing and repeated saving disabled");
        snapshot.username = QStringLiteral("等待确认时远端更新");
        title_dialog.set_conversations({snapshot}, {});
        check(title->text() == draft && overview->text() == snapshot.username && !rename->isEnabled(),
              "A remote snapshot cannot erase a submitted draft or permit repeated submission while pending");
        title_dialog.finish_action(42, false, {});
        snapshot.username = draft;
        title_dialog.set_conversations({snapshot}, {});
        check(title->text() == draft && title->isEnabled() && !rename->isEnabled() && overview->text() == draft,
              "The successful authoritative acknowledgement makes the saved title clean without changing its original bytes");
        title->setText(QStringLiteral("失权前未提交群名"));
        title_dialog.set_members(42, {{1, "owner", chat::member_role::member, {}},
                                      {2, "admin", chat::member_role::owner, {}}}, {});
        rename->click();
        check(!title->isEnabled() && !rename->isEnabled() && submitted == 1,
              "Losing management permission keeps a title draft from being submitted through disabled controls");
        std::cout << "PASS Qt exact group title drafts survive remote updates and become clean after successful save\n";
    }
    {
        group_dialog private_dialog(43, 1, QStringLiteral("权限切换"), {}, false, nullptr);
        auto* private_tabs = private_dialog.findChild<QTabWidget*>("groupTabs");
        auto* private_requests = private_dialog.findChild<QListWidget*>("groupJoinRequestsList");
        auto* request_page = private_requests->parentWidget();
        auto* management_page = private_dialog.findChild<QScrollArea*>("groupManagementScroll");
        auto* private_members = private_dialog.findChild<QListWidget*>("groupMembersList");
        auto* manage = private_dialog.findChild<QPushButton*>("groupManageButton");
        auto* accept = private_dialog.findChild<QPushButton*>("groupAcceptRequestButton");
        auto* more = private_dialog.findChild<QPushButton*>("groupMoreRequestsButton");
        auto* request_status = private_dialog.findChild<QLabel*>("groupJoinRequestsStatus");
        auto* announcement = private_dialog.findChild<QPlainTextEdit*>("groupAnnouncementEdit");
        auto const authorized = QList<member_data>{{1, "self", chat::member_role::admin, {}},
                                                   {2, "owner", chat::member_role::owner, {}},
                                                   {3, "selected", chat::member_role::member, {}}};
        auto const ordinary = QList<member_data>{{1, "self", chat::member_role::member, {}},
                                                 {2, "owner", chat::member_role::owner, {}},
                                                 {3, "selected", chat::member_role::member, {}}};
        auto check_private_absent = [&] {
            check(private_tabs->count() == 2 && private_tabs->indexOf(request_page) == -1 &&
                  private_tabs->indexOf(management_page) == -1 && private_requests->count() == 0 &&
                  !accept->isEnabled() && !more->isEnabled() && manage->isHidden(),
                  "Unavailable or ordinary membership removes private pages, clears private rows and disables private actions");
        };
        check_private_absent();
        private_dialog.set_members(99, authorized, {});
        private_dialog.set_members(43, authorized, QStringLiteral("成员加载失败"));
        private_dialog.set_requests(43, {{10, "unavailable private applicant", false, 0, {}}}, 10, false, {});
        check_private_absent();
        int refreshes = 0;
        QObject::connect(&private_dialog, &group_dialog::requests_requested, &private_dialog,
                         [&](qint64 before) { check(before == 0, "Role restoration refreshes the first request page"); ++refreshes; });
        private_dialog.set_members(43, authorized, {});
        check(private_tabs->count() == 4 && private_tabs->widget(2) == request_page &&
              private_tabs->widget(3) == management_page && private_tabs->tabText(2) == QStringLiteral("入群申请 (0)") &&
              request_status->text() == QStringLiteral("正在加载申请…") && refreshes == 1,
              "Authoritative manager membership adds the original private pages in order with actual empty count and loading state");
        private_tabs->setCurrentIndex(0);
        manage->click();
        check(private_tabs->currentWidget() == management_page,
              "The overview management action selects its actual page identity");
        private_dialog.set_requests(43, {{10, "first", false, 0, {}}, {11, "selected applicant", false, 0, {}}}, 10, false, {});
        private_requests->setCurrentRow(1);
        private_dialog.set_requests(43, {{12, "older", false, 0, {}}}, 0, true, {});
        check(private_tabs->tabText(private_tabs->indexOf(request_page)) == QStringLiteral("入群申请 (3)") &&
              private_requests->currentItem()->data(Qt::UserRole).toLongLong() == 11 && accept->isEnabled(),
              "Authorized request pagination updates its real tab and preserves the selected applicant");
        auto const announcement_draft = QStringLiteral("加载失败前未保存公告");
        announcement->setPlainText(announcement_draft);
        private_dialog.set_members(43, authorized, QStringLiteral("已授权成员加载失败"));
        check_private_absent();
        check(announcement->toPlainText() == announcement_draft && announcement->isReadOnly(),
              "Member loading failure disables announcement editing without treating the old authoritative role as revoked or erasing its draft");
        private_dialog.set_requests(43, {{13, "late private applicant after error", false, 0, {}}}, 13, false, {});
        private_dialog.finish_action(43, false, {});
        check_private_absent();
        private_dialog.set_members(43, authorized, {});
        check(private_tabs->count() == 4 && private_tabs->widget(2) == request_page &&
              private_tabs->widget(3) == management_page && private_tabs->tabText(2) == QStringLiteral("入群申请 (0)") &&
              private_requests->count() == 0 && !accept->isEnabled() && !more->isEnabled() && refreshes == 2 &&
              announcement->toPlainText() == announcement_draft && !announcement->isReadOnly(),
              "Only a successful authoritative member snapshot restores private pages after availability loss despite the old authorized role");
        private_tabs->setCurrentIndex(1);
        private_members->setCurrentRow(2);
        private_dialog.set_members(43, ordinary, {});
        check_private_absent();
        check(private_tabs->currentWidget() == private_members->parentWidget() &&
              private_members->currentItem()->data(Qt::UserRole).toLongLong() == 3,
              "Removing private pages preserves the visible public member page and selected identity");
        private_dialog.set_requests(43, {{13, "stale private applicant", false, 0, {}}}, 13, false, {});
        private_dialog.set_members(99, authorized, {});
        private_dialog.set_members(43, authorized, QStringLiteral("旧权限加载失败"));
        check_private_absent();
        private_dialog.set_members(43, authorized, {});
        check(private_tabs->count() == 4 && private_tabs->widget(2) == request_page &&
              private_tabs->widget(3) == management_page && private_tabs->tabText(2) == QStringLiteral("入群申请 (0)") &&
              private_requests->count() == 0 && !accept->isEnabled() && !more->isEnabled() &&
              request_status->text() == QStringLiteral("正在加载申请…") && refreshes == 3 &&
              private_tabs->currentIndex() == 1 && private_members->currentItem()->data(Qt::UserRole).toLongLong() == 3,
              "Restored management reuses the original pages without stale private count, rows or lost public selection");
        private_dialog.set_requests(43, {{14, "fresh applicant", false, 0, {}}}, 14, false, {});
        private_requests->setCurrentRow(0);
        private_dialog.set_members(43, authorized, {});
        check(private_tabs->count() == 4 && private_tabs->tabText(2) == QStringLiteral("入群申请 (1+)") &&
              private_requests->count() == 1 && private_requests->currentItem()->data(Qt::UserRole).toLongLong() == 14 &&
              accept->isEnabled() && !more->isEnabled(),
              "Repeated authoritative manager updates do not duplicate pages or clear current rows, and refresh invalidates the old cursor");
        private_tabs->setCurrentWidget(request_page);
        private_dialog.set_members(43, ordinary, {});
        check_private_absent();
        check(private_tabs->currentIndex() >= 0 && private_tabs->currentIndex() < 2,
              "Revoking permission on the active private page leaves a valid public selection");
        std::cout << "PASS Qt private group tabs remove and restore by authoritative role without stale count or duplicate pages\n";
    }
    std::cout << "PASS Qt group overview live member avatar and bounded details\n";
}

void check_primary_navigation()
{
    chat_widget page;
    page.setStyleSheet(chat_style_sheet());
    page.resize(1180, 760);
    page.set_user(QStringLiteral("本人"), 1);
    page.set_connection_available(true);
    page.show();
    QApplication::processEvents();
    auto* navigation = page.findChild<QFrame*>("navigationPanel");
    auto buttons = navigation->findChildren<QToolButton*>();
    check(buttons.size() == 3, "Primary navigation is only Chats, Contacts and account avatar");
    check(page.findChild<QToolButton*>("profileAvatar")->accessibleName() == QStringLiteral("我的资料") &&
          page.findChild<QListView*>("conversationList")->accessibleName() == QStringLiteral("会话列表") &&
          page.findChild<QListView*>("messageList")->accessibleName() == QStringLiteral("消息记录"),
          "Account action, conversation list and message history have stable accessible purposes");
    auto* actions = page.findChild<QToolButton*>("chatsActionsButton");
    check(actions && actions->isVisible() && actions->menu(), "Chats header exposes a lightweight action menu");
    auto* add = page.findChild<QAction*>("addFriendAction");
    auto* create = page.findChild<QAction*>("createGroupAction");
    auto* join = page.findChild<QAction*>("joinGroupAction");
    check(add && create && join && actions->menu()->actions().size() == 3, "Header menu has three real actions");
    add->trigger();
    auto searches = page.findChildren<QLineEdit*>("userSearchEdit");
    check(std::any_of(searches.begin(), searches.end(), [](auto* field) {
        return field->isVisible() && field->placeholderText() == QStringLiteral("搜索用户");
    }), "Header add friend opens existing search");
    page.set_add_contact_search_results({{600, QStringLiteral("搜索 用户"), false, 0, {}}});
    QApplication::processEvents();
    auto search_lists = page.findChildren<QListView*>("userList");
    auto search_list = std::find_if(search_lists.begin(), search_lists.end(), [](auto* list) { return list->isVisible(); });
    check(search_list != search_lists.end(), "User search results are visible before clicking their avatar");
    check((*search_list)->accessibleName() == QStringLiteral("用户搜索结果") &&
          std::any_of(searches.begin(), searches.end(), [](auto* field) {
              return field->isVisible() && field->accessibleName() == QStringLiteral("搜索用户");
          }), "Add friend input and results expose their accessible purpose independently of placeholder text");
    auto const search_row = (*search_list)->visualRect((*search_list)->model()->index(0, 0));
    check_profile_avatar_click(*search_list, QPoint(20, search_row.center().y()));
    for (auto key : {Qt::Key_Return, Qt::Key_Enter})
    {
        int opened_profiles = 0;
        bool correct_profile = false;
        QTimer close_profile;
        QObject::connect(&close_profile, &QTimer::timeout, &page, [&] {
            auto* profile = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (profile && profile->objectName() == "profileDialog")
            {
                ++opened_profiles;
                correct_profile = profile->windowTitle() == QStringLiteral("搜索 用户");
                profile->reject();
            }
        });
        close_profile.start(0);
        (*search_list)->setFocus();
        QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
        QApplication::sendEvent(*search_list, &home);
        QKeyEvent activate(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(*search_list, &activate);
        close_profile.stop();
        check(opened_profiles == 1 && correct_profile, "User search keyboard activation opens exactly the current user's profile");
    }
    auto* back = page.findChild<QToolButton*>("sidebarHeaderButton");
    auto* chats = *std::find_if(buttons.begin(), buttons.end(), [](auto* button) { return button->text() == QStringLiteral("聊天"); });
    check(chats->objectName() == "navigationSelected", "Add friend from Chats preserves primary ownership");
    back->click();
    check(actions->isVisible() && !back->isVisible(), "Add friend Back returns to Chats");
    for (auto* button : buttons)
    { if (button->text() == QStringLiteral("联系人")) { button->click(); } }
    check(!actions->isVisible(), "Chats actions do not become another primary page");
    auto lists = page.findChildren<QListView*>("userList");
    auto selected_list = std::find_if(lists.begin(), lists.end(), [](auto* view) { return view->isVisible(); });
    check(selected_list != lists.end(), "Contacts list is visible");
    auto* contact_view = *selected_list;
    check(contact_view->accessibleName() == QStringLiteral("联系人列表") &&
          std::any_of(searches.begin(), searches.end(), [](auto* field) {
              return field->isVisible() && field->accessibleName() == QStringLiteral("搜索联系人");
          }), "Contacts input and list have accessible purposes distinct from user search");
    auto* add_contact = page.findChild<QToolButton*>("sidebarTextButton");
    add_contact->click();
    back->click();
    check(contact_view->isVisible() && !back->isVisible(), "Contacts Add friend Back returns to Contacts");
    auto* new_friends = page.findChild<QPushButton*>("newFriendsButton");
    new_friends->click();
    add_contact->click();
    back->click();
    check(page.findChild<QLabel*>("friendRequestsStatus")->isVisible() && back->isVisible(), "Add friend Back restores the empty New friends page");
    back->click();
    check(contact_view->isVisible() && !back->isVisible(), "New friends Back returns to Contacts");
    auto* incoming = page.findChild<QListWidget*>("incomingFriendRequests");
    auto* outgoing = page.findChild<QListWidget*>("outgoingFriendRequests");
    QList<user_data> contacts;
    for (int count : {0, 1, 20})
    {
        contacts.clear();
        for (int i = 0; i < count; ++i)
        { contacts.push_back({10+i, QStringLiteral("张 三😀").repeated(4) + QString::number(i), false, 0, {}}); }
        page.set_contacts(contacts);
        page.set_friend_requests({{200, QStringLiteral("收到 申请"), false, 0, {}}}, {{201, QStringLiteral("发出 申请"), false, 0, {}}}, {});
        check(contact_view->model()->rowCount() == count, "Only accepted contacts appear at 0/1/20 counts");
        check(incoming->count() == 1 && outgoing->count() == 1, "Pending requests remain separate from accepted contacts");
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(dialog && dialog->objectName() == "createGroupDialog", "Menu invokes existing two-step group flow");
            check(dialog->layout()->contentsMargins() == QMargins(24, 24, 24, 24) && dialog->layout()->spacing() == 12,
                "Create group shares the dialog content rhythm");
            auto* list = dialog->findChild<QListWidget*>("groupContactPicker");
            auto* next = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
            auto* cancel = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel);
            check(next->icon().isNull() && cancel->icon().isNull() && cancel->text() == QStringLiteral("取消"),
                "Create group actions have no platform icons");
            check(list->count() == count && !next->isEnabled(), "Group picker only contains accepted contacts and requires selection");
            if (count > 0)
            {
                list->item(0)->setCheckState(Qt::Checked);
                auto const pixels = dialog->grab().toImage();
                auto const primary = logical_pixel(pixels, next->mapTo(dialog, QPoint(next->width() - 12, next->height() / 2)));
                check(primary.lightness() < 100, "Enabled group progression is visibly primary");
                auto* chips = dialog->findChild<QListWidget*>("groupSelectedContacts");
                check(chips->count() == 1 && chips->item(0)->text().contains(contacts[0].username), "Long Unicode selection remains intact");
                if (count == 20)
                { for (int row = 1; row < list->count(); ++row) { list->item(row)->setCheckState(Qt::Checked); } }
                next->click();
                QApplication::processEvents();
                check(dialog->findChild<QLineEdit*>("newGroupTitleEdit")->isVisible(), "Selected friends precede group title step");
                check(dialog->height() <= 380, "Naming one selected friend does not retain the tall picker canvas");
                check(dialog->findChild<QLineEdit*>("newGroupTitleEdit")->hasFocus(), "Naming starts at the title input");
                if (count == 20)
                {
                    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
                    QApplication::sendEvent(dialog->findChild<QLineEdit*>("newGroupTitleEdit"), &tab);
                    auto* members = dialog->findChild<QListWidget*>("groupNamingMembers");
                    check(members->hasFocus() && members->count() == 20, "Keyboard users can reach all selected group members");
                    QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
                    QApplication::sendEvent(members, &end);
                    check(members->verticalScrollBar()->value() > 0, "The bounded member summary can scroll by keyboard");
                }
            }
            dialog->reject();
        });
        create->trigger();
    }
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        check(dialog && dialog->objectName() == "joinGroupDialog", "Join group keeps a focused link input");
        check(dialog->width() >= 460 && dialog->okButtonText() == QStringLiteral("加入") &&
            dialog->cancelButtonText() == QStringLiteral("取消"),
            QStringLiteral("Join group uses a readable width and named actions: %1 / %2 / %3")
                .arg(dialog->width()).arg(dialog->okButtonText()).arg(dialog->cancelButtonText()).toUtf8().constData());
        for (auto* button : dialog->findChildren<QPushButton*>())
        { check(button->icon().isNull(), "Join group has no platform action icons"); }
        dialog->reject();
    });
    page.findChild<QAction*>("joinGroupAction")->trigger();
    page.set_contacts({{10, QStringLiteral("Alice Bob"), false, 0, {}}, {11, QStringLiteral("张 三"), false, 0, {}},
                       {12, QStringLiteral("a.b"), false, 0, {}}, {13, QStringLiteral("Älice"), false, 0, {}}});
    QApplication::processEvents();
    int direct_requests = 0;
    qint64 direct_user = 0;
    auto direct_connection = QObject::connect(&page, &chat_widget::direct_conversation_requested, &page,
        [&](qint64 user, QString) { ++direct_requests; direct_user = user; });
    auto const contact_row = contact_view->visualRect(contact_view->model()->index(0, 0));
    check_profile_avatar_click(contact_view, QPoint(20, contact_row.center().y()));
    check(direct_requests == 0, "Opening a contact avatar profile does not also open its chat after close");
    auto const contact_point = QPoint(80, contact_row.center().y());
    QMouseEvent contact_press(QEvent::MouseButtonPress, contact_point, contact_view->viewport()->mapToGlobal(contact_point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent contact_release(QEvent::MouseButtonRelease, contact_point, contact_view->viewport()->mapToGlobal(contact_point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(contact_view->viewport(), &contact_press);
    QApplication::sendEvent(contact_view->viewport(), &contact_release);
    check(direct_requests == 1, "A contact row body still opens its chat with one click");
    contact_view->setFocus();
    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
    QKeyEvent keypad_enter(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier);
    QApplication::sendEvent(contact_view, &home);
    QApplication::sendEvent(contact_view, &down);
    QApplication::sendEvent(contact_view, &enter);
    check(direct_requests == 2 && direct_user == 11, "Contacts Enter opens exactly the current accepted contact");
    QApplication::sendEvent(contact_view, &end);
    QApplication::sendEvent(contact_view, &keypad_enter);
    check(direct_requests == 3 && direct_user == 13, "Contacts keypad Enter opens exactly the current accepted contact");
    QObject::disconnect(direct_connection);
    conversation_data direct;
    direct.id = 91; direct.user = 10; direct.username = QStringLiteral("Alice Bob");
    auto other = direct;
    other.id = 92; other.user = 11; other.username = QStringLiteral("张 三");
    page.set_conversations({direct, other});
    chats->click();
    QApplication::processEvents();
    auto* conversations = page.findChild<QListView*>("conversationList");
    click_list_body(conversations, conversations->model()->index(0, 0));
    int chat_opens = 0;
    qint64 opened_conversation = 0;
    auto chat_connection = QObject::connect(&page, &chat_widget::conversation_selected, &page,
        [&](qint64 conversation, bool) { ++chat_opens; opened_conversation = conversation; });
    auto const conversation_row = conversations->visualRect(conversations->model()->index(1, 0));
    check_profile_avatar_click(conversations, QPoint(20, conversation_row.center().y()));
    check(chat_opens == 0, "Opening a direct conversation avatar profile does not also select its chat after close");
    click_list_body(conversations, conversations->model()->index(1, 0));
    check(chat_opens == 1, "A direct conversation row body still selects its chat with one click");
    conversations->setFocus();
    QApplication::sendEvent(conversations, &home);
    QApplication::sendEvent(conversations, &enter);
    check(chat_opens == 2 && opened_conversation == 91, "Chats Enter opens exactly the current conversation");
    QApplication::sendEvent(conversations, &end);
    QApplication::sendEvent(conversations, &keypad_enter);
    check(chat_opens == 3 && opened_conversation == 92, "Chats keypad Enter opens exactly the current conversation");
    QObject::disconnect(chat_connection);
    for (auto* button : buttons)
    { if (button->text() == QStringLiteral("联系人")) { button->click(); } }
    auto* contact_search = *std::find_if(searches.begin(), searches.end(), [](auto* field) {
        return field->placeholderText() == QStringLiteral("搜索联系人");
    });
    for (auto const& [query, id] : QList<QPair<QString, qint64>>{{"BOB", 10}, {QStringLiteral("三"), 11}, {".b", 12}, {QStringLiteral("äli"), 13}})
    {
        contact_search->setText(query);
        check(contact_view->model()->rowCount() == 1 && contact_view->model()->index(0, 0).data(Qt::UserRole + 1).toLongLong() == id,
              ("Contacts substring match: " + query.toStdString() + " rows=" + std::to_string(contact_view->model()->rowCount()) +
               " id=" + std::to_string(contact_view->model()->index(0, 0).data(Qt::UserRole + 1).toLongLong())).c_str());
        check(new_friends->isVisible(), "Local search never filters the New friends entry");
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            auto* search = dialog->findChild<QLineEdit*>("groupContactSearch");
            auto* list = dialog->findChild<QListWidget*>("groupContactPicker");
            search->setText(query);
            int visible = 0;
            for (int row = 0; row < list->count(); ++row)
            {
                if (!list->item(row)->isHidden())
                {
                    ++visible;
                    check(list->item(row)->data(Qt::UserRole).toLongLong() == id, "Group picker matches the same accepted friend");
                }
            }
            check(visible == 1, "Group picker and Contacts share matching semantics");
            dialog->reject();
        });
        create->trigger();
    }
    contact_search->clear();
    check(contact_view->model()->rowCount() == 4, "Cleared Contacts filter restores accepted list");
    direct = {};
    direct.id = 50; direct.user = 200; direct.username = QStringLiteral("收到 申请"); direct.can_send = false;
    page.open_conversation(direct);
    auto* edit = page.findChild<QPlainTextEdit*>("messageEdit");
    page.set_friend_requests({}, {}, {});
    check(!edit->isEnabled() && edit->placeholderText().contains(QStringLiteral("添加好友")), "Read-only history distinguishes no friendship");
    page.set_friend_requests({}, {{200, direct.username, false, 0, {}}}, {});
    check(!edit->isEnabled() && edit->placeholderText().contains(QStringLiteral("等待对方")), "Read-only history distinguishes outgoing pending");
    page.set_friend_requests({{200, direct.username, false, 0, {}}}, {}, {});
    check(!edit->isEnabled() && edit->placeholderText().contains(QStringLiteral("待处理")), "Read-only history distinguishes incoming pending");
    page.set_connection_available(false);
    check(!actions->isEnabled() && !add->isEnabled() && !create->isEnabled() && !join->isEnabled(), "Offline header actions cannot issue requests");
    {
        QMenu menu(&page);
        auto* reply = menu.addAction(QStringLiteral("回复"));
        auto* reactions = menu.addMenu(QStringLiteral("表情回应"));
        reactions->addAction(QStringLiteral("👍"));
        menu.addSeparator();
        menu.addAction(QStringLiteral("删除"));
        menu.popup(page.mapToGlobal(QPoint(430, 90)));
        QApplication::processEvents();
        auto const background = logical_pixel(menu.grab().toImage(), QPoint(8, 8));
        check(background.alpha() == 255 && background.lightness() > 200,
              "Popup menu paints an opaque light background instead of transparent black");
        menu.setActiveAction(reply);
        QApplication::processEvents();
        auto const row = menu.actionGeometry(reply);
        auto const selected = logical_pixel(menu.grab().toImage(), QPoint(row.right() - 8, row.center().y()));
        check(selected.alpha() == 255 && selected.lightness() < 150,
              "Focused popup action has a distinct opaque selection");
        reactions->popup(menu.mapToGlobal(QPoint(menu.width(), 0)));
        QApplication::processEvents();
        auto const submenu = logical_pixel(reactions->grab().toImage(), QPoint(8, 8));
        check(submenu.alpha() == 255 && submenu.lightness() > 200,
              "Reaction submenu shares the readable popup surface");
        reactions->close();
        menu.close();
    }
    std::cout << "PASS Qt primary navigation, action menu, accepted contacts and pending history states\n";
}

void check_profile_layout()
{
    chat_widget page;
    page.setStyleSheet(chat_style_sheet());
    page.resize(980, 640);
    auto const username = QStringLiteral("测试用户").repeated(5) + QStringLiteral("😀");
    page.set_user(username, 1);
    page.set_connection_available(true);
    page.show();
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        check(dialog && dialog->objectName() == "profileDialog", "Own account profile is visible");
        int identities = 0;
        for (auto* label : dialog->findChildren<QLabel*>())
        { if (label->isVisible() && label->text() == username) { ++identities; } }
        check(identities == 1, "Profile presents the user identity once instead of repeating it in a second section");
        check(dialog->width() <= 520 && dialog->height() <= 600, "Account profile fits the normal dialog width and a minimum-height desktop");
        check(!dialog->findChild<QToolButton*>("profileCloseButton")->accessibleName().isEmpty(),
              "The icon-only profile close control has an accessible name");
        for (auto* action : dialog->findChildren<QToolButton*>("profileActionButton"))
        { check(!action->isVisible() || action->height() <= 48, "Profile actions use compact desktop buttons rather than oversized tiles"); }
        auto* change = dialog->findChild<QPushButton*>("changeAvatarButton");
        auto* logout = dialog->findChild<QPushButton*>("profileLogoutButton");
        auto const profile_pixels = dialog->grab().toImage();
        auto const change_background = logical_pixel(profile_pixels, change->mapTo(dialog, QPoint(change->width() - 12, change->height() / 2)));
        auto const logout_background = logical_pixel(profile_pixels, logout->mapTo(dialog, QPoint(logout->width() - 12, logout->height() / 2)));
        check(change_background.lightness() < 100 && logout_background.lightness() > 200,
              ("Account action hierarchy: change=" + change_background.name().toStdString() +
               " logout=" + logout_background.name().toStdString()).c_str());
        dialog->reject();
    });
    page.show_user_details(1, username);
    auto const contact_name = QStringLiteral("朋友 用户😀");
    page.set_contacts({{2, contact_name, false, 0, {}}});
    page.set_presence({2, true, 0});
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto* relationship = dialog->findChild<QLabel*>("profileRelationship");
        check(relationship && relationship->text() == QStringLiteral("在线"), "Contact profile shows existing authoritative presence");
        page.set_presence({2, false, 1710000000000});
        check(relationship->text().startsWith(QStringLiteral("最后上线于")), "An open contact profile updates presence without reopening");
        page.set_contacts({});
        check(relationship->text() == QStringLiteral("添加好友后可发送消息"), "Removed friendship profile no longer shows private presence");
        dialog->reject();
    });
    page.show_user_details(2, contact_name);
    std::cout << "PASS Qt profile identity hierarchy\n";
}

void check_confirmation_dialogs()
{
    QWidget parent;
    parent.setStyleSheet(chat_style_sheet());
    parent.show();
    for (auto const& action : {QStringLiteral("删除"), QStringLiteral("移除联系人"), QStringLiteral("退出登录"),
                              QStringLiteral("转让群主"), QStringLiteral("移除成员"), QStringLiteral("退出群聊")})
    {
        for (int choice = 0; choice < 4; ++choice)
        {
            QTimer::singleShot(0, [&] {
                auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                check(dialog && dialog->objectName() == "confirmationDialog", "Shared confirmation remains a real message box");
                check(dialog->icon() == QMessageBox::NoIcon && dialog->textFormat() == Qt::PlainText,
                    "Confirmation does not add a platform icon or interpret user identity as markup");
                check(dialog->button(QMessageBox::Yes)->text() == action &&
                    dialog->button(QMessageBox::No)->text() == QStringLiteral("取消"), "Confirmation names both decisions");
                check(dialog->defaultButton() == dialog->button(QMessageBox::No) &&
                    dialog->escapeButton() == dialog->button(QMessageBox::No), "Dangerous actions default to cancellation");
                for (auto* button : dialog->buttons()) { check(button->icon().isNull(), "Confirmation buttons have no platform icons"); }
                if (choice < 2)
                {
                    QKeyEvent key(QEvent::KeyPress, choice == 0 ? Qt::Key_Return : Qt::Key_Escape, Qt::NoModifier);
                    QApplication::sendEvent(dialog, &key);
                }
                else if (choice == 2) { dialog->close(); }
                else { dialog->button(QMessageBox::Yes)->click(); }
            });
            check(confirm_action(&parent, action, QStringLiteral("测试用户 <b>张三😀</b> 的操作后果"), action) == (choice == 3),
                "Enter, Escape and closing cancel; only an explicit positive selection confirms");
        }
    }
    std::cout << "PASS Qt named confirmation actions and safe keyboard defaults\n";
}

void check_message_editor()
{
    chat_widget page;
    page.setStyleSheet(chat_style_sheet());
    page.resize(980, 640);
    page.set_user(QStringLiteral("本人"), 1);
    page.set_connection_available(true);
    conversation_data direct;
    direct.id = 50; direct.user = 2; direct.username = QStringLiteral("朋友"); direct.can_send = true;
    page.open_conversation(direct);
    message_data message;
    message.id = 7; message.conversation = direct.id; message.from = 1; message.username = QStringLiteral("本人");
    message.text = QStringLiteral("原始中文 0123456789 é ❤︎ ❤️ 1️⃣7️⃣#️⃣*️⃣ 👩‍💻 👨‍👩‍👧‍👦 👍️🏽 \U0001faff A🏽\n").repeated(24);
    page.set_messages(direct.id, {message}, {}, false, false, false);
    page.show();
    QApplication::processEvents();
    auto* view = page.findChild<QListView*>("messageList");
    int edits = 0;
    QString submitted;
    QObject::connect(&page, &chat_widget::edit_message_requested, &page,
        [&](qint64 conversation, qint64 id, QString text) {
            check(conversation == direct.id && id == message.id, "Message edit retains its original target");
            ++edits;
            submitted = std::move(text);
        });
    for (int choice = 0; choice < 7; ++choice)
    {
        if (choice == 6)
        {
            page.open_conversation(direct);
            page.set_messages(direct.id, {message}, {}, false, false, false);
        }
        auto const edits_before = edits;
        auto const index = view->model()->index(choice == 5 ? 1 : 0, 0);
        view->scrollTo(index);
        QApplication::processEvents();
        QTimer inspect;
        QObject::connect(&inspect, &QTimer::timeout, &page, [&] {
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            if (!dialog) { return; }
            inspect.stop();
            dialog->activateWindow();
            wait([&] { return dialog->isActiveWindow(); });
            check(dialog->objectName() == "editMessageDialog", "Message editing has a named real input dialog");
            check(dialog->layout()->contentsMargins() == QMargins(24, 24, 24, 24) && dialog->layout()->spacing() == 12,
                  "Message editing uses shared dialog spacing");
            check(dialog->width() == chat_theme::dialog_normal_width, "Message editing has the normal dialog width");
            auto* editor = dialog->findChild<QPlainTextEdit*>("editMessageText");
            check(editor && editor->toPlainText() == message.text && !editor->accessibleName().isEmpty(),
                  "Message editing preserves the complete original Unicode body");
            check(editor->lineWrapMode() == QPlainTextEdit::WidgetWidth && editor->tabChangesFocus(),
                  "Message editing wraps text and uses Tab for focus rather than replacing the selection");
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            auto* save = buttons->button(QDialogButtonBox::Ok);
            check(save->text() == QStringLiteral("保存") && buttons->button(QDialogButtonBox::Cancel)->text() == QStringLiteral("取消"),
                  "Message editing names both decisions");
            for (auto* button : buttons->buttons()) { check(button->icon().isNull(), "Message editing has no platform button icons"); }
            check(logical_pixel(save->grab().toImage(), QPoint(save->width() / 2, 4)) == QColor("#315A4B"),
                  "Message save is visibly the primary action");
            editor->setFocus();
            auto editor_image = editor->grab().toImage();
            check(editor_image.pixelColor(0, editor_image.height() / 2) == QColor("#547C68"),
                  "Message editing has a visible input focus boundary");
            QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
            QApplication::sendEvent(editor, &tab);
            check(QApplication::focusWidget() == save && editor->toPlainText() == message.text,
                  "Tab reaches Save without mutating the body");
            editor_image = editor->grab().toImage();
            check(editor_image.pixelColor(0, editor_image.height() / 2) == QColor("#DDD9D0"),
                  "Message editing retains its unfocused input boundary");
            if (choice == 0)
            {
                check_preedit_display(editor);
                editor->setFocus();
                QApplication::processEvents();
                check(!editor->document()->isUndoAvailable(),
                      "Display highlighting does not create an initial message-edit undo command");
                editor->selectAll();
                editor->copy();
                check(QApplication::clipboard()->text().toUtf8() == message.text.toUtf8(),
                      "Copying from the real message editor preserves exact Unicode UTF-8");
                editor->moveCursor(QTextCursor::End);
                QString const suffix = QStringLiteral("追加 é 👍️🏽");
                QApplication::clipboard()->setText(suffix);
                editor->paste();
                QApplication::processEvents();
                check(editor->toPlainText().toUtf8() == (message.text + suffix).toUtf8(),
                      "Message-edit insertion changes only the exact pasted body");
                editor->undo();
                QApplication::processEvents();
                check(editor->toPlainText().toUtf8() == message.text.toUtf8() && !editor->document()->isUndoAvailable(),
                      "One message-edit undo restores the original body without a display-format undo step");
                editor->redo();
                check(editor->toPlainText().toUtf8() == (message.text + suffix).toUtf8(),
                      "Message-edit redo restores exact pasted Unicode");
                editor->undo();
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(dialog, &escape);
            }
            else
            {
                editor->setPlainText(choice == 1 ? QString{} : choice == 2 ? QStringLiteral(" \n\t　") : QStringLiteral("已编辑中文 🙂\n第二行 é"));
                if (choice == 4)
                {
                    auto older = message;
                    older.id = 6;
                    older.text = QStringLiteral("此前的另一条消息");
                    page.set_messages(direct.id, {older}, {}, true, false, false);
                }
                if (choice == 5)
                {
                    auto other = direct;
                    other.id = 60;
                    page.open_conversation(other);
                }
                save->click();
            }
        });
        inspect.start(0);
        QTimer::singleShot(0, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            check(menu, "Message edit is reached through the actual context menu");
            QAction* edit = nullptr;
            for (auto* action : menu->actions()) { if (action->text() == QStringLiteral("编辑")) { edit = action; } }
            check(edit, "An own text message has an edit action");
            if (choice == 6)
            {
                auto older = message;
                older.id = 6;
                older.text = QStringLiteral("菜单打开后载入的另一条消息");
                page.set_messages(direct.id, {older}, {}, true, false, false);
            }
            menu->setActiveAction(edit);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &enter);
        });
        view->customContextMenuRequested(view->visualRect(index).intersected(view->viewport()->rect()).center());
        check(edits == edits_before + (choice >= 2 && choice != 5),
              "Escape, an empty body and a changed conversation do not submit an edit");
        if (choice >= 2)
        {
            check(submitted == (choice == 2 ? QStringLiteral(" \n\t　") : QStringLiteral("已编辑中文 🙂\n第二行 é")),
                  "Nonempty whitespace and multiline Unicode keep their existing exact save semantics");
        }
    }
    std::cout << "PASS Qt message edit layout, keyboard and exact body semantics\n";
}

void check_reply_and_read_details_controls()
{
    chat_widget page;
    page.setStyleSheet(chat_style_sheet());
    page.resize(980, 640);
    page.set_user(QStringLiteral("本人"), 1);
    page.set_connection_available(true);
    conversation_data conversation;
    conversation.id = 50; conversation.group = true; conversation.can_send = true;
    page.open_conversation(conversation);
    message_data message;
    message.id = 7; message.conversation = 50; message.from = 1;
    message.username = QStringLiteral("本人");
    message.text = QString(79, QLatin1Char('x')) + QStringLiteral("👩‍💻尾行");
    read_positions positions;
    QList<member_data> members;
    member_data self; self.id = 1; self.username = QStringLiteral("本人"); members.push_back(self);
    QString const long_name = QStringLiteral("长读者姓名 中文 é 👩‍💻 ").repeated(12);
    for (qint64 id = 2; id <= 24; ++id)
    {
        member_data member; member.id = id;
        member.username = id == 3 ? long_name : QStringLiteral("读者 %1").arg(id);
        members.push_back(member); positions.insert(id, 7);
    }
    page.set_messages(50, {message}, positions, false, false, false);
    page.set_members(50, members, {});
    page.show(); page.activateWindow();
    wait([&] { return page.isActiveWindow(); });
    auto* view = page.findChild<QListView*>("messageList");
    auto open_action = [&](QString const& name) {
        QTimer::singleShot(0, &page, [name] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            check(menu, "Read and reply controls are reached through the real message menu");
            QAction* chosen = nullptr;
            for (auto* action : menu->actions()) { if (action->text() == name) { chosen = action; } }
            check(chosen, "The requested message control exists");
            menu->setActiveAction(chosen);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &enter);
        });
        view->customContextMenuRequested(view->visualRect(view->model()->index(0, 0)).center());
    };
    open_action(QStringLiteral("回复"));
    page.activateWindow();
    wait([&] { return page.isActiveWindow(); });
    auto* cancel = page.findChild<QToolButton*>("cancelReplyButton");
    auto* editor = page.findChild<QPlainTextEdit*>("messageEdit");
    QString const draft = QStringLiteral("保留草稿 中文 é 👩‍💻\n第二行");
    editor->setPlainText(draft);
    QApplication::processEvents();
    auto* reply_preview = page.findChild<QLabel*>("replyPreview");
    auto const prefix = QString(79, QLatin1Char('x'));
    check(reply_preview && reply_preview->text() == QStringLiteral("回复 本人：") + prefix &&
              reply_preview->textFormat() == Qt::PlainText,
          "The real reply label retains plain text and never splits a ZWJ cluster at the 80-unit limit");
    for (auto const& cluster : {QStringLiteral("é"), QStringLiteral("👍️🏽"), QStringLiteral("👨‍👩‍👧‍👦")})
    {
        message.text = prefix + cluster + QStringLiteral("尾行");
        ++message.edited_at;
        page.update_message(message);
        check(reply_preview->text() == QStringLiteral("回复 本人：") + prefix && editor->toPlainText() == draft &&
                  view->model()->index(0, 0).data(message_model::text_role).toString().toUtf8() == message.text.toUtf8(),
              "Updated reply previews omit an entire boundary cluster while retaining the complete body and draft");
    }
    message.text = QString(78, QLatin1Char('x')) + QStringLiteral("é尾行");
    ++message.edited_at;
    page.update_message(message);
    check(reply_preview->text() == QStringLiteral("回复 本人：") + QString(78, QLatin1Char('x')) + QStringLiteral("é"),
          "A complete combining cluster ending exactly at the reply limit remains visible");
    check(cancel->isVisible() && cancel->size() == QSize(36, 36) && !cancel->icon().isNull(),
          "Reply cancellation has the shared close icon and a usable hit box");
    check(cancel->accessibleName() == QStringLiteral("取消回复") && cancel->toolTip() == QStringLiteral("取消回复"),
          "Reply cancellation has a meaningful Chinese accessible name and tooltip");
    editor->setFocus();
    wait([&] { return editor->hasFocus(); });
    QKeyEvent backtab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
    QApplication::sendEvent(editor, &backtab);
    check(QApplication::focusWidget() == cancel && editor->toPlainText() == draft,
          "Keyboard focus reaches reply cancellation without changing the draft");
    auto const cancel_image = cancel->grab().toImage();
    editor->setFocus();QApplication::processEvents();
    auto const unfocused_image = cancel->grab().toImage();
    QApplication::sendEvent(editor, &backtab);QApplication::processEvents();
    int focus_pixels = 0;
    int unfocused_pixels = 0;
    for (int y = cancel_image.height() / 4; y < 3 * cancel_image.height() / 4; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            auto const color = cancel_image.pixelColor(x, y);
            if (color.rgb() == QColor("#547C68").rgb() && color.alpha() >= 200) { ++focus_pixels; }
            auto const unfocused_color = unfocused_image.pixelColor(x, y);
            if (unfocused_color.rgb() == QColor("#547C68").rgb() && unfocused_color.alpha() >= 200) { ++unfocused_pixels; }
        }
    }
    check(focus_pixels > 0 && unfocused_pixels == 0,
          "Reply cancellation has a visible keyboard focus boundary");
    int sent = 0; qint64 reply = -1;
    QObject::connect(&page, &chat_widget::send_message_requested, &page,
        [&](qint64 conversation_id, QString text, qint64 reply_id) {
            check(conversation_id == 50 && text == draft, "Cancelling reply preserves the original draft and conversation");
            ++sent; reply = reply_id;
        });
    QKeyEvent space_press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent space_release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(cancel, &space_press); QApplication::sendEvent(cancel, &space_release);
    check(!page.findChild<QLabel*>("replyPreview")->isVisible() && editor->toPlainText() == draft && editor->hasFocus(),
          "Space cancels the reply and returns focus to the unchanged draft");
    page.findChild<QToolButton*>("sendButton")->click();
    check(sent == 1 && reply == 0, "A cancelled reply sends as an ordinary message");

    bool inspected = false, timed_out = false;
    QTimer inspect;
    QObject::connect(&inspect, &QTimer::timeout, &page, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != QStringLiteral("readDetailsDialog")) { return; }
        inspect.stop(); inspected = true; dialog->activateWindow();
        wait([&] { return dialog->isActiveWindow(); });
        auto* list = dialog->findChild<QListWidget*>("readMembersList");
        check(list && qobject_cast<user_delegate*>(list->itemDelegate()) && list->count() == 23,
              "Read details use the existing user delegate and the actual readers");
        check(list->accessibleName() == QStringLiteral("已读成员") &&
                  list->item(1)->text() == long_name && list->item(1)->toolTip() == long_name &&
                  list->item(1)->data(Qt::StatusTipRole).toString() == QStringLiteral("已读"),
              "Read details retain long Unicode names and expose their complete tooltip without invented presence");
        auto const mouse_point = list->visualItemRect(list->item(2)).center();
        QMouseEvent mouse_press(QEvent::MouseButtonPress, mouse_point, list->viewport()->mapToGlobal(mouse_point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent mouse_release(QEvent::MouseButtonRelease, mouse_point, list->viewport()->mapToGlobal(mouse_point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &mouse_press);
        QApplication::sendEvent(list->viewport(), &mouse_release);
        check(list->currentItem() && list->currentItem()->data(Qt::UserRole).toLongLong() == 4,
              "The existing user delegate preserves real mouse row selection without profile handlers");
        list->setFocus(); list->setCurrentRow(0);
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(list, &down);
        check(list->currentItem()->data(Qt::UserRole).toLongLong() == 3,
              "Read members support real keyboard row navigation");
        auto const row_rect = list->visualItemRect(list->currentItem());
        check(row_rect.height() == chat_theme::dialog_row_height, "Read members use shared user row metrics");
        auto const list_image = list->viewport()->grab().toImage();
        check(logical_pixel(list_image, QPoint(row_rect.right() - 6, row_rect.bottom() - 6)) == QColor("#E7EEE9"),
              "The selected read member uses the cream and green user-list palette");
        list->setCurrentRow(13);
        list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum() / 2);
        auto const selected_id = list->currentItem()->data(Qt::UserRole).toLongLong();
        auto const scroll = list->verticalScrollBar()->value();
        check(selected_id == 15 && scroll > 0, "Refresh fixture has a selected reader and a nonzero scroll position");
        page.findChild<avatar_cache*>()->changed(15);
        check(list->currentItem() && list->currentItem()->data(Qt::UserRole).toLongLong() == selected_id &&
                  list->verticalScrollBar()->value() == scroll && list->hasFocus(),
              "Avatar refresh retains reader identity, scroll position and keyboard focus");
        page.set_read_message(50, 2, 8);
        check(list->currentItem() && list->currentItem()->data(Qt::UserRole).toLongLong() == selected_id &&
                  list->verticalScrollBar()->value() == scroll,
              "Read-receipt refresh retains the selected reader and scroll position");
        members.removeIf([selected_id](member_data const& member) { return member.id == selected_id; });
        page.set_members(50, members, {});
        check(!list->currentItem() && list->selectedItems().isEmpty(),
              "Removing the selected reader does not silently select a different reader");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(dialog, &escape);
    });
    inspect.start(0);
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &page, [&] {
        timed_out = true;
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) { dialog->reject(); }
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) { menu->close(); }
    });
    watchdog.start(5000);
    open_action(QStringLiteral("已读详情"));
    inspect.stop(); watchdog.stop();
    check(inspected && !timed_out, "Read-control inspection completed through the real modal dialog");
    std::cout << "PASS Qt reply cancellation and read-member controls\n";
}

QList<QPair<QString, QString>> unicode_boundary_samples()
{
    return {{QStringLiteral("surrogate"), QStringLiteral("🙂")},
            {QStringLiteral("zwj"), QStringLiteral("👩‍💻")},
            {QStringLiteral("combining"), QStringLiteral("é")},
            {QStringLiteral("skin"), QStringLiteral("👍️🏽")},
            {QStringLiteral("keycap"), QStringLiteral("1️⃣")}};
}

bool check_pinned_unicode_boundaries(QString const& artifact_directory = {})
{
    chat_widget page;
    page.setStyleSheet(chat_style_sheet());
    page.resize(1180, 760);
    page.set_user(QStringLiteral("本人"), 1);
    page.set_connection_available(true);
    conversation_data group;
    group.id = 50; group.group = true; group.can_send = true;
    group.username = QStringLiteral("置顶边界群");
    group.pinned_message.id = 7;
    group.pinned_message.username = QStringLiteral("发布者&本人");
    page.open_conversation(group);
    page.show();
    QApplication::processEvents();
    auto* button = page.findChild<QPushButton*>("pinnedMessageButton");
    check(button, "Pinned Unicode contract uses the real pinned-message button");
    QString query;
    int searches = 0;
    QObject::connect(&page, &chat_widget::message_search_requested, &page,
        [&](qint64 conversation, qint64 user, bool is_group, QString title, QString value) {
            check(conversation == 50 && user == 1 && is_group && title == group.username,
                  "Older pinned-message lookup retains its actual conversation and actor");
            query = std::move(value); ++searches;
        });
    bool intact = true;
    QString const prefix(79, QLatin1Char('x'));
    QString const summary = QStringLiteral("置顶消息 · 发布者&本人：") + prefix;
    for (auto const& [name, cluster] : unicode_boundary_samples())
    {
        group.pinned_message.text = QStringLiteral("\n  \n") + prefix + cluster + QStringLiteral("尾行\n第二行");
        page.set_conversations({group});
        QApplication::processEvents();
        check(button->isVisible() && page.conversation(50)->pinned_message.text.toUtf8() == group.pinned_message.text.toUtf8(),
              "Pinned display truncation leaves the complete original multiline Unicode untouched");
        bool const summary_ok = button->text() == QString(summary).replace(QLatin1Char('&'), QStringLiteral("&&")) &&
            button->toolTip() == summary + QStringLiteral("\n点击定位；较早的消息通过搜索查看。");
        if (!artifact_directory.isEmpty())
        {
            check(page.grab().save(artifact_directory + QStringLiteral("/qt_pinned_unicode_") + name + QStringLiteral(".png")),
                  "The real pinned Unicode widget has an original capture");
        }
        int const previous_searches = searches;
        button->click();
        bool const query_ok = searches == previous_searches + 1 && query == prefix;
        std::cout << (summary_ok && query_ok ? "PASS" : "RED") << " Qt pinned Unicode " << name.toStdString()
                  << " summary=" << summary_ok << " first_line=" << query_ok << '\n';
        intact = intact && summary_ok && query_ok;
    }
    for (auto const width : {980, 1180, 1440})
    {
        page.resize(width, 760);
        QApplication::processEvents();
        QStyleOptionButton option;
        option.initFrom(button);
        option.features = QStyleOptionButton::Flat;
        auto const contents = button->style()->subElementRect(QStyle::SE_PushButtonContents, &option, button);
        auto const actual = button->grab().toImage();
        QImage prefix(actual.size(), QImage::Format_ARGB32_Premultiplied);
        prefix.setDevicePixelRatio(actual.devicePixelRatio());
        prefix.fill(Qt::transparent);
        QTextLayout layout(QStringLiteral("置顶消息"), button->font());
        layout.beginLayout();
        auto line = layout.createLine();
        line.setLineWidth(contents.width());
        layout.endLayout();
        QPainter painter(&prefix);
        painter.setPen(Qt::black);
        layout.draw(&painter, QPointF(contents.left(), contents.top() + (contents.height() - line.height()) / 2));
        painter.end();
        auto const background = actual.pixelColor(actual.width() / 2, 0);
        int ink = 0, visible = 0;
        for (int y = 0; y < prefix.height(); ++y)
        {
            for (int x = 0; x < prefix.width(); ++x)
            {
                if (prefix.pixelColor(x, y).alpha() < 230) { continue; }
                ++ink;
                auto const pixel = actual.pixelColor(x, y);
                if (std::abs(pixel.red() - background.red()) + std::abs(pixel.green() - background.green()) +
                    std::abs(pixel.blue() - background.blue()) > 80)
                { ++visible; }
            }
        }
        bool const prefix_visible = ink >= 30 && visible * 100 >= ink * 95;
        std::cout << (prefix_visible ? "PASS" : "RED") << " Qt pinned visible leading label width=" << width
                  << " ink=" << ink << " visible=" << visible << '\n';
        intact = intact && prefix_visible;
        if (!artifact_directory.isEmpty())
        {
            check(page.grab().save(artifact_directory + QStringLiteral("/qt_pinned_visible_%1.png").arg(width)),
                  "The natural-width pinned control retains an original complete capture");
        }
    }
    page.activateWindow();
    wait([&] { return page.isActiveWindow(); });
    button->clearFocus();
    QApplication::processEvents();
    auto const unfocused = button->grab().toImage();
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(&page, &tab);
    button->setFocus(Qt::TabFocusReason);
    QApplication::processEvents();
    std::cout << "Qt pinned focus has_focus=" << button->hasFocus()
              << " pixels_changed=" << (button->grab().toImage() != unfocused) << '\n';
    check(button->hasFocus() && button->grab().toImage() != unfocused,
          "The readable pinned label retains a visible keyboard focus indication");
    auto const* accessible = QAccessible::queryAccessibleInterface(button);
    check(accessible && accessible->role() == QAccessible::Button &&
              accessible->text(QAccessible::Name).startsWith(QStringLiteral("置顶消息")),
          "Pinned elision retains the actual button accessibility role and leading name");
    int const before_keyboard = searches;
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(button, &press);
    QApplication::sendEvent(button, &release);
    check(searches == before_keyboard + 1 && query == prefix,
          "The elided pinned button still locates the actual older message with Space");
    button->setEnabled(false);
    button->click();
    check(searches == before_keyboard + 1, "A disabled pinned button cannot invoke its location action");
    member_data owner;
    owner.id = 1;
    owner.username = QStringLiteral("本人");
    owner.role = chat::member_role::owner;
    page.set_members(50, {owner}, {});
    QApplication::processEvents();
    auto* unpin = page.findChild<QToolButton*>("unpinMessageButton");
    check(unpin && unpin->isVisible(), "The actual group owner can see the unpin control");
    auto const unpin_image = unpin->grab().toImage();
    check(logical_pixel(unpin_image, QPoint(5, unpin->height() / 2)).lightness() >= 230 &&
              unpin->palette().color(QPalette::ButtonText) == QColor("#315A4B"),
          "The unpin control has a readable cream-surface secondary-action palette");
    return intact;
}

void check_message_action_targets()
{
    // Real menus and dialogs, without a server: older history resets the model
    // while the user's action must retain the message selected before that reset.
    for (auto const& action_text : {QStringLiteral("删除"), QStringLiteral("回复"),
                                   QStringLiteral("下载文件"), QStringLiteral("已读详情")})
    {
        int const modes = action_text == QStringLiteral("删除") ? 4 : 2;
        for (int mode = 0; mode < modes; ++mode)
        {
            chat_widget page;
            page.setStyleSheet(chat_style_sheet());
            page.resize(980, 640);
            page.set_user(QStringLiteral("本人"), 1);
            page.set_connection_available(true);
            conversation_data original_conversation;
            original_conversation.id = 50;
            original_conversation.group = true;
            original_conversation.username = QStringLiteral("原会话");
            original_conversation.can_send = true;
            page.open_conversation(original_conversation);
            message_data original;
            original.id = 7; original.conversation = 50; original.from = 1;
            original.username = QStringLiteral("原发送者");
            original.text = QStringLiteral("原消息 7：中文 🙂");
            if (action_text == QStringLiteral("下载文件"))
            {
                original.attachment = attachment_data{QStringLiteral("original-7.bin"),
                    QStringLiteral("application/octet-stream"), 12};
            }
            read_positions const positions{{2, 6}, {3, 7}};
            page.set_messages(50, {original}, positions, false, false, false);
            member_data self; self.id = 1; self.username = QStringLiteral("本人");
            member_data older_reader; older_reader.id = 2; older_reader.username = QStringLiteral("只读到 6");
            member_data original_reader; original_reader.id = 3; original_reader.username = QStringLiteral("已读原消息 7");
            page.set_members(50, {self, older_reader, original_reader}, {});
            page.show();
            QApplication::processEvents();
            auto* view = page.findChild<QListView*>("messageList");
            auto const index = view->model()->index(0, 0);
            view->scrollTo(index);
            QApplication::processEvents();

            int operation_count = 0;
            qint64 submitted_conversation = 0, submitted_message = 0;
            QString submitted_filename;
            QObject::connect(&page, &chat_widget::delete_message_requested, &page,
                [&](qint64 conversation, qint64 id) {
                    ++operation_count; submitted_conversation = conversation; submitted_message = id;
                });
            QObject::connect(&page, &chat_widget::send_message_requested, &page,
                [&](qint64 conversation, QString, qint64 reply) {
                    ++operation_count; submitted_conversation = conversation; submitted_message = reply;
                });
            QObject::connect(&page, &chat_widget::attachment_open_requested, &page,
                [&](qint64 conversation, qint64 id, QString filename, bool) {
                    ++operation_count; submitted_conversation = conversation; submitted_message = id;
                    submitted_filename = std::move(filename);
                });
            auto insert_older = [&] {
                auto older = original; older.id = 6;
                older.username = QStringLiteral("另一发送者");
                older.text = QStringLiteral("不相关的较早消息 6");
                if (older.attachment) { older.attachment->filename = QStringLiteral("older-6.bin"); }
                page.set_messages(50, {older}, positions, true, false, false);
                check(view->model()->rowCount() == 2 &&
                          view->model()->index(0, 0).data(message_model::id_role).toLongLong() == 6 &&
                          view->model()->index(1, 0).data(message_model::id_role).toLongLong() == 7,
                      "Action target fixture really inserts an older message ahead of the original");
            };
            auto change_conversation = [&] {
                auto other = original_conversation; other.id = 60; other.username = QStringLiteral("另一会话");
                page.open_conversation(other);
                auto unrelated = original; unrelated.id = 70; unrelated.conversation = 60;
                page.set_messages(60, {unrelated}, positions, false, false, false);
                page.set_members(60, {self, older_reader, original_reader}, {});
                check(view->model()->index(0, 0).data(message_model::id_role).toLongLong() == 70,
                      "Changed-conversation fixture is ready and contains a different message");
            };

            bool selected = false, inspected = false, timed_out = false;
            QList<qint64> actual_readers;
            QTimer inspect;
            QObject::connect(&inspect, &QTimer::timeout, &page, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) { return; }
                if (auto* confirmation = qobject_cast<QMessageBox*>(dialog))
                {
                    if (action_text != QStringLiteral("删除")) { return; }
                    inspect.stop(); inspected = true;
                    if (mode == 0 || mode == 2) { insert_older(); }
                    if (mode == 3) { change_conversation(); }
                    confirmation->button(mode == 2 ? QMessageBox::No : QMessageBox::Yes)->click();
                }
                else if (dialog->objectName() == QStringLiteral("readDetailsDialog"))
                {
                    inspect.stop(); inspected = true;
                    auto* list = dialog->findChild<QListWidget*>("readMembersList");
                    check(list, "Read details use the actual members list");
                    for (int row = 0; row < list->count(); ++row)
                    {
                        actual_readers.push_back(list->item(row)->data(Qt::UserRole).toLongLong());
                    }
                    dialog->reject();
                }
            });
            inspect.start(0);
            // Bounded escape for a missing driver callback, never a product PASS.
            QTimer watchdog;
            watchdog.setSingleShot(true);
            QObject::connect(&watchdog, &QTimer::timeout, &page, [&] {
                timed_out = true;
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) { dialog->reject(); }
                if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) { menu->close(); }
            });
            watchdog.start(5000);
            QTimer::singleShot(0, &page, [&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                check(menu, "Message actions are reached through the actual context menu");
                QAction* action = nullptr;
                for (auto* item : menu->actions()) { if (item->text() == action_text) { action = item; } }
                check(action, "The fixture exposes the requested real message action");
                selected = true;
                if (mode == 1) { change_conversation(); }
                else if (action_text != QStringLiteral("删除")) { insert_older(); }
                menu->setActiveAction(action);
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(menu, &enter);
            });
            view->customContextMenuRequested(view->visualRect(index).intersected(view->viewport()->rect()).center());
            inspect.stop(); watchdog.stop();
            check(selected && !timed_out, "Message action driver selected its real menu item within a bounded wait");

            if (action_text == QStringLiteral("回复"))
            {
                auto* preview = page.findChild<QLabel*>("replyPreview");
                check(preview, "Reply exposes its real preview");
                if (mode == 0)
                {
                    check(preview->isVisible() &&
                              preview->text() == QStringLiteral("回复 原发送者：原消息 7：中文 🙂"),
                          "Reply preview retains the original sender and body after history insertion");
                    page.findChild<QPlainTextEdit*>("messageEdit")->setPlainText(QStringLiteral("回复原消息"));
                    page.findChild<QToolButton*>("sendButton")->click();
                }
                else { check(!preview->isVisible(), "Changing conversation while the menu is open does not start a reply"); }
            }
            if (action_text == QStringLiteral("已读详情"))
            {
                check(mode == 0 ? inspected && actual_readers == QList<qint64>{3} : !inspected,
                      "Read details retain the original message readers and do not cross conversations");
                check(operation_count == 0, "Read details do not emit a modifying operation");
            }
            else
            {
                check(operation_count == (mode == 0 ? 1 : 0),
                      "Only a confirmed action in its original conversation emits an operation");
                if (mode == 0)
                {
                    check(submitted_conversation == 50 && submitted_message == 7,
                          "Delete, reply and attachment retain the original message target after history insertion");
                    if (action_text == QStringLiteral("下载文件"))
                    {
                        check(submitted_filename == QStringLiteral("original-7.bin"),
                              "Attachment target and filename both belong to the original message");
                    }
                }
            }
            if (action_text == QStringLiteral("删除") && mode != 1)
            {
                check(inspected, "Deletion modes entered the actual confirmation dialog");
            }
        }
    }
    std::cout << "PASS Qt message action target identity, cancellation and conversation fencing\n";
}

void check_message_search_keyboard_visibility()
{
    message_search_dialog dialog(50, 1, false, QStringLiteral("搜索焦点"), {}, nullptr);
    dialog.setStyleSheet(chat_style_sheet());
    auto* input = dialog.findChild<QLineEdit*>("messageSearchEdit");
    auto* button = dialog.findChild<QPushButton*>("searchMessagesButton");
    auto* results = dialog.findChild<QListView*>("messageSearchResults");
    dialog.show();
    dialog.activateWindow();
    input->setText(QStringLiteral("关键词"));
    button->click();
    QList<message_data> matches;
    for (int i = 0; i < 3; ++i)
    {
        message_data match;
        match.id = 100 + i;
        match.conversation = 50;
        match.from = 2;
        match.username = QStringLiteral("朋友");
        match.timestamp = 1770000000000 + i;
        match.text = QStringLiteral("关键词 %1 é 👩‍💻").arg(i + 1);
        matches.push_back(match);
    }
    dialog.set_results(50, QStringLiteral("关键词"), 0, matches, {}, false, {});
    input->setFocus();
    QApplication::processEvents();
    check(input->hasFocus() && results->selectionMode() == QAbstractItemView::SingleSelection,
          "Search starts at the query and preserves the results' actual SingleSelection policy");
    auto row_image = [&](int row) {
        QApplication::processEvents();
        auto const rect = results->visualRect(results->model()->index(row, 0));
        check(results->viewport()->rect().contains(rect), "Focus fixtures remain fully visible without scrolling");
        auto const pixmap = results->viewport()->grab();
        auto const ratio = pixmap.devicePixelRatio();
        return pixmap.toImage().copy(qRound(rect.x() * ratio), qRound(rect.y() * ratio),
                                    qRound(rect.width() * ratio), qRound(rect.height() * ratio));
    };
    auto const plain0 = row_image(0);
    auto const plain1 = row_image(1);
    auto const plain2 = row_image(2);
    auto key = [&](int code) {
        auto* focused = QApplication::focusWidget();
        check(focused, "Search keyboard events go to the actual focused widget");
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
        QApplication::sendEvent(focused, &press);
        QApplication::sendEvent(focused, &release);
        QApplication::processEvents();
    };
    key(Qt::Key_Tab);
    check(button->hasFocus(), "Tab first reaches the search action");
    key(Qt::Key_Tab);
    check(results->hasFocus() && results->currentIndex().row() == 0 &&
          results->selectionModel()->selectedRows().isEmpty() && row_image(0) != plain0,
          "Tab visibly identifies the current search result before selection");
    key(Qt::Key_Down);
    check(results->hasFocus() && results->currentIndex().row() == 1 &&
          results->selectionModel()->isSelected(results->currentIndex()) &&
          row_image(0) == plain0 && row_image(1) != plain1,
          "Down moves the visible search-result identity to the selected second row");
    key(Qt::Key_Down);
    check(results->currentIndex().row() == 2 && results->selectionModel()->isSelected(results->currentIndex()) &&
          row_image(1) == plain1 && row_image(2) != plain2,
          "A second Down moves the visible selection without leaving a stale row outline");
    auto const focused2 = row_image(2);
    input->setFocus();
    QApplication::processEvents();
    check(!results->hasFocus() && results->selectionModel()->isSelected(results->currentIndex()) &&
          row_image(2) != focused2 && row_image(2) != plain2,
          "Blur removes the focus outline while retaining visible selected-result identity");
    dialog.reject();
    std::cout << "PASS Qt search results show actual Tab/Down focus and selected identity\n";
}

void check_message_search_live_policy()
{
    message_search_dialog dialog(50, 1, false, QStringLiteral("实时搜索命中"), {}, nullptr);
    dialog.setStyleSheet(chat_style_sheet());
    auto* input = dialog.findChild<QLineEdit*>("messageSearchEdit");
    auto* search = dialog.findChild<QPushButton*>("searchMessagesButton");
    auto* more = dialog.findChild<QPushButton*>("moreSearchResultsButton");
    auto* results = dialog.findChild<QListView*>("messageSearchResults");
    auto* status = dialog.findChild<QLabel*>("messageSearchStatus");
    auto* count = dialog.findChild<QLabel*>("messageSearchCount");
    QList<QPair<QString, qint64>> requests;
    QObject::connect(&dialog, &message_search_dialog::search_requested, &dialog,
        [&](QString query, qint64 before) { requests.push_back({query, before}); });
    auto const query = QStringLiteral("%_中文");
    auto message = [&](qint64 id) {
        message_data value;
        value.id = id; value.conversation = 50; value.from = 2;
        value.username = QStringLiteral("朋友"); value.timestamp = 1770000000000 + id;
        value.text = query + QStringLiteral(" original é 👩‍💻 %1").arg(id);
        return value;
    };
    auto start = [&] { input->setText(query); search->click(); QApplication::processEvents(); };
    auto index_for = [&](qint64 id) {
        for (int row = 0; row < results->model()->rowCount(); ++row)
        {
            auto index = results->model()->index(row, 0);
            if (index.data(message_model::id_role).toLongLong() == id) { return index; }
        }
        return QModelIndex{};
    };
    auto ids = [&] {
        QApplication::processEvents();
        QList<qint64> values;
        for (int row = 0; row < results->model()->rowCount(); ++row)
        { values.push_back(results->model()->index(row, 0).data(message_model::id_role).toLongLong()); }
        return values;
    };
    auto accessible_name = [&](qint64 id) {
        QApplication::processEvents();
        auto const index = index_for(id);
        check(index.isValid(), "Accessible search context belongs to an actual visible hit");
        auto* accessible = QAccessible::queryAccessibleInterface(results);
        check(accessible && accessible->tableInterface(), "Search list exposes the actual Qt accessible table interface");
        auto* cell = accessible->tableInterface()->cellAt(index.row(), 0);
        check(cell && cell->role() == QAccessible::ListItem, "Search context is read from an accessible list item");
        return cell->text(QAccessible::Name);
    };
    dialog.show(); dialog.activateWindow(); QApplication::processEvents();
    check(count->isHidden() && status->isVisible(), "A new search shows only its input prompt, not an empty loaded-count row");
    check(input->accessibleName() == QStringLiteral("消息搜索关键词") &&
          results->accessibleName() == QStringLiteral("消息搜索结果"),
          "Message search input and result list have explicit accessible purposes");
    start();
    check(count->isHidden() && status->isVisible(), "Initial search loading has one response feedback line");
    dialog.set_results(50, query, 0, {message(100), message(101)}, {}, false, {});
    auto const initial_name = accessible_name(100);
    auto const initial_time = QDateTime::fromMSecsSinceEpoch(message(100).timestamp).toLocalTime()
                                 .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    check(initial_name == index_for(100).data(Qt::AccessibleTextRole).toString() &&
          initial_name.contains(QStringLiteral("朋友")) && initial_name.contains(initial_time) &&
          initial_name.contains(message(100).text), "Public accessible search item reads canonical author/date/body through its proxy");
    auto edit = message(100); edit.text = QStringLiteral("no longer a current match"); edit.edited_at = 20;
    dialog.update_message(50, edit, {});
    check(accessible_name(100).contains(edit.text) && accessible_name(100).contains(QStringLiteral("已编辑")) &&
          !accessible_name(100).contains(message(100).text), "Accessible proxy item follows live edited text without retaining stale body");
    auto matching_edit = message(101); matching_edit.edited_at = 21;
    dialog.update_message(50, matching_edit, {});
    dialog.update_message(50, message(99), {});
    check(ids() == QList<qint64>{100, 101} && index_for(100).data(message_model::text_role).toString() == edit.text &&
          requests.size() == 1, "Loaded hits retain live matching/nonmatching edits without matching or hidden searches");
    auto deleted = edit; deleted.deleted = true; deleted.text.clear();
    dialog.update_message(50, deleted, {});
    check(ids() == QList<qint64>{101}, "A live-deleted loaded hit is actually absent, not a counted tombstone");
    check(index_for(101).data(Qt::AccessibleTextRole).toString().contains(message(101).text) &&
          !index_for(101).data(Qt::AccessibleTextRole).toString().contains(edit.text),
          "Accessible remaining model hit does not inherit the deleted target body");
    auto* help = dialog.findChild<QLabel*>("messageSearchHelp");
    check(help && help->isVisible() && help->text().contains(QStringLiteral("正文实时更新")) &&
          help->text().contains(QStringLiteral("重新搜索")) && count && count->text().contains(QStringLiteral("已加载 1")),
          "Search persistently explains live bodies and counts only visible loaded hits");
    check(help->font().pixelSize() == 13 && count->font().pixelSize() == 13 &&
          help->palette().color(QPalette::WindowText) == QColor(QStringLiteral("#5D6C64")) &&
          count->palette().color(QPalette::WindowText) == QColor(QStringLiteral("#5D6C64")),
          "Search help and loaded count reuse the secondary-label font and foreground");
    auto last_deleted = matching_edit; last_deleted.deleted = true;
    dialog.update_message(50, last_deleted, {});
    check(ids().isEmpty() && count->text().contains(QStringLiteral("重新搜索")) &&
          count->isVisible() && status->isHidden() && status->text() != QStringLiteral("没有匹配的消息。"),
          "Deleting the last loaded hit visibly invites refresh, not fresh global emptiness");
    start(); dialog.set_results(50, query, 0, {}, {}, false, {});
    check(status->text() == QStringLiteral("没有匹配的消息。") && status->isVisible() && count->isHidden(),
          "Explicit refresh shows one authoritative empty-page response without a duplicate zero count");
    input->clear(); search->click(); QApplication::processEvents();
    check(status->text() == QStringLiteral("请输入关键词。") && status->isVisible() && count->isHidden(),
          "An invalid empty query keeps only actionable input feedback");
    start(); dialog.set_results(50, query, 0, {}, {}, false, QStringLiteral("初页搜索错误"));
    dialog.update_message(50, message(99), {});
    check(status->isVisible() && status->text() == QStringLiteral("初页搜索错误") && count->isHidden(),
          "An initial page error remains the sole zero-hit feedback even after an unknown live event");

    start();
    dialog.set_reactions(50, 100, 7, {{QStringLiteral("👍"), {2}}}, {});
    dialog.set_reactions(50, 100, 6, {{QStringLiteral("❤️"), {2}}}, {});
    dialog.update_message(50, edit, {});
    dialog.update_message(50, last_deleted, {});
    check(ids().isEmpty() && status->text() == QStringLiteral("正在搜索…"),
          "Unknown live events remain invisible and do not overwrite pending-search status");
    check(count->isHidden(), "Unknown live events do not expose duplicate zero-count text during loading");
    dialog.set_results(50, query, 0, {message(100), message(101)}, {}, true, {});
    auto const merged_reactions = index_for(100).data(message_model::reactions_role).value<QList<reaction_data>>();
    check(ids() == QList<qint64>{100} && index_for(100).data(message_model::text_role).toString() == edit.text &&
          !merged_reactions.isEmpty() && merged_reactions.front().emoji == QStringLiteral("👍"),
          "Initial late pages reconcile unknown edits, deletion and independent reaction revision");
    auto const late_name = index_for(100).data(Qt::AccessibleTextRole).toString();
    check(late_name.contains(edit.text) && late_name.contains(QStringLiteral("反应 👍 1 人")),
          "Accessible late-page model context consumes canonical live edit and reaction facets");
    more->click();
    auto dead80 = message(80); dead80.deleted = true;
    auto dead81 = message(81); dead81.deleted = true;
    dialog.update_message(50, dead80, {}); dialog.update_message(50, dead81, {});
    dialog.update_message(50, message(10), {});
    dialog.set_results(50, query, 100, {message(80), message(81)}, {}, true, {});
    check(ids() == QList<qint64>{100} && more->isEnabled(), "An all-filtered older page still permits pagination without exposing unknown live IDs");
    more->click();
    check(requests.back().second == 80, "Pagination uses the current raw-page boundary, not visible or cached-source first ID");
    results->setCurrentIndex(index_for(100));
    dialog.set_reactions(50, 78, 9, {}, {});
    dialog.set_reactions(50, 78, 8, {{QStringLiteral("❤️"), {2}}}, {});
    auto live79 = message(79); live79.edited_at = 25; live79.text = QStringLiteral("older-page live body no longer matching");
    dialog.update_message(50, live79, {});
    auto raw78 = message(78); raw78.reaction_revision = 1; raw78.reactions = {{QStringLiteral("👍"), {2}}};
    auto const raw79 = message(79);
    dialog.set_results(50, query, 80, {raw78, raw79}, {}, false, {});
    check(ids() == QList<qint64>{78, 79, 100} && index_for(78).data(message_model::text_role).toString() == raw78.text &&
          index_for(78).data(message_model::reactions_role).value<QList<reaction_data>>().isEmpty() &&
          index_for(79).data(message_model::text_role).toString() == live79.text &&
          results->currentIndex().data(message_model::id_role).toLongLong() == 100 &&
          results->selectionModel()->isSelected(results->currentIndex()),
          "Reaction-only pending clears retain the real unedited body and older-page merges retain selected ID");
    auto dead79 = raw79; dead79.deleted = true; dialog.update_message(50, dead79, {});
    check(results->currentIndex().data(message_model::id_role).toLongLong() == 100,
          "Deleting a preceding visible row preserves selected identity");
    auto dead78 = raw78; dead78.deleted = true; dialog.update_message(50, dead78, {});
    dialog.update_message(50, deleted, {});
    check(ids().isEmpty() && !results->currentIndex().isValid() && results->selectionModel()->selectedRows().isEmpty(),
          "Deleting the selected last visible hit clears selection and current index");

    start();
    auto dead60 = message(60); dead60.deleted = true;
    auto dead61 = message(61); dead61.deleted = true;
    dialog.update_message(50, dead60, {}); dialog.update_message(50, dead61, {});
    dialog.set_results(50, query, 0, {message(60), message(61)}, {}, true, {});
    check(ids().isEmpty() && more->isEnabled(), "A wholly hidden loaded set still exposes its real older-page action");
    more->click();
    check(requests.back().second == 60, "Zero visible hits preserve the initial raw-page cursor");

    start(); dialog.set_results(50, query, 0, {message(100)}, {}, true, {}); more->click();
    dialog.set_results(50, query, 100, {}, {}, false, QStringLiteral("测试搜索错误"));
    dialog.update_message(50, edit, {}); dialog.set_reactions(50, 100, 30, {}, {});
    auto foreign = deleted; foreign.conversation = 51;
    dialog.update_message(50, foreign, {}); dialog.update_message(51, deleted, {});
    check(ids() == QList<qint64>{100} && status->text() == QStringLiteral("测试搜索错误"),
          "Live events preserve page-error status and foreign-conversation events cannot delete a loaded hit");
    check(count->isVisible() && status->isVisible(), "An older-page error preserves the distinct nonzero loaded count");
    dialog.set_reactions(50, 99, 50, {{QStringLiteral("❤️"), {2}}}, {});
    auto const request_count = requests.size();
    start();
    check(requests.size() == request_count + 1 && requests.back() == qMakePair(query, qint64(0)) && ids().isEmpty(),
          "Same-query explicit refresh sends a new before-zero request and resets loaded membership");
    dialog.set_results(50, query, 100, {message(80)}, {}, true, {});
    dialog.set_results(51, query, 0, {message(100)}, {}, false, {});
    dialog.set_results(50, QStringLiteral("old query"), 0, {message(100)}, {}, false, {});
    check(ids().isEmpty() && status->text() == QStringLiteral("正在搜索…"), "Stale before/query/conversation pages cannot overwrite a refresh");
    dialog.set_results(50, query, 0, {message(99)}, {}, false, {});
    check(index_for(99).data(message_model::reactions_role).value<QList<reaction_data>>().isEmpty(),
          "Same-query refresh does not reuse pending reaction state from a prior request scope");

    start(); dialog.set_results(50, query, 0, {message(100), message(101), message(102)}, {}, true, {});
    results->setCurrentIndex(index_for(101));
    auto middle_deleted = message(101); middle_deleted.deleted = true;
    dialog.update_message(50, middle_deleted, {});
    check(results->currentIndex().data(message_model::id_role).toLongLong() == 102 &&
          results->selectionModel()->isSelected(results->currentIndex()), "Deleted selected hit moves to the nearest next visible hit");
    auto final_deleted = message(102); final_deleted.deleted = true; dialog.update_message(50, final_deleted, {});
    check(results->currentIndex().data(message_model::id_role).toLongLong() == 100,
          "Deleting the last selected row falls back to the preceding hit");
    more->click();
    auto const original = message(100).text;
    QGuiApplication::clipboard()->clear();
    QTimer::singleShot(0, &dialog, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        check(menu, "Loaded-hit copying opens the real menu before a live deletion");
        dialog.update_message(50, deleted, {});
        dialog.set_results(50, query, 100, {message(99)}, {}, false, {});
        check(ids() == QList<qint64>{99}, "Deletion and pending insertion genuinely replace the clicked proxy row");
        QAction* copy = nullptr;
        for (auto* action : menu->actions()) { if (action->text() == QStringLiteral("复制消息")) { copy = action; } }
        check(copy, "The already-open menu retains its captured copy action");
        menu->setActiveAction(copy);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
        QApplication::sendEvent(menu, &enter);
    });
    results->customContextMenuRequested(results->visualRect(index_for(100)).center());
    check(QGuiApplication::clipboard()->text() == original,
          "An already-open copy menu keeps its captured original text, never the replacement row");
    dialog.reject();
    std::cout << "PASS Qt loaded search hits, live reconciliation, raw cursors, status, selection and captured copy\n";
}

void check_message_dialogs()
{
    message_search_dialog dialog(50, 1, false, QStringLiteral("朋友"), {}, nullptr);
    dialog.setStyleSheet(chat_style_sheet());
    QList<qint64> requested;
    QObject::connect(&dialog, &message_search_dialog::search_requested, &dialog,
        [&](QString query, qint64 before) {
            check(query == QStringLiteral("中文关键词"), "Enter searches the current query");
            requested.push_back(before);
        });
    dialog.show();
    QApplication::processEvents();
    auto* input = dialog.findChild<QLineEdit*>("messageSearchEdit");
    input->setFocus();
    input->setText(QStringLiteral("中文关键词"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QApplication::sendEvent(input, &enter);
    QApplication::processEvents();
    check(requested == QList<qint64>{0} && dialog.isVisible() && input->hasFocus(),
          "Enter submits exactly one search without closing the dialog or losing input focus");
    message_data match;
    match.id = 100;
    match.conversation = 50;
    match.from = 2;
    match.username = QStringLiteral("朋友");
    match.text = QStringLiteral("中文关键词");
    dialog.set_results(50, QStringLiteral("中文关键词"), 0, {match}, {}, true, {});
    auto* more = dialog.findChild<QPushButton*>("moreSearchResultsButton");
    more->setFocus();
    QApplication::processEvents();
    QApplication::sendEvent(more, &enter);
    check(requested == QList<qint64>{0, 100} && dialog.isVisible(),
          "Enter on the focused pagination button loads earlier results without closing search");
    input->setFocus();
    QApplication::processEvents();
    QApplication::sendEvent(input, &enter);
    check(requested == QList<qint64>{0, 100, 0} && dialog.isVisible(),
          "Returning to the query restores Enter search after paging");
    auto* search_button = dialog.findChild<QPushButton*>("searchMessagesButton");
    auto* earlier = dialog.findChild<QPushButton*>("moreSearchResultsButton");
    auto* search_close = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Close);
    check(input->mapTo(&dialog, QPoint()).x() == 24 && input->mapTo(&dialog, QPoint()).y() == 24 &&
          earlier->mapTo(&dialog, earlier->rect().center()).y() == search_close->mapTo(&dialog, search_close->rect().center()).y(),
          "Search shares detail padding and keeps secondary pagination in the footer");
    check(search_close->text() == QStringLiteral("关闭") && search_close->icon().isNull() &&
          logical_pixel(search_button->grab().toImage(), QPoint(search_button->width()/2, 5)) == QColor(49, 90, 75),
          "Search has one primary action and a localized secondary close without a platform icon");
    match.text = QStringLiteral("中文关键词 original é 👩‍💻");
    dialog.set_results(50, QStringLiteral("中文关键词"), 0, {match}, {}, true, {});
    earlier->click();
    auto* results = dialog.findChild<QListView*>("messageSearchResults");
    auto const result_index = results->model()->index(0, 0);
    results->scrollTo(result_index);
    QApplication::processEvents();
    QGuiApplication::clipboard()->clear();
    QTimer::singleShot(0, &dialog, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        check(menu, "Search copying opens the actual context menu");
        auto older = match;
        older.id = 99;
        older.text = QStringLiteral("另一条较早的搜索结果");
        dialog.set_results(50, QStringLiteral("中文关键词"), 100, {older}, {}, false, {});
        check(results->model()->rowCount() == 2 &&
              results->model()->index(0, 0).data(message_model::id_role).toLongLong() == 99 &&
              results->model()->index(1, 0).data(message_model::id_role).toLongLong() == 100,
              "A pending search page actually moves the original result to another row");
        QAction* copy = nullptr;
        for (auto* action : menu->actions()) { if (action->text() == QStringLiteral("复制消息")) { copy = action; } }
        check(copy, "Search results expose their real copy action");
        menu->setActiveAction(copy);
        QApplication::sendEvent(menu, &enter);
    });
    results->customContextMenuRequested(results->visualRect(result_index).intersected(results->viewport()->rect()).center());
    check(QGuiApplication::clipboard()->text() == match.text,
          "Search copying retains the complete original message despite a pending page arriving");
    auto* close = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Close);
    close->setFocus();
    QApplication::processEvents();
    QApplication::sendEvent(close, &enter);
    check(!dialog.isVisible(), "The focused Close button remains usable with Enter");
    QPixmap source(640, 360);
    source.fill(QColor(37, 83, 68));
    attachment_dialog preview(50, 3, QStringLiteral("图片.png"), true, nullptr, source);
    preview.setStyleSheet(chat_style_sheet());
    preview.set_data(50, 3, QByteArray("cached image bytes"), {});
    preview.show();
    QApplication::processEvents();
    auto* image = preview.findChild<QLabel*>("attachmentImage");
    auto* save_button = preview.findChild<QPushButton*>("saveAttachmentButton");
    auto* preview_close = preview.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Close);
    auto* status = preview.findChild<QLabel*>("attachmentStatus");
    check(status->mapTo(&preview, QPoint()).x() == 24 &&
          save_button->mapTo(&preview, save_button->rect().center()).y() == preview_close->mapTo(&preview, preview_close->rect().center()).y() &&
          preview_close->text() == QStringLiteral("关闭") && preview_close->icon().isNull(),
          "Attachment uses shared padding and one compact footer for save and close");
    check(logical_pixel(save_button->grab().toImage(), QPoint(save_button->width()/2, 5)) == QColor(49, 90, 75),
          "Saving the original attachment is the primary dialog action");
    for (auto size : {QSize(420, 360), QSize(700, 600), QSize(500, 440)})
    {
        preview.resize(size);
        QApplication::processEvents();
        check(preview.size() == size, "An image does not impose its source dimensions as the preview window minimum");
        auto const pixels = image->pixmap().size();
        check(pixels.width() <= image->width() && pixels.height() <= image->height() &&
              std::abs(pixels.width() * 9 - pixels.height() * 16) <= 16,
              "Resizing image preview fits the complete image while preserving its aspect ratio");
        check(pixels.width() <= 640 && pixels.height() <= 360 &&
              preview.findChild<QPushButton*>("saveAttachmentButton")->isEnabled(),
              "Preview resize never enlarges the source or disables original-file saving");
    }
    preview.reject();
    QTemporaryDir chooser_files;
    check(chooser_files.isValid(), "File chooser has an owned directory");
    for (auto const mode : {QFileDialog::AcceptOpen, QFileDialog::AcceptSave})
    {
        QFileDialog picker(&dialog);
        picker.setOption(QFileDialog::DontUseNativeDialog);
        picker.setAcceptMode(mode);
        picker.setViewMode(QFileDialog::Detail);
        picker.setDirectory(chooser_files.path());
        picker.show();
        QApplication::processEvents();
        for (auto const name : {"lookInCombo", "fileTypeCombo"})
        {
            auto* combo = picker.findChild<QComboBox*>(name);
            check(combo && combo->isVisible(), "Open and Save expose their actual location and file-type controls");
            auto const pixels = combo->grab().toImage();
            check(logical_pixel(pixels, QPoint(combo->width()/2, 4)) == QColor(QStringLiteral("#FFFFFF")) &&
                  combo->palette().color(QPalette::ButtonText) == QColor(QStringLiteral("#27332E")),
                  "File chooser location and file type have readable text on a light surface");
            combo->setFocus();
            QApplication::processEvents();
            auto const focused = combo->grab().toImage();
            check(focused.pixelColor(0, focused.height()/2) == QColor(QStringLiteral("#547C68")),
                  "File chooser combos expose their keyboard focus on the light surface");
            combo->showPopup();
            QApplication::processEvents();
            check(combo->view()->isVisible() &&
                  combo->view()->palette().color(QPalette::Base) == QColor(QStringLiteral("#FFFFFF")) &&
                  combo->view()->palette().color(QPalette::Text) == QColor(QStringLiteral("#27332E")) &&
                  combo->view()->palette().color(QPalette::Highlight) == QColor(QStringLiteral("#315A4B")) &&
                  combo->view()->palette().color(QPalette::HighlightedText) == QColor(QStringLiteral("#FFFFFF")),
                  "File chooser popup lists retain readable unselected and selected text");
            combo->hidePopup();
        }
        bool visible_header = false;
        for (auto* header : picker.findChildren<QHeaderView*>())
        {
            if (!header->isVisible()) { continue; }
            visible_header = true;
            auto const pixels = header->viewport()->grab().toImage();
            check(logical_pixel(pixels, QPoint(5, 3)) == QColor(QStringLiteral("#F0F4F1")) &&
                  header->palette().color(QPalette::ButtonText) == QColor(QStringLiteral("#27332E")),
                  "File chooser column headings remain readable on their own light surface");
        }
        check(visible_header, "The actual file chooser details header was checked");
        auto* details = picker.findChild<QToolButton*>("detailModeButton");
        check(details && details->isVisible() && details->isDown(), "File chooser exposes its current details mode");
        auto const selected_mode = details->grab().toImage();
        check(logical_pixel(selected_mode, QPoint(5, details->height()/2)) == QColor(QStringLiteral("#E7EEE9")),
              "The current file view mode uses the same readable selected surface");
        picker.reject();
    }
    std::cout << "PASS Qt message dialog keyboard, footer hierarchy and responsive image preview\n";
}

void check_message_composer()
{
    chat_widget page;
    page.setStyleSheet(chat_style_sheet());
    page.resize(1180, 760);
    page.set_user(QStringLiteral("本人"), 1);
    conversation_data direct;
    direct.id = 50;
    direct.user = 2;
    direct.username = QStringLiteral("朋友");
    direct.can_send = true;
    page.open_conversation(direct);
    page.show();
    QApplication::processEvents();
    auto* edit = page.findChild<QPlainTextEdit*>("messageEdit");
    check(edit, "Message composer supports plain multiline editing");
    auto* send = page.findChild<QToolButton*>("sendButton");
    auto const compact_height = edit->height();
    check(compact_height <= 46 && !send->isEnabled() && !edit->accessibleName().isEmpty(),
          "Empty composer is compact, named and cannot send");
    int sent = 0;
    QString sent_text;
    QObject::connect(&page, &chat_widget::send_message_requested, &page,
        [&](qint64 conversation, QString text, qint64 reply) {
            check(conversation == direct.id && reply == 0, "Composer sends to the active conversation");
            ++sent;
            sent_text = std::move(text);
        });
    edit->setFocus();
    auto const text = QStringLiteral("第一行 中文 0123456789 é\n第二行 ❤︎ ❤️ 1️⃣7️⃣#️⃣*️⃣\n第三行 👩‍💻 👨‍👩‍👧‍👦 👍️🏽 \U0001faff A🏽");
    QApplication::clipboard()->setText(text);
    QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier, "v");
    QApplication::sendEvent(edit, &paste);
    check(edit->toPlainText() == text, "Multiline paste preserves message text and newlines");
    wait([&] { return edit->height() > compact_height; });
    check(edit->verticalScrollBar()->maximum() == 0, "Short Chinese and emoji drafts show all lines without scrolling");
    check(sent == 0 && send->isEnabled(), "Pasting only edits the draft");
    edit->selectAll();
    edit->copy();
    check(QApplication::clipboard()->text().toUtf8() == text.toUtf8(),
          "Composer copying preserves exact Unicode UTF-8 including selectors, keycaps and unknown text");
    edit->moveCursor(QTextCursor::End);
    QString const suffix = QStringLiteral("追加 é 👍️🏽");
    QApplication::clipboard()->setText(suffix);
    QApplication::sendEvent(edit, &paste);
    QApplication::processEvents();
    check(edit->toPlainText().toUtf8() == (text + suffix).toUtf8(),
          "Composer pasting preserves exact extended Unicode sequences");
    edit->undo();
    QApplication::processEvents();
    check(edit->toPlainText().toUtf8() == text.toUtf8(),
          "Composer undo removes only the last paste despite display highlighting");
    edit->redo();
    check(edit->toPlainText().toUtf8() == (text + suffix).toUtf8(),
          "Composer redo restores the exact pasted Unicode body");
    edit->undo();
    edit->moveCursor(QTextCursor::End);
    QKeyEvent newline(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier, "\n");
    QApplication::sendEvent(edit, &newline);
    edit->insertPlainText(QStringLiteral("尾行"));
    check(sent == 0 && edit->toPlainText() == text + QStringLiteral("\n尾行"),
          "Shift+Enter inserts a newline without sending");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QApplication::sendEvent(edit, &enter);
    check(sent == 1 && sent_text == text + QStringLiteral("\n尾行") && edit->toPlainText() == sent_text && !send->isEnabled(),
          "Enter submits the complete multiline draft and retains it until confirmation");
    page.finish_message_send(direct.id, 1, 1, sent_text, {}, {}, {});
    check(edit->toPlainText().isEmpty(), "Confirmed sending clears the matching draft");
    wait([&] { return edit->height() == compact_height; });
    check(!send->isEnabled(), "Cleared composer disables sending");
    auto const long_text = QStringLiteral("长内容 中文 🙂\n").repeated(20);
    edit->setPlainText(long_text);
    wait([&] { return edit->verticalScrollBar()->maximum() > 0; });
    auto const bounded_height = edit->height();
    check(bounded_height <= 160 && edit->toPlainText() == long_text, "Long drafts scroll without truncation or unlimited growth");
    edit->setPlainText(QStringLiteral("长内容 中文 🙂\n").repeated(40));
    check(edit->height() == bounded_height, "Extra lines do not further enlarge the composer");
    page.resize(1920, 1080);
    edit->setPlainText(QStringLiteral("中文混合 ") + QStringLiteral("自然换行").repeated(12));
    QApplication::processEvents();
    auto const wide_height = edit->height();
    page.resize(980, 640);
    wait([&] { return edit->height() > wide_height; });
    auto* bar = page.findChild<QFrame*>("inputBar");
    wait([&] { return bar->rect().contains(send->geometry()) && bar->rect().contains(edit->geometry()); });
    auto const draft = edit->toPlainText();
    page.set_connection_available(false);
    QApplication::sendEvent(edit, &enter);
    check(sent == 1 && !edit->isEnabled() && !send->isEnabled() && edit->toPlainText() == draft,
          "Offline composer preserves the draft and refuses sending");
    page.set_connection_available(true);
    check_preedit_display(edit);
    edit->setPlainText(QStringLiteral("准备 "));
    edit->moveCursor(QTextCursor::End);
    QApplication::processEvents();
    QString const before_preedit = edit->toPlainText();
    QString const emoji_preedit = QStringLiteral("👩‍💻 é 👍️🏽");
    QTextCharFormat composition_format;
    composition_format.setUnderlineStyle(QTextCharFormat::SingleUnderline);
    QInputMethodEvent composing(emoji_preedit, {
        QInputMethodEvent::Attribute(QInputMethodEvent::TextFormat, 0, static_cast<int>(emoji_preedit.size()), composition_format)});
    QApplication::sendEvent(edit, &composing);
    QApplication::processEvents();
    check(edit->toPlainText().toUtf8() == before_preedit.toUtf8() && !edit->document()->isUndoAvailable() &&
              edit->textCursor().block().layout()->preeditAreaText() == emoji_preedit,
          "Unicode preedit remains a display composition without raw-text or undo mutations");
    QApplication::sendEvent(edit, &enter);
    check(sent == 1, "Enter cannot send an uncommitted Unicode composition");
    QInputMethodEvent emoji_commit;
    emoji_commit.setCommitString(emoji_preedit);
    QApplication::sendEvent(edit, &emoji_commit);
    QApplication::processEvents();
    check(edit->toPlainText().toUtf8() == (before_preedit + emoji_preedit).toUtf8() &&
              edit->textCursor().block().layout()->preeditAreaText().isEmpty(),
          "Input-method commit preserves exact Unicode UTF-8 and clears display preedit");
    edit->undo();
    QApplication::processEvents();
    check(edit->toPlainText().toUtf8() == before_preedit.toUtf8() && !edit->document()->isUndoAvailable(),
          "One composition undo restores the original draft without a highlighting undo step");
    edit->moveCursor(QTextCursor::End);
    QInputMethodEvent preedit(QStringLiteral("zhong"), {});
    QApplication::sendEvent(edit, &preedit);
    QApplication::sendEvent(edit, &enter);
    check(sent == 1, "Enter cannot send while the input method has uncommitted text");
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("中"));
    QApplication::sendEvent(edit, &commit);
    QApplication::sendEvent(edit, &enter);
    check(sent == 2 && sent_text == QStringLiteral("准备 中"), "Committed input method text can be sent normally");
    std::cout << "PASS Qt multiline composer, keyboard, bounded wrapping, offline and input method\n";
}

void check_message_viewport()
{
    chat_widget page;
    page.setStyleSheet(chat_style_sheet());
    page.resize(1180, 760);
    page.set_user(QStringLiteral("本人"), 1);
    conversation_data direct;
    direct.id = 50;
    direct.user = 2;
    direct.username = QStringLiteral("朋友");
    direct.can_send = true;
    page.open_conversation(direct);
    page.show();
    QList<message_data> history;
    for (int i = 1; i <= 40; ++i)
    {
        message_data item;
        item.id = i;
        item.conversation = direct.id;
        item.from = 2;
        item.username = direct.username;
        item.timestamp = i;
        item.text = QStringLiteral("历史消息 %1\n可滚动的中文正文").arg(i);
        history.push_back(std::move(item));
    }
    page.set_messages(direct.id, std::move(history), {}, false, false, false);
    auto* list = page.findChild<QListView*>("messageList");
    auto* scroll = list->verticalScrollBar();
    auto* edit = page.findChild<QPlainTextEdit*>("messageEdit");
    auto* send = page.findChild<QToolButton*>("sendButton");
    wait([&] { return scroll->maximum() > 200 && scroll->value() == scroll->maximum(); });
    edit->setPlainText(QStringLiteral("从最新位置发送"));
    QApplication::processEvents();
    list->scrollToBottom();
    send->click();
    QApplication::processEvents();
    check(scroll->value() == scroll->maximum(),
          "Sending feedback preserves the latest-message position in scrollable history");
    page.finish_message_send(direct.id, 41, 41, edit->toPlainText(), {}, {}, {});
    wait([&] { return scroll->value() == scroll->maximum(); });
    page.set_typing(direct.id, 2, direct.username, true);
    QApplication::processEvents();
    check(scroll->value() == scroll->maximum(), "Typing feedback keeps the latest message visible");
    page.set_typing(direct.id, 2, direct.username, false);
    QApplication::processEvents();
    check(scroll->value() == scroll->maximum(), "Removing typing feedback keeps the latest position");
    auto const compact_height = edit->height();
    edit->setPlainText(QStringLiteral("下一条草稿\n").repeated(5));
    wait([&] { return edit->height() > compact_height && scroll->value() == scroll->maximum(); });
    edit->clear();
    wait([&] { return edit->height() == compact_height && scroll->value() == scroll->maximum(); });
    for (auto const size : {QSize(980, 640), QSize(1440, 900), QSize(1920, 1080), QSize(1180, 760)})
    {
        page.resize(size);
        wait([&] { return page.size() == size && scroll->value() == scroll->maximum(); });
    }
    message_data incoming;
    incoming.id = 42;
    incoming.conversation = direct.id;
    incoming.from = 2;
    incoming.username = direct.username;
    incoming.timestamp = 42;
    incoming.text = QStringLiteral("对方的新消息");
    page.add_message(direct.id, incoming);
    auto const latest = list->model()->index(list->model()->rowCount() - 1, 0);
    wait([&] { return scroll->value() == scroll->maximum() &&
        list->visualRect(latest).intersects(list->viewport()->rect()); });
    check(list->visualRect(latest).intersects(list->viewport()->rect()), "The new message is actually in the viewport");
    auto* cancel_reply = page.findChild<QToolButton*>("cancelReplyButton");
    for (bool const at_bottom : {true, false})
    {
        if (at_bottom) { list->scrollToBottom(); }
        else { scroll->setValue(scroll->maximum() - 180); }
        auto const reply_position = scroll->value();
        auto const target = list->indexAt(QPoint(list->viewport()->width() / 2, list->viewport()->height() / 2));
        check(target.isValid(), "Reply targets an actually visible message");
        QTimer::singleShot(0, &page, [] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            check(menu, "Reply uses the actual message menu");
            auto const actions = menu->actions();
            auto const action = std::ranges::find_if(actions, [](auto* value) {
                return value->text() == QStringLiteral("回复");
            });
            check(action != actions.cend(), "The visible message can be replied to");
            menu->setActiveAction(*action);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &enter);
        });
        list->customContextMenuRequested(list->visualRect(target).intersected(list->viewport()->rect()).center());
        wait([&] { return cancel_reply->isVisible() && (at_bottom
            ? scroll->value() == scroll->maximum() : scroll->value() == reply_position); });
        cancel_reply->click();
        wait([&] { return !cancel_reply->isVisible() && (at_bottom
            ? scroll->value() == scroll->maximum() : scroll->value() == reply_position); });
    }
    scroll->setValue(scroll->maximum() - 180);
    auto const history_position = scroll->value();
    page.set_typing(direct.id, 2, direct.username, true);
    edit->setPlainText(QStringLiteral("阅读历史时发送"));
    send->click();
    QApplication::processEvents();
    check(scroll->value() == history_position && scroll->value() < scroll->maximum(),
          "Sending and typing feedback do not pull a history reader to the latest message");
    page.finish_message_send(direct.id, 43, 43, edit->toPlainText(), {}, {}, {});
    incoming.id = 44;
    page.add_message(direct.id, incoming);
    QApplication::processEvents();
    check(scroll->value() == history_position, "Confirmed and incoming messages preserve an explicit history position");
    list->scrollToBottom();
    page.resize(980, 640);
    scroll->setValue(scroll->maximum() - 180);
    auto const interrupted_position = scroll->value();
    QApplication::processEvents();
    check(scroll->value() == interrupted_position,
          "A user scroll after resize cancels pending latest-position restoration");
    list->scrollToBottom();
    incoming.id = 45;
    page.add_message(direct.id, incoming);
    scroll->setValue(scroll->maximum() - 180);
    auto const incoming_interrupted_position = scroll->value();
    QApplication::processEvents();
    check(scroll->value() == incoming_interrupted_position,
          "A user scroll after an incoming message cancels pending latest-position restoration");
    std::cout << "PASS Qt latest-message following and explicit history reading\n";
}

void check_conversation_drafts()
{
    chat_widget page;
    page.setStyleSheet(chat_style_sheet());
    page.resize(1180, 760);
    page.set_user(QStringLiteral("本人"), 1);
    conversation_data first;
    first.id = 50;
    first.user = 2;
    first.username = QStringLiteral("朋友");
    first.can_send = true;
    conversation_data second;
    second.id = 60;
    second.group = true;
    second.username = QStringLiteral("群聊");
    second.can_send = true;
    auto* edit = page.findChild<QPlainTextEdit*>("messageEdit");
    page.open_conversation(first);
    page.show();
    QApplication::processEvents();
    edit->setPlainText(QStringLiteral("给朋友的草稿\n还没有发送"));
    page.open_conversation(second);
    check(edit->toPlainText().isEmpty(), "Changing conversations does not carry a draft to another recipient");
    edit->setPlainText(QStringLiteral("给群聊的草稿"));
    page.open_conversation(first);
    check(edit->toPlainText() == QStringLiteral("给朋友的草稿\n还没有发送"), "Returning restores that conversation's draft");
    wait([&] { return edit->verticalScrollBar()->maximum() == 0; });
    page.open_conversation(second);
    check(edit->toPlainText() == QStringLiteral("给群聊的草稿"), "Each conversation retains its own draft");
    page.close_conversation(first.id);
    page.open_conversation(first);
    check(edit->toPlainText().isEmpty(), "Closing an inactive conversation discards its draft");
    page.set_conversations({first});
    page.open_conversation(second);
    check(edit->toPlainText().isEmpty(), "Authoritative removal discards an inactive group's draft");
    edit->setPlainText(QStringLiteral("上一个账号的草稿"));
    page.open_conversation(first);
    page.set_user(QStringLiteral("另一账号"), 3);
    page.open_conversation(second);
    check(edit->toPlainText().isEmpty(), "Changing accounts cannot restore the previous account's draft");
    auto* send = page.findChild<QToolButton*>("sendButton");
    int requests = 0;
    QObject::connect(&page, &chat_widget::send_message_requested, &page, [&](auto...) { ++requests; });
    auto const failed_text = QStringLiteral("失败后继续编辑");
    edit->setPlainText(failed_text);
    send->click();
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QApplication::sendEvent(edit, &enter);
    check(requests == 1 && !send->isEnabled() && edit->isEnabled() && edit->toPlainText() == failed_text,
          "Pending sending preserves editable text and prevents duplicate submission");
    page.finish_message_send(second.id, 0, 0, failed_text, {}, {}, QStringLiteral("Invalid params"));
    check(send->isEnabled() && edit->toPlainText() == failed_text, "Rejected sending leaves the draft ready for manual correction");
    send->click();
    edit->setPlainText(QStringLiteral("等待期间写的新草稿"));
    page.finish_message_send(second.id, 1, 1, failed_text, {}, {}, {});
    check(requests == 2 && send->isEnabled() && edit->toPlainText() == QStringLiteral("等待期间写的新草稿"),
          "Confirmation cannot erase text edited after submission");
    auto const background_text = QStringLiteral("发给群的消息");
    edit->setPlainText(background_text);
    send->click();
    page.open_conversation(first);
    edit->setPlainText(QStringLiteral("另一会话的草稿"));
    page.finish_message_send(second.id, 2, 2, background_text, {}, {}, {});
    check(edit->toPlainText() == QStringLiteral("另一会话的草稿"), "Background confirmation does not modify the current conversation");
    page.open_conversation(second);
    check(edit->toPlainText().isEmpty(), "Background confirmation clears only the submitted conversation's matching draft");
    edit->setPlainText(failed_text);
    send->click();
    page.open_conversation(first);
    page.finish_message_send(second.id, 0, 0, failed_text, {}, {}, QStringLiteral("Invalid params"));
    page.open_conversation(second);
    check(edit->toPlainText() == failed_text && send->isEnabled(), "Background rejection retains the original conversation's draft");
    send->click();
    page.set_connection_available(false);
    check(edit->toPlainText() == failed_text && !edit->isEnabled(), "Disconnect keeps unconfirmed text");
    page.set_connection_available(true);
    check(edit->toPlainText() == failed_text && send->isEnabled(), "Reconnect leaves the draft available without automatic resending");
    std::cout << "PASS Qt conversation draft ownership, closure and account isolation\n";
}

int main(int argc, char** argv)
{
    bool const widgets_only = argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--widgets-only");
    if (!widgets_only && argc != 3)
    {
        return 1;
    }
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (widgets_only)
    {
        try { check_authentication_layout(); check_friend_request_layout(); check_group_detail_layout(); check_primary_navigation(); check_profile_layout(); check_confirmation_dialogs(); check_message_editor(); check_reply_and_read_details_controls(); check_message_action_targets(); check_message_dialogs(); check_message_composer(); check_message_viewport(); check_conversation_drafts(); check_message_search_keyboard_visibility(); check_message_search_live_policy(); check(check_pinned_unicode_boundaries(), "Pinned summaries and older-message queries omit every incomplete boundary cluster"); return 0; }
        catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
    }
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
        check_authentication_layout();
        check_friend_request_layout();
        check_group_detail_layout();
        check_primary_navigation();
        check_profile_layout();
        check_confirmation_dialogs();
        check_message_editor();
        check_reply_and_read_details_controls();
        check_message_action_targets();
        check_message_dialogs();
        check_message_composer();
        check_message_viewport();
        check_conversation_drafts();
        check_message_search_keyboard_visibility();
        check_message_search_live_policy();
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
            auto const background = logical_pixel(image, QPoint(10, entry->height() / 2));
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
            auto* create = windows[0]->findChild<QAction*>("createGroupAction");
            check(create, "Chats menu create group action");
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
            create->trigger();
            wait([&] { return pages[0]->active_conversation() > 0 && pages[0]->messages_ready(); });
            group = pages[0]->active_conversation();
            for (int i = 1; i < 3; ++i)
            {
                auto* view = windows[i]->findChild<QListView*>("conversationList");
                wait([&] { return view->model()->rowCount() == 1; });
                click_list_body(view, view->model()->index(0, 0));
                wait([&] { return pages[i]->active_conversation() == group && pages[i]->messages_ready(); });
            }
            auto* edit = windows[0]->findChild<QPlainTextEdit*>("messageEdit");
            auto type_character = [&](int actor) {
                auto* input = windows[actor]->findChild<QPlainTextEdit*>("messageEdit");
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
            check(edit->toPlainText() == "x", "Typing ends after idle without discarding the draft");
            type_character(0);
            wait([&] { return peer_typing->isVisible(); });
            edit->setPlainText(QStringLiteral("Qt 群消息验证"));
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
            auto const rejected_draft = QString(64 * 1024 - 256, QLatin1Char('x'));
            edit->setPlainText(rejected_draft);
            windows[0]->findChild<QToolButton*>("sendButton")->click();
            wait([&] {
                return std::ranges::any_of(pages[0]->findChildren<QLabel*>(), [](auto* label) {
                    return label->text().contains(QStringLiteral("Invalid params"));
                });
            });
            check(edit->toPlainText() == rejected_draft && receipt_view->model()->rowCount() == 1,
                "Server rejection preserves the complete draft without adding a message");
            edit->clear();
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
                              list->item(0)->data(Qt::StatusTipRole).toString().contains(QStringLiteral("群主")) &&
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
                         list->item(1)->data(Qt::StatusTipRole).toString().contains(QStringLiteral("管理员")))
                {
                    dialog->grab().save(QString::fromLocal8Bit(argv[2]) + "/qt_group_roles.png");
                    ++role_step;
                    button->click();
                }
                else if (role_step == 2 && button->isEnabled() &&
                         !list->item(1)->data(Qt::StatusTipRole).toString().contains(QStringLiteral("管理员")))
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
            auto const avatar_color = avatar_image.scaled(208, 208, Qt::KeepAspectRatio, Qt::SmoothTransformation).pixelColor(0, 0);
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
                check(list->item(0)->data(Qt::DecorationRole).value<QPixmap>().toImage() == pages[1]->avatars().image(ids[0]).toImage(), "Group member avatar");
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
            bool search_profile_opened = false;
            QTimer::singleShot(30, windows[2].get(), [&] {
                inspect_noncontact_profile(names[1], true);
                search_profile_opened = true;
            });
            search_view->scrollTo(search_view->model()->index(0, 0));
            QApplication::processEvents();
            QStyleOptionViewItem search_option;
            search_option.rect = search_view->visualRect(search_view->model()->index(0, 0));
            auto const search_avatar_point = QPoint(search_option.rect.left() + chat_theme::dialog_left + 4,
                                                    search_option.rect.top() + chat_theme::dialog_avatar_top + 4);
            QMouseEvent avatar_click(QEvent::MouseButtonRelease, search_avatar_point,
                search_view->viewport()->mapToGlobal(search_avatar_point), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QMouseEvent avatar_press(QEvent::MouseButtonPress, search_avatar_point,
                search_view->viewport()->mapToGlobal(search_avatar_point), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(search_view->viewport(), &avatar_press);
            check(QApplication::sendEvent(search_view->viewport(), &avatar_click), "Search result avatar opens public profile");
            check(search_profile_opened, "A real search avatar click completes the profile and friendship action flow");
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
                check(profile->findChild<QLabel*>("profileDialogName")->text() == names[1], "Accepted search result profile identity");
                check(message && profile->findChild<QPushButton*>("removeContactButton")->isVisible(),
                    "Accepted search result offers messaging and friend removal");
                profile->reject();
            });
            click_list_body(search_view, search_view->model()->index(0, 0));
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
            windows[1]->findChild<QPlainTextEdit*>("messageEdit")->setPlainText(QStringLiteral("Qt 引用回复"));
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
            auto* pending_reply_draft = windows[2]->findChild<QPlainTextEdit*>("messageEdit");
            pending_reply_draft->setPlainText(QStringLiteral("尚未发送的草稿"));
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
            check(!pending_reply_preview->isVisible() && pending_reply_draft->toPlainText() == QStringLiteral("尚未发送的草稿"),
                "Deleting a prepared reply target clears the invalid reference and preserves the draft");
            windows[2]->findChild<QPlainTextEdit*>("messageEdit")->setPlainText(QStringLiteral("离线编辑验证"));
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
            wait([&] { return !windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled(); });
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
                        return windows[i]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled() &&
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
            windows[1]->findChild<QPlainTextEdit*>("messageEdit")->setPlainText(QStringLiteral("重连后的群消息 @") + names[2]);
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
            click_list_body(contact_view, contact_view->model()->index(0, 0));
            wait([&] { return pages[0]->active_conversation() != group && pages[0]->messages_ready(); });
            wait([&] { return !group_typing->isVisible(); });
            auto const direct = pages[0]->active_conversation();
            wait([&] { return pages[0]->conversation(direct) && pages[0]->conversation(direct)->can_send; });
            check(pages[0]->active_conversation() == direct &&
                      windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled() &&
                      windows[0]->findChild<QListView*>("messageList")->model()->rowCount() == 0,
                  "An authoritative snapshot preserves a newly opened empty direct chat");
            windows[0]->findChild<QPlainTextEdit*>("messageEdit")->setPlainText(QStringLiteral("保留单聊历史"));
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
            check(!windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled() &&
                !windows[0]->findChild<QToolButton*>("sendButton")->isEnabled() &&
                !windows[0]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled() &&
                windows[0]->findChild<QPlainTextEdit*>("messageEdit")->placeholderText().contains(QStringLiteral("还不是好友")),
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
                check(!windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled(), "Adding is not optimistic authorization");
                wait([&] { return action->text() == QStringLiteral("等待验证"); });
                accept_friend(1, 0);
                wait([&] { return action->text() == QStringLiteral("消息") && action->isEnabled(); });
                action->click();
            });
            windows[0]->findChild<QPushButton*>("chatHeaderButton")->click();
            wait([&] { return pages[0]->conversation(direct) && pages[0]->conversation(direct)->can_send &&
                windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled(); });
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
            click_list_body(peer_conversations, direct_index);
            pages[1]->contact_remove_requested(ids[0]);
            wait([&] { return avatar_contacts->model()->rowCount() == 0 && !pages[1]->conversation(direct)->can_send; });
            auto* peer_attachment_view = windows[1]->findChild<QListView*>("messageList");
            wait([&] { return pages[1]->active_conversation() == direct && pages[1]->messages_ready() &&
                                  peer_attachment_view->model()->rowCount() == 2; });
            check(!pages[1]->conversation(direct)->can_send &&
                !windows[1]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled() &&
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
                check(!windows[1]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled(), "Add does not optimistically grant send");
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
            check(!windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled() &&
                !windows[0]->findChild<QToolButton*>("sendButton")->isEnabled(),
                "Reconnect cannot re-enable cached send permission before the authoritative snapshot");
            std::unique_ptr<PGresult, decltype(&PQclear)> released_snapshot(PQexec(db, "COMMIT"), &PQclear);
            check(released_snapshot && PQresultStatus(released_snapshot.get()) == PGRES_COMMAND_OK, "Release reconnect snapshot");
            wait([&] { return pages[0]->messages_ready() && !pages[0]->conversation(direct)->can_send; });
            check(!windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled() &&
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
                windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled(); });

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
                click_list_body(list, selected);
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
                         list->item(1)->data(Qt::StatusTipRole).toString().contains(QStringLiteral("管理员")))
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
                    check(list->item(1)->data(Qt::StatusTipRole).toString().contains(QStringLiteral("管理员")), "Realtime role visible to administrator");
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
            windows[2]->findChild<QPlainTextEdit*>("messageEdit")->setPlainText(QStringLiteral("退出后清除的草稿"));
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
                      windows[2]->findChild<QPlainTextEdit*>("messageEdit")->toPlainText().isEmpty() &&
                      !windows[2]->findChild<QToolButton*>("sendButton")->isEnabled() &&
                      !windows[2]->findChild<QToolButton*>("sendAttachmentButton")->isEnabled(),
                  "Leaving clears active history, draft and sending controls");
            wait([&] { return windows[0]->findChild<QLabel*>("chatPresence")->text().contains(QStringLiteral("2 名成员")); });
            wait([&] { return windows[2]->findChild<QListView*>("conversationList")->model()->rowCount() == 0; });
            auto const before_leave_message = pages[0]->latest_message_id();
            windows[0]->findChild<QPlainTextEdit*>("messageEdit")->setPlainText(QStringLiteral("退出期间的群消息"));
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
                        row >= 0 && list->item(row)->data(Qt::StatusTipRole).toString().contains(QStringLiteral("群主"))))
                    {
                        if (button_name == "groupTransferButton")
                        {
                            check(list->item(1)->data(Qt::UserRole).toLongLong() == ids[0] && list->item(1)->data(Qt::StatusTipRole).toString().contains(QStringLiteral("管理员")), "Former Qt owner becomes admin");
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
                    windows[2]->findChild<QPlainTextEdit*>("messageEdit")->setPlainText(QStringLiteral("移除后清理的草稿"));
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
                    windows[2]->findChild<QPlainTextEdit*>("messageEdit")->toPlainText().isEmpty() &&
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
                windows[2]->findChild<QAction*>("joinGroupAction")->trigger();
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
            wait([&] { return !windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled(); });
            start();
            for (int i = 0; i < 2; ++i)
            { wait([&, i] { return pages[i]->messages_ready() && windows[i]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled(); }); }
            wait([&] { return windows[2]->findChild<QAction*>("joinGroupAction")->isEnabled(); });
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
            wait([&] { return !windows[0]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled(); });
            start();
            for (int i = 0; i < 3; ++i)
            {
                wait([&, i] { return pages[i]->messages_ready() && windows[i]->findChild<QPlainTextEdit*>("messageEdit")->isEnabled(); });
            }
            bool recovered_owner = false;
            QTimer owner_poll;
            QObject::connect(&owner_poll, &QTimer::timeout, [&] {
                auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                if (!dialog) { return; }
                auto* list = dialog->findChild<QListWidget*>("groupMembersList");
                if (list->count() != 3) { return; }
                check(list->item(0)->data(Qt::StatusTipRole).toString().contains(QStringLiteral("群主")) &&
                    list->item(1)->data(Qt::StatusTipRole).toString().contains(QStringLiteral("管理员")), "Ownership roles recover in Qt role groups after reconnect");
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
            click_list_body(direct_list, notice_conversation);
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
            windows[0]->activateWindow();
            QApplication::processEvents();
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
            windows[0]->activateWindow();
            QApplication::processEvents();
            check(windows[0]->findChild<QLineEdit*>("loginUsernameEdit")->hasFocus(),
                  "Confirmed logout returns keyboard focus to the login identity field");
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

            bool const pinned_unicode_ok = check_pinned_unicode_boundaries(QString::fromLocal8Bit(argv[2]));
            check(!pages[1]->conversation(group)->muted, "Unicode notification fixture uses the existing unmuted group");
            QModelIndex unicode_receiver_direct;
            wait([&] {
                auto* list = windows[1]->findChild<QListView*>("conversationList");
                for (int row = 0; row < list->model()->rowCount(); ++row)
                {
                    auto const index = list->model()->index(row, 0);
                    if (index.data(conversation_model::id_role).toLongLong() == direct)
                    { unicode_receiver_direct = index; return true; }
                }
                return false;
            });
            click_list_body(windows[1]->findChild<QListView*>("conversationList"), unicode_receiver_direct);
            wait([&] { return pages[1]->active_conversation() == direct && pages[1]->messages_ready(); });
            select_group(0);
            bool notification_unicode_ok = true;
            QString const title_prefix(79, QLatin1Char('x'));
            QString const body_prefix(119, QLatin1Char('x'));
            for (auto const& [name, cluster] : unicode_boundary_samples())
            {
                QString const full_title = title_prefix + cluster + QStringLiteral("群尾");
                bool renamed = false, requested = false, rename_timed_out = false;
                QTimer rename_poll, rename_watchdog;
                QObject::connect(&rename_poll, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<group_dialog*>(QApplication::activeModalWidget());
                    if (!dialog) { return; }
                    auto* input = dialog->findChild<QLineEdit*>("groupTitleEdit");
                    auto* button = dialog->findChild<QPushButton*>("groupRenameButton");
                    if (!requested && input->isEnabled())
                    {
                        input->setText(full_title);
                        check(button->isEnabled(), "A legal Unicode boundary group title is accepted by the actual rename control");
                        requested = true; button->click();
                    }
                    else if (requested && pages[0]->conversation(group)->username == full_title)
                    { renamed = true; rename_poll.stop(); dialog->accept(); }
                });
                rename_watchdog.setSingleShot(true);
                QObject::connect(&rename_watchdog, &QTimer::timeout, [&] {
                    rename_timed_out = true;
                    if (auto* dialog = QApplication::activeModalWidget()) { qobject_cast<QDialog*>(dialog)->reject(); }
                });
                rename_poll.start(20); rename_watchdog.start(5000);
                windows[0]->findChild<QPushButton*>("chatHeaderButton")->click();
                rename_poll.stop(); rename_watchdog.stop();
                check(renamed && !rename_timed_out, "Notification title fixture is persisted through the real group rename dialog");
                wait([&] { return pages[1]->conversation(group)->username == full_title; });
                auto const previous_notices = notifications[1].size();
                auto const sender_notices_before = notifications[0].size();
                QString const full_body = body_prefix + cluster + QStringLiteral("正文尾\n第二行");
                windows[0]->findChild<QPlainTextEdit*>("messageEdit")->setPlainText(full_body);
                windows[0]->findChild<QToolButton*>("sendButton")->click();
                wait([&] { return notifications[1].size() == previous_notices + 1; });
                auto* sender_messages = windows[0]->findChild<QListView*>("messageList");
                wait([&] {
                    return sender_messages->model()->rowCount() > 0 && sender_messages->model()
                        ->index(sender_messages->model()->rowCount() - 1, 0).data(message_model::text_role).toString() == full_body;
                });
                check(pages[0]->conversation(group)->username.toUtf8() == full_title.toUtf8() &&
                          pages[1]->conversation(group)->username.toUtf8() == full_title.toUtf8() &&
                          sender_messages->model()->index(sender_messages->model()->rowCount() - 1, 0)
                              .data(message_model::text_role).toString().toUtf8() == full_body.toUtf8() &&
                          notifications[0].size() == sender_notices_before,
                      "Notification truncation preserves the complete persisted Unicode title/body and excludes the actual sender");
                auto const& notice = notifications[1].back();
                bool const title_ok = notice.title == title_prefix + QStringLiteral(" · ") + names[0];
                bool const body_ok = notice.summary == body_prefix;
                check(notice.conversation == group, "Unicode boundary notification retains its real conversation identity");
                std::cout << (title_ok && body_ok ? "PASS" : "RED") << " Qt real notification Unicode " << name.toStdString()
                          << " title=" << title_ok << " body=" << body_ok << '\n';
                notification_unicode_ok = notification_unicode_ok && title_ok && body_ok;
                check(windows[0]->grab().save(QString::fromLocal8Bit(argv[2]) + QStringLiteral("/qt_notification_unicode_") + name + QStringLiteral(".png")),
                      "The real sender controls retain an original Unicode notification-fixture capture");
            }
            check(pinned_unicode_ok && notification_unicode_ok,
                  "Pinned controls and real notification arguments omit incomplete boundary clusters without changing original Unicode");
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
