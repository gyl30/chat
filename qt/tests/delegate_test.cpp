#include <iostream>

#include <QApplication>
#include <QFontMetrics>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <chat/attachment.hpp>

#include "message_delegate.hpp"
#include "avatar.hpp"
#include "user_model.hpp"
#include "user_delegate.hpp"
#include "conversation_model.hpp"
#include "conversation_delegate.hpp"
#include "../../tests/avatar_fixture.hpp"
#include "message_model.hpp"
#include "theme.hpp"
#include "message_images.hpp"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    avatar_cache avatars;
    int downloads = 0;
    QObject::connect(&avatars, &avatar_cache::requested, &avatars, [&](qint64, qint64) { ++downloads; });
    auto const png = QByteArray::fromBase64(QByteArray(avatar_png_base64.data(), avatar_png_base64.size()));
    message_images pictures;
    QList<qint64> image_requests;
    QObject::connect(&pictures, &message_images::requested, &pictures, [&](qint64 conversation, qint64 message) {
        if (conversation != 70) { std::abort(); }
        image_requests.push_back(message);
    });
    for (qint64 id : {1, 2, 3, 4}) { pictures.observe(70, id); pictures.observe(70, id); }
    if (image_requests != QList<qint64>{1, 2, 3}) { return 1; }
    pictures.receive(71, 1, png, {});
    if (!pictures.image(1).isNull()) { return 1; }
    pictures.receive(70, 1, png, {});
    if (pictures.image(1).isNull() || pictures.bytes(1) != png || image_requests != QList<qint64>{1, 2, 3, 4}) { return 1; }
    pictures.observe(70, 1);
    if (image_requests.size() != 4) { return 1; }
    pictures.receive(70, 2, QByteArray::fromHex("89504e470d0a1a0a"), {});
    pictures.receive(70, 3, {}, QStringLiteral("download failed"));
    if (!pictures.image(2).isNull() || pictures.status(2).isEmpty() || pictures.status(3).isEmpty()) { return 1; }
    pictures.retry();
    pictures.receive(70, 4, png, {});
    if (!pictures.image(4).isNull() || pictures.image(1).isNull()) { return 1; }
    pictures.observe(70, 4);
    pictures.receive(70, 4, png, {});
    if (pictures.image(4).isNull()) { return 1; }
    message_model image_messages(nullptr, nullptr, &pictures);
    image_messages.reset(70);
    message_data photo;
    photo.id = 1;
    photo.conversation = 70;
    photo.from = 2;
    photo.username = "image sender";
    photo.attachment = attachment_data{"photo.png", "image/png", png.size()};
    image_messages.add_message(photo);
    QStyleOptionViewItem image_option;
    image_option.rect = QRect(0, 0, 640, 450);
    image_option.font = QApplication::font();
    message_delegate image_delegate;
    QImage photo_render(image_option.rect.size(), QImage::Format_ARGB32_Premultiplied);
    auto const pixmap_key = pictures.image(1).cacheKey();
    for (int i = 0; i < 10; ++i)
    {
        photo_render.fill(Qt::white);
        QPainter photo_painter(&photo_render);
        image_delegate.paint(&photo_painter, image_option, image_messages.index(0, 0));
    }
    if (pictures.image(1).cacheKey() != pixmap_key || image_requests.size() != 5 ||
        image_delegate.sizeHint(image_option, image_messages.index(0, 0)).height() < 240) { return 1; }
    int image_clicks = 0;
    QObject::connect(&image_delegate, &message_delegate::image_clicked, &image_delegate, [&](QModelIndex const& index) {
        if (index.data(message_model::id_role).toLongLong() != 1) { std::abort(); }
        ++image_clicks;
    });
    QMouseEvent photo_click(QEvent::MouseButtonRelease, QPointF(150, 120), QPointF(150, 120),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    image_delegate.editorEvent(&photo_click, &image_messages, image_option, image_messages.index(0, 0));
    if (image_clicks != 1) { return 1; }
    photo.deleted = true;
    image_messages.update_message(photo);
    if (!pictures.bytes(1).isEmpty() || !image_messages.index(0, 0).data(message_model::image_role).value<QPixmap>().isNull()) { return 1; }
    pictures.remove(70);
    if (!pictures.bytes(4).isEmpty()) { return 1; }
    auto large_png = png + QByteArray(static_cast<qsizetype>(chat::max_attachment_size) - png.size(), '\0');
    for (qint64 id = 10; id < 18; ++id) { pictures.observe(70, id); pictures.receive(70, id, large_png, {}); }
    if (!pictures.bytes(10).isEmpty() || pictures.image(17).isNull()) { return 1; }
    pictures.clear();
    if (!pictures.bytes(17).isEmpty()) { return 1; }
    image_requests.clear();
    for (qint64 id = 20; id < 25; ++id) { pictures.observe(70, id); }
    pictures.discard_queued();
    pictures.observe(70, 30);
    pictures.receive(70, 20, png, {});
    pictures.receive(70, 21, png, {});
    pictures.receive(70, 22, png, {});
    if (image_requests != QList<qint64>{20, 21, 22, 30} || pictures.image(20).isNull()) { return 1; }
    pictures.clear();
    std::cout << "PASS image download deduplication, queue bound, immutable cache, paint, fallback, lifecycle and eviction\n";
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
    messages.set_members({{1, "self", {}, {}}, {2, QStringLiteral("成员"), {}, {}}});
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
    auto const original_height = delegate.sizeHint(option, messages.index(0, 0)).height();
    messages.set_reactions(message.id, 1, {{QStringLiteral("👍"), {1, 2}}, {QStringLiteral("❤️"), {3}}});
    if (delegate.sizeHint(option, messages.index(0, 0)).height() <= original_height) { return 1; }
    int reaction_clicks = 0;
    QObject::connect(&delegate, &message_delegate::reaction_clicked, &delegate,
                     [&](QModelIndex const& index, QString emoji) {
        if (index.data(message_model::id_role).toLongLong() != message.id || emoji != QStringLiteral("👍")) { std::abort(); }
        ++reaction_clicks;
    });
    for (int y = original_height - 20; y < delegate.sizeHint(option, messages.index(0, 0)).height() && !reaction_clicks; y += 2)
    {
        for (int x = 0; x < option.rect.width() && !reaction_clicks; x += 2)
        {
            QMouseEvent click(QEvent::MouseButtonRelease, QPointF(x, y), QPointF(x, y),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            delegate.editorEvent(&click, &messages, option, messages.index(0, 0));
        }
    }
    if (reaction_clicks != 1) { std::cerr << "FAIL reaction chip click\n"; return 1; }
    messages.set_reactions(message.id, 2, {});
    if (delegate.sizeHint(option, messages.index(0, 0)).height() != original_height) { return 1; }
    std::cout << "PASS Qt reaction chip layout, hit target and clear\n";
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
    auto const unmuted_rendering = rendered;
    conversations.set_muted(direct.id, true);
    rendered.fill(Qt::white);
    QPainter muted_painter(&rendered);
    conversation_painter.paint(&muted_painter, option, conversations.index(0, 0));
    muted_painter.end();
    if (rendered == unmuted_rendering) { std::cerr << "FAIL conversation mute indicator\n"; return 1; }
    conversations.set_muted(direct.id, false);
    conversations.set_pinned(direct.id, true);
    rendered.fill(Qt::white);
    QPainter pinned_painter(&rendered);
    conversation_painter.paint(&pinned_painter, option, conversations.index(0, 0));
    pinned_painter.end();
    if (rendered == unmuted_rendering) { std::cerr << "FAIL conversation pin indicator\n"; return 1; }
    conversations.set_pinned(direct.id, false);
    auto mentioned = message;
    mentioned.text = QStringLiteral("@自己\n多行 ") + QStringLiteral("正文 ").repeated(70);
    mentioned.mentions = {{1, QStringLiteral("自己")}};
    mentioned.edited_at = 200;
    messages.update_message(mentioned);
    rendered.fill(QColor(QStringLiteral("#F7F5EF")));
    QPainter mention_painter(&rendered);
    delegate.paint(&mention_painter, option, messages.index(0, 0));
    mention_painter.end();
    int blue_pixels = 0;
    for (int y = 0; y < rendered.height(); ++y)
    {
        for (int x = 0; x < rendered.width(); ++x)
        {
            blue_pixels += rendered.pixelColor(x, y) == QColor(QStringLiteral("#277399"));
        }
    }
    if (blue_pixels < 10 || delegate.sizeHint(option, messages.index(0, 0)).height() <= 100)
    {
        std::cerr << "FAIL persisted mention highlight and multiline layout\n";
        return 1;
    }
    messages.reset(1, true);
    messages.add_message(message);
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
