#ifndef CHAT_QT_SRC_CLIPBOARD_IMAGE_HPP
#define CHAT_QT_SRC_CLIPBOARD_IMAGE_HPP

#include <QString>
#include <expected>

class QImage;

std::expected<QString, QString> save_clipboard_image(QImage const& image, qint64 client_id);
void remove_clipboard_image(QString const& path, qint64 client_id);

#endif
