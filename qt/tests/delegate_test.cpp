#include <iostream>

#include <QApplication>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QStyleOptionViewItem>

#include "message_delegate.hpp"
#include "message_model.hpp"
#include "theme.hpp"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    message_model messages;
    messages.set_self_user(1);
    messages.reset(1, true);
    message_data message;
    message.id = 1;
    message.conversation = 1;
    message.from = 2;
    message.username = QStringLiteral("成员");
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
