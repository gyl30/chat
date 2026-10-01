#include <iostream>

#include <QApplication>
#include <QFontMetrics>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionViewItem>

#include "message_delegate.hpp"
#include "avatar.hpp"
#include "user_model.hpp"
#include "user_delegate.hpp"
#include "conversation_model.hpp"
#include "conversation_delegate.hpp"
#include "../../tests/avatar_fixture.hpp"
#include "message_model.hpp"
#include "theme.hpp"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    avatar_cache avatars;
    int downloads = 0;
    QObject::connect(&avatars, &avatar_cache::requested, &avatars, [&](qint64, qint64) { ++downloads; });
    auto const png = QByteArray::fromBase64(QByteArray(avatar_png_base64.data(), avatar_png_base64.size()));
    for (int i = 0; i < 100; ++i) { avatars.observe(2, {1, true}); }
    if (downloads != 1 || !avatars.image(2).isNull()) { return 1; }
    avatars.receive(2, {1, true}, png);
    auto const green = avatars.image(2).toImage().pixelColor(0, 0);
    auto const icon = avatar_icon("bob", 44, avatars.image(2)).pixmap(44, 44).toImage();
    if (icon.pixelColor(22, 22) != green || icon.pixelColor(0, 0).alpha() != 0) { return 1; }
    avatars.retry();
    if (downloads != 1) { return 1; }
    user_model users(nullptr, &avatars);
    users.set_users({{2, "bob", false, 0, {1, true}}, {3, "other", false, 0, {}}});
    conversation_model conversations(nullptr, &avatars);
    conversation_data direct;
    direct.id = 1;
    direct.user = 2;
    direct.username = "bob";
    direct.avatar = {1, true};
    conversations.set_conversations({direct});
    int changed_users = 0;
    QObject::connect(&users, &QAbstractItemModel::dataChanged, &users,
        [&](QModelIndex const& first, QModelIndex const& last, auto const&) {
            if (first.row() != 0 || last.row() != 0) { std::abort(); }
            ++changed_users;
        });
    avatars.observe(2, {2, false});
    avatars.receive(2, {1, true}, png);
    if (!avatars.image(2).isNull() || changed_users != 1 || downloads != 1) { return 1; }
    avatars.observe(2, {3, true});
    avatars.receive(2, {3, true}, "broken png");
    for (int i = 0; i < 100; ++i) { avatars.observe(2, {3, true}); }
    if (!avatars.image(2).isNull() || downloads != 2) { return 1; }
    avatars.retry();
    avatars.receive(2, {3, true}, png);
    if (downloads != 3 || avatars.image(2).isNull() ||
        conversations.index(0, 0).data(Qt::DecorationRole).value<QPixmap>().isNull()) { return 1; }
    avatars.observe(3, {0, false});
    if (downloads != 3 || avatar_icon("other", 44).pixmap(44, 44).toImage().pixelColor(22, 22) == green) { return 1; }
    std::cout << "PASS Qt avatar fallback, circular image, one download per revision, ABA/stale protection and retry\n";

    message_model messages(nullptr, &avatars);
    messages.set_self_user(1);
    messages.reset(1, true);
    message_data message;
    message.id = 1;
    message.conversation = 1;
    message.from = 2;
    message.username = QStringLiteral("成员");
    message.avatar = {3, true};
    message.timestamp = 1;
    message.edited_at = 100;
    message.text = QStringLiteral("offline retained edit");
    messages.add_message(message);

    QStyleOptionViewItem option;
    option.rect = QRect(0, 0, 640, 300);
    option.font = QApplication::font();
    option.font.setPixelSize(14);
    QImage rendered(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(QColor(QStringLiteral("#F7F5EF")));
    QPainter painter(&rendered);
    message_delegate delegate;
    delegate.paint(&painter, option, messages.index(0, 0));
    painter.end();

    int read_clicks = 0;
    QObject::connect(&delegate, &message_delegate::read_details_clicked, &delegate,
                     [&](QModelIndex const& index) {
        if (index.data(message_model::id_role).toLongLong() != message.id) { std::abort(); }
        ++read_clicks;
    });
    QPoint read_point;
    for (int y = 0; y < delegate.sizeHint(option, messages.index(0, 0)).height() && read_clicks == 0; y += 4)
    {
        for (int x = 0; x < option.rect.width() && read_clicks == 0; x += 4)
        {
            QMouseEvent click(QEvent::MouseButtonRelease, QPointF(x, y), QPointF(x, y),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            delegate.editorEvent(&click, &messages, option, messages.index(0, 0));
            read_point = {x, y};
        }
    }
    if (read_clicks != 1) { std::cerr << "FAIL group read count click target\n"; return 1; }
    message_model direct_messages;
    direct_messages.set_self_user(1);
    direct_messages.reset(1);
    direct_messages.add_message(message);
    QMouseEvent direct_click(QEvent::MouseButtonRelease, QPointF(read_point), QPointF(read_point),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    delegate.editorEvent(&direct_click, &direct_messages, option, direct_messages.index(0, 0));
    if (read_clicks != 1) { std::cerr << "FAIL direct chat exposes group read detail\n"; return 1; }

    int green_pixels = 0;
    for (int y = 0; y < rendered.height(); ++y)
    {
        for (int x = 0; x < chat_theme::message_side_margin + chat_theme::message_avatar_size; ++x)
        {
            green_pixels += rendered.pixelColor(x, y) == green;
        }
    }
    if (green_pixels < 100) { std::cerr << "FAIL real message avatar paint\n"; return 1; }
    user_delegate user_painter;
    conversation_delegate conversation_painter;
    for (bool conversation : {false, true})
    {
        rendered.fill(Qt::white);
        QPainter avatar_painter(&rendered);
        if (conversation) { conversation_painter.paint(&avatar_painter, option, conversations.index(0, 0)); }
        else { user_painter.paint(&avatar_painter, option, users.index(0, 0)); }
        avatar_painter.end();
        if (rendered.pixelColor(chat_theme::dialog_left + chat_theme::dialog_avatar_size / 2,
                                chat_theme::dialog_avatar_top + chat_theme::dialog_avatar_size / 2) != green) { return 1; }
    }
    // Keep the edited-message rendering assertion on its original image.
    rendered.fill(QColor(QStringLiteral("#F7F5EF")));
    QPainter message_painter(&rendered);
    delegate.paint(&message_painter, option, messages.index(0, 0));
    message_painter.end();
    avatars.clear();
    avatars.receive(2, {3, true}, png);
    if (!avatars.image(2).isNull()) { return 1; }
    QFontMetrics metrics(option.font);
    QImage expected(metrics.horizontalAdvance(message.text), metrics.height(), rendered.format());
    expected.fill(Qt::white);
    QPainter reference(&expected);
    reference.setRenderHint(QPainter::Antialiasing);
    reference.setFont(option.font);
    reference.setPen(QColor(QStringLiteral("#26342E")));
    reference.drawText(expected.rect(), Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, message.text);
    reference.end();

    auto const text_left = chat_theme::message_side_margin + chat_theme::message_avatar_skip
        + chat_theme::message_padding_horizontal;
    for (int top = 0; top + expected.height() <= rendered.height(); ++top)
    {
        if (rendered.copy(text_left, top, expected.width(), expected.height()) == expected)
        {
            std::cout << "PASS Qt edited message renders the entire single-line body\n";
            return 0;
        }
    }
    std::cerr << "FAIL Qt edited message body is clipped or wrapped into an unallocated line\n";
    return 1;
}
