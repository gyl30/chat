#include "attachment_dialog.hpp"
#include "theme.hpp"
#include <utility>

#include <QBuffer>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QImageReader>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVBoxLayout>

attachment_dialog::attachment_dialog(qint64 conversation, qint64 message, QString filename, bool preview, QWidget* parent,
                                     QPixmap image)
    : QDialog(parent), conversation_(conversation), message_(message), filename_(std::move(filename)), preview_(preview),
      preview_image_(std::move(image))
{
    setObjectName(QStringLiteral("attachmentDialog"));
    setWindowTitle(filename_);
    resize(preview ? QSize(700, 600) : QSize(500, 180));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                              chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    status_ = new QLabel(QStringLiteral("正在下载 %1…").arg(filename_), this);
    status_->setObjectName(QStringLiteral("attachmentStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    image_ = new QLabel(this);
    image_->setObjectName(QStringLiteral("attachmentImage"));
    image_->setAlignment(Qt::AlignCenter);
    image_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    image_->installEventFilter(this);
    image_->setPixmap(preview_image_);
    image_->hide();
    layout->addWidget(image_, 1);
    save_button_ = new QPushButton(QStringLiteral("保存文件"), this);
    save_button_->setObjectName(QStringLiteral("saveAttachmentButton"));
    save_button_->setEnabled(false);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(save_button_, QDialogButtonBox::ActionRole);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    buttons->button(QDialogButtonBox::Close)->setIcon({});
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(save_button_, &QPushButton::clicked, this, [this] {
        auto const path = QFileDialog::getSaveFileName(this, QStringLiteral("保存文件"),
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + filename_);
        if (path.isEmpty())
        {
            return;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data_) != data_.size() || !file.commit())
        {
            status_->setText(QStringLiteral("保存失败：%1").arg(file.errorString()));
            return;
        }
        status_->setText(QStringLiteral("已保存到 %1").arg(path));
    });
}

bool attachment_dialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == image_ && event->type() == QEvent::Resize && !preview_image_.isNull())
    {
        image_->setPixmap(preview_image_.scaled(preview_image_.size().boundedTo(image_->size()),
                                               Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    return QDialog::eventFilter(watched, event);
}

void attachment_dialog::set_data(qint64 conversation, qint64 message, QByteArray data, QString const& error_message)
{
    if (conversation != conversation_ || message != message_)
    {
        return;
    }
    if (!error_message.isEmpty())
    {
        status_->setText(error_message);
        return;
    }
    data_ = std::move(data);
    save_button_->setEnabled(true);
    status_->setText(QStringLiteral("%1 · %2 KiB").arg(filename_, QString::number(data_.size() / 1024.0, 'f', 1)));
    if (!preview_)
    {
        return;
    }
    if (!preview_image_.isNull()) { image_->show(); return; }
    QBuffer buffer(&data_);
    buffer.open(QIODevice::ReadOnly);
    QImageReader::setAllocationLimit(64);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    auto const size = reader.size();
    if (size.width() <= 0 || size.height() <= 0 || static_cast<qint64>(size.width()) * size.height() > 16 * 1024 * 1024)
    {
        status_->setText(QStringLiteral("图片无法预览，或尺寸过大；可以保存原文件。"));
        return;
    }
    reader.setScaledSize(size.scaled(QSize(640, 480), Qt::KeepAspectRatio));
    auto const image = reader.read();
    if (image.isNull())
    {
        status_->setText(QStringLiteral("图片无法预览；可以保存原文件。"));
        return;
    }
    preview_image_ = QPixmap::fromImage(image);
    image_->setPixmap(preview_image_.scaled(preview_image_.size().boundedTo(image_->size()),
                                           Qt::KeepAspectRatio, Qt::SmoothTransformation));
    image_->show();
}
