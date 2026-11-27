#include "attachment_dialog.hpp"
#include "theme.hpp"
#include "download_tasks.hpp"
#include <QToolButton>
#include <utility>

#include <QBuffer>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QImageReader>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>

#include <QStandardPaths>
#include <QVBoxLayout>

attachment_dialog::attachment_dialog(
    download_tasks& downloads, qint64 conversation, qint64 message, QString filename, bool preview, QWidget* parent, QPixmap image, qint64 item)
    : QDialog(parent),
      conversation_(conversation),
      message_(message),
      item_(item),
      filename_(std::move(filename)),
      preview_(preview),
      preview_image_(std::move(image))
{
    setObjectName(QStringLiteral("attachmentDialog"));
    setWindowTitle(preview ? QStringLiteral("图片预览") : QStringLiteral("附件详情"));
    resize(preview ? QSize(700, 600) : QSize(500, 180));
    auto* layout = new QVBoxLayout(this);
    frame_dialog(*this);
    layout->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
    layout->setSpacing(chat_theme::dialog_spacing);
    status_ = new QLabel(preview ? QStringLiteral("正在加载预览…") : filename_, this);
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
    // The cached bubble thumbnail is only a placeholder until the original arrives.
    image_->setVisible(preview_ && !preview_image_.isNull());
    layout->addWidget(image_, 1);
    save_button_ = new QPushButton(QStringLiteral("保存文件"), this);
    save_button_->setObjectName(QStringLiteral("saveAttachmentButton"));
    auto* buttons = new QDialogButtonBox(this);
    buttons->addButton(save_button_, QDialogButtonBox::ActionRole);
    layout->addWidget(downloads.source_button(this, [=] { return download_source{conversation, message, item, {}}; }));
    layout->addWidget(buttons);
    connect(save_button_,
            &QPushButton::clicked,
            this,
            [this]
            {
                auto* chooser = new QFileDialog(this, QStringLiteral("保存文件"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
                chooser->setAttribute(Qt::WA_DeleteOnClose);
                chooser->setAcceptMode(QFileDialog::AcceptSave);
                chooser->selectFile(filename_);
                connect(chooser,
                        &QFileDialog::fileSelected,
                        this,
                        [this](QString const& path)
                        {
                            save_button_->setEnabled(false);
                            status_->setText(QStringLiteral("正在下载…"));
                            saving_to_ = path;
                            emit save_requested(conversation_, message_, path);
                        });
                connect(this, &QDialog::finished, chooser, &QDialog::reject);
                chooser->open();
            });
}

void attachment_dialog::download_finished(qint64 conversation, qint64 message, QString path, QString error, qint64 item)
{
    if (conversation != conversation_ || message != message_ || item != item_ || saving_to_ != path)
    {
        return;
    }
    saving_to_.clear();
    save_button_->setEnabled(true);
    status_->setText(error.isEmpty() ? QStringLiteral("已保存到 %1").arg(path) : error);
}
bool attachment_dialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == image_ && event->type() == QEvent::Resize && !preview_image_.isNull())
    {
        image_->setPixmap(preview_image_.scaled(preview_image_.size().boundedTo(image_->size()), Qt::KeepAspectRatio, Qt::SmoothTransformation));
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

    status_->setText(QStringLiteral("%1 · %2 KiB").arg(filename_, QString::number(data.size() / 1024.0, 'f', 1)));
    if (!preview_)
    {
        return;
    }
    QBuffer buffer(&data);
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
    auto const image = reader.read();
    if (image.isNull())
    {
        status_->setText(QStringLiteral("图片无法预览；可以保存原文件。"));
        return;
    }
    preview_image_ = QPixmap::fromImage(image);
    image_->setPixmap(preview_image_.scaled(preview_image_.size().boundedTo(image_->size()), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    image_->show();
}
