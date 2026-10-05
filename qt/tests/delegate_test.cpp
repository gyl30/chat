#include <iostream>

#include <QApplication>
#include <QBuffer>
#include <QFontMetrics>
#include <QImage>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QTextLayout>
#include <QtMath>
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
#include "icons.hpp"

bool check_message_selection_raster()
{
    message_model model;
    model.set_self_user(1);
    model.reset(50);
    message_data message;
    message.id = 100;
    message.conversation = 50;
    message.from = 2;
    message.username = QStringLiteral("朋友");
    message.text = QStringLiteral("搜索结果 é 👩‍💻");
    model.add_message(message);
    message_delegate delegate;
    QListView selectable;
    selectable.setSelectionMode(QAbstractItemView::SingleSelection);
    QListView normal;
    normal.setSelectionMode(QAbstractItemView::NoSelection);
    for (int width : {320, 640})
    {
        for (auto origin : {QPoint(0, 0), QPoint(17, 23)})
        {
            for (qreal ratio : {1.0, 2.0})
            {
                for (bool outgoing : {false, true})
                {
                    message.from = outgoing ? 1 : 2;
                    model.reset(50);
                    model.add_message(message);
                    auto const index = model.index(0, 0);
                    QStyleOptionViewItem option;
                    option.font = QApplication::font();
                    option.font.setPixelSize(14);
                    option.rect = QRect(origin, QSize(width, 200));
                    auto const hint = delegate.sizeHint(option, index);
                    option.rect.setHeight(hint.height());
                    auto paint = [&](QWidget const* widget, QStyle::State state) {
                        option.widget = widget;
                        option.state = state;
                        if (delegate.sizeHint(option, index) != hint) { std::abort(); }
                        QImage image(QSize(qCeil((width + origin.x() + 20) * ratio),
                                           qCeil((hint.height() + origin.y() + 20) * ratio)),
                                     QImage::Format_ARGB32_Premultiplied);
                        image.setDevicePixelRatio(ratio);
                        image.fill(QColor(QStringLiteral("#F7F5EF")));
                        QPainter painter(&image);
                        delegate.paint(&painter, option, index);
                        return image;
                    };
                    auto const plain = paint(&selectable, QStyle::State_Enabled);
                    auto const selected = paint(&selectable, QStyle::State_Enabled | QStyle::State_Selected);
                    auto const focus = paint(&selectable, QStyle::State_Enabled | QStyle::State_HasFocus);
                    auto const combined = paint(&selectable, QStyle::State_Enabled | QStyle::State_Selected | QStyle::State_HasFocus);
                    if (plain == selected || plain == focus || selected == combined)
                    {
                        std::cerr << "FAIL selectable message selection and focus are not separately visible\n";
                        return false;
                    }
                    if (paint(nullptr, QStyle::State_Enabled | QStyle::State_Selected) != selected ||
                        paint(nullptr, QStyle::State_Enabled | QStyle::State_HasFocus) != focus)
                    {
                        std::cerr << "FAIL null-widget message paint ignores standard style states\n";
                        return false;
                    }
                    for (auto state : {QStyle::State(QStyle::State_Enabled | QStyle::State_Selected),
                                       QStyle::State(QStyle::State_Enabled | QStyle::State_HasFocus),
                                       QStyle::State(QStyle::State_Enabled | QStyle::State_Selected | QStyle::State_HasFocus)})
                    {
                        if (paint(&normal, state) != plain)
                        {
                            std::cerr << "FAIL NoSelection message changes its existing rendering\n";
                            return false;
                        }
                    }
                    QRect const inner(qRound((option.rect.left() + 8) * ratio),
                                      qRound((option.rect.top() + 8) * ratio),
                                      qRound((option.rect.width() - 16) * ratio),
                                      qRound((option.rect.height() - 16) * ratio));
                    if (plain.copy(inner) != selected.copy(inner) || plain.copy(inner) != combined.copy(inner))
                    {
                        std::cerr << "FAIL message selection recolors its body instead of an outline\n";
                        return false;
                    }
                    QRect const row(qRound(option.rect.left() * ratio), qRound(option.rect.top() * ratio),
                                    qRound(option.rect.width() * ratio), qRound(option.rect.height() * ratio));
                    for (int y = 0; y < plain.height(); ++y)
                    {
                        for (int x = 0; x < plain.width(); ++x)
                        {
                            if (!row.contains(x, y) && plain.pixel(x, y) != combined.pixel(x, y))
                            {
                                std::cerr << "FAIL message outline escapes its row\n";
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }
    std::cout << "PASS Qt selectable message outlines, null-widget states and unchanged NoSelection rendering\n";
    return true;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (!check_message_selection_raster()) { return 1; }
    auto const chat_icon = svg_icon(QStringLiteral("chat"), QColor(QStringLiteral("#315A4B")));
    for (qreal ratio : {1.0, 1.25, 1.5, 2.0})
    {
        auto const pixels = chat_icon.pixmap(QSize(20, 20), ratio);
        if (pixels.width() < qRound(20 * ratio) || pixels.height() < qRound(20 * ratio) ||
            pixels.deviceIndependentSize() != QSizeF(20, 20))
        {
            std::cerr << "FAIL SVG icon retains its pixel density at " << ratio << "x: "
                      << pixels.width() << " pixels at " << pixels.devicePixelRatio() << "x\n";
            return 1;
        }
    }
    auto const initial_icon = avatar_icon(QStringLiteral("张三"), 64);
    for (qreal ratio : {1.0, 1.25, 1.5, 2.0})
    {
        auto const pixels = initial_icon.pixmap(QSize(64, 64), ratio);
        if (pixels.width() < qRound(64 * ratio) || pixels.height() < qRound(64 * ratio) ||
            pixels.deviceIndependentSize() != QSizeF(64, 64) || pixels.toImage().pixelColor(0, 0).alpha() != 0)
        {
            std::cerr << "FAIL circular fallback avatar retains its pixel density at " << ratio << "x\n";
            return 1;
        }
    }
    for (int width : {208, 416})
    {
        QImage photo(width, 208, QImage::Format_RGB32);
        for (int y = 0; y < photo.height(); ++y)
        {
            for (int x = 0; x < photo.width(); ++x) { photo.setPixelColor(x, y, x % 2 ? Qt::white : Qt::black); }
        }
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!photo.save(&buffer, "PNG")) { return 1; }
        avatar_cache profile_photo;
        profile_photo.observe(7, {1, true});
        profile_photo.receive(7, {1, true}, bytes);
        auto const pixels = avatar_icon("photo", 104, profile_photo.image(7)).pixmap(QSize(104, 104), 2.0).toImage();
        if (pixels.size() != QSize(208, 208) || pixels.pixelColor(64, 104) != Qt::black ||
            pixels.pixelColor(65, 104) != Qt::white)
        {
            std::cerr << "FAIL profile photo retains source detail at 200%\n";
            return 1;
        }
    }
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
    image_requests.clear();
    for (qint64 id : {100, 101, 102}) { pictures.observe(70, id); }
    pictures.remove(70);
    pictures.observe(70, 100);
    pictures.observe(70, 103);
    if (image_requests != QList<qint64>{100, 101, 102})
    { std::cerr << "FAIL removed in-flight downloads still count toward the concurrency bound\n"; return 1; }
    pictures.receive(70, 100, {}, "old membership denied");
    if (image_requests != QList<qint64>{100, 101, 102, 100} || !pictures.bytes(100).isEmpty()) { return 1; }
    pictures.receive(70, 100, png, {});
    pictures.receive(70, 101, png, {});
    pictures.receive(70, 102, png, {});
    pictures.receive(70, 103, png, {});
    if (pictures.image(100).isNull() || pictures.image(103).isNull() || !pictures.bytes(101).isEmpty() ||
        !pictures.bytes(102).isEmpty() || image_requests != QList<qint64>{100, 101, 102, 100, 103}) { return 1; }
    pictures.clear();
    std::cout << "PASS image download deduplication, queue bound, immutable cache, paint, fallback, lifecycle and eviction\n";
    image_requests.clear();
    pictures.observe(70, 200);
    pictures.remove(70);
    pictures.observe(70, 200);
    if (image_requests != QList<qint64>{200}) { return 1; }
    pictures.receive(70, 200, {}, "old membership denied");
    if (image_requests != QList<qint64>{200, 200} || !pictures.bytes(200).isEmpty()) { return 1; }
    pictures.receive(70, 200, png, {});
    if (pictures.image(200).isNull()) { return 1; }
    pictures.clear();
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
    message_delegate long_delegate;
    for (int width : {320, 430, 640, 1280})
    {
        for (bool outgoing : {false, true})
        {
            for (int kind : {0, 1, 2, 3})
            {
                for (QPoint origin : {QPoint(0, 0), QPoint(17, 23)})
                {
                    for (qreal ratio : {1.0, 1.25, 1.5, 2.0})
                    {
                        message_model long_messages;
                        long_messages.set_self_user(1);
                        long_messages.reset(1);
                        message_data long_message;
                        long_message.id = 1;
                        long_message.conversation = 1;
                        long_message.from = outgoing ? 1 : 2;
                        auto const username = QString(64, QLatin1Char('W'));
                        if (kind == 1 || kind == 3)
                        {
                            long_message.text = QLatin1Char('@') + username;
                            long_message.mentions = {{2, username}};
                        }
                        else if (kind == 2)
                        {
                            long_message.text = QStringLiteral("https://example.com/") + QString(150, QLatin1Char('W')) +
                                                QStringLiteral("/终点 é 👩‍💻\n最后一行\n");
                        }
                        else
                        {
                            long_message.text = QString(150, QLatin1Char('W'));
                        }
                        if (kind == 3)
                        {
                            long_message.reply = {9, QStringLiteral("引用者"), QStringLiteral("@") + username, 0, false};
                        }
                        long_messages.add_message(long_message);
                        auto const index = long_messages.index(0, 0);
                        QStyleOptionViewItem long_option;
                        long_option.font = QApplication::font();
                        long_option.font.setPixelSize(14);
                        long_option.rect = QRect(origin, QSize(width, 2000));
                        auto const reserved = chat_theme::message_side_margin * 2 + (outgoing ? 0 : chat_theme::message_avatar_skip);
                        auto const bubble_max = std::min(chat_theme::message_max_width, width - reserved);
                        auto const inner_max = bubble_max - chat_theme::message_padding_horizontal * 2;
                        auto text = index.data(message_model::text_role).toString();
                        auto const reply = index.data(message_model::reply_text_role).toString();
                        auto const body_start = reply.isEmpty() ? 0 : reply.size() + 2;
                        if (!reply.isEmpty())
                        {
                            text = reply + QStringLiteral("\n\n") + text;
                        }
                        text.replace(QLatin1Char('\n'), QChar::LineSeparator);
                        QTextLayout reference_layout(text, long_option.font);
                        QTextOption text_option;
                        text_option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
                        reference_layout.setTextOption(text_option);
                        if (!long_message.mentions.isEmpty())
                        {
                            QTextCharFormat format;
                            format.setForeground(QColor(QStringLiteral("#277399")));
                            reference_layout.setFormats({{static_cast<int>(body_start), static_cast<int>(long_message.text.size()), format}});
                        }
                        reference_layout.beginLayout();
                        int text_height = 0;
                        int text_width = 1;
                        while (true)
                        {
                            auto line = reference_layout.createLine();
                            if (!line.isValid())
                            {
                                break;
                            }
                            line.setLineWidth(inner_max);
                            line.setPosition(QPointF(0, text_height));
                            text_height += qCeil(line.height());
                            text_width = std::max(text_width, qCeil(line.naturalTextWidth()));
                        }
                        reference_layout.endLayout();
                        auto const hint = long_delegate.sizeHint(long_option, index);
                        if (reference_layout.lineCount() < 2 ||
                            hint.height() < text_height + chat_theme::message_padding_vertical * 2 + chat_theme::message_margin_top)
                        {
                            std::cerr << "FAIL long message allocates every wrapped line: " << width << '/' << outgoing << '/' << kind << '\n';
                            return 1;
                        }
                        QImage actual(QSize(qCeil((width + origin.x() * 2) * ratio), qCeil((hint.height() + origin.y() * 2 + 20) * ratio)),
                                      QImage::Format_ARGB32_Premultiplied);
                        actual.setDevicePixelRatio(ratio);
                        actual.fill(QColor(QStringLiteral("#F7F5EF")));
                        QPainter actual_painter(&actual);
                        long_delegate.paint(&actual_painter, long_option, index);
                        actual_painter.end();
                        QImage expected_long(actual.size(), actual.format());
                        expected_long.setDevicePixelRatio(ratio);
                        auto const bubble_color = QColor(outgoing ? QStringLiteral("#D6EAD9") : QStringLiteral("#FFFFFF"));
                        expected_long.fill(bubble_color);
                        QPainter expected_painter(&expected_long);
                        expected_painter.setRenderHint(QPainter::Antialiasing);
                        expected_painter.setPen(QColor(QStringLiteral("#26342E")));
                        auto const left =
                            outgoing ? width - chat_theme::message_side_margin - text_width - chat_theme::message_padding_horizontal
                                     : chat_theme::message_side_margin + chat_theme::message_avatar_skip + chat_theme::message_padding_horizontal;
                        reference_layout.draw(&expected_painter,
                                              origin + QPoint(left, chat_theme::message_margin_top + chat_theme::message_padding_vertical));
                        expected_painter.end();
                        for (auto const color : {QColor(QStringLiteral("#26342E")), QColor(QStringLiteral("#277399"))})
                        {
                            for (int y = 0; y < actual.height(); ++y)
                            {
                                for (int x = 0; x < actual.width(); ++x)
                                {
                                    auto const actual_pixel = actual.pixelColor(x, y);
                                    auto const expected_pixel = expected_long.pixelColor(x, y);
                                    if ((actual_pixel == color) != (expected_pixel == color)
                                        || (color == QColor(QStringLiteral("#26342E"))
                                            && expected_pixel != bubble_color && actual_pixel != expected_pixel))
                                    {
                                        std::cerr << "FAIL long message retains complete body inside its bubble: " << width << '/' << outgoing << '/'
                                                  << kind << " at " << x << ',' << y << '\n';
                                        return 1;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    std::cout << "PASS long plain text, URL, mention and reply wrap without clipping\n";
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
