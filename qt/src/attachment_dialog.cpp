#include "attachment_dialog.hpp"
#include "theme.hpp"
#include "download_tasks.hpp"
#include <QToolButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QClipboard>
#include <QGuiApplication>
#include <algorithm>
#include <cmath>
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

namespace
{
class preview_canvas final : public QLabel
{
   public:
    using QLabel::QLabel;

   protected:
    void paintEvent(QPaintEvent*) override
    {
        auto const image = pixmap();
        if (!image.isNull())
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawPixmap(rect(), image);
        }
    }
};
}    // namespace

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
    image_ = new preview_canvas(this);
    image_->setObjectName(QStringLiteral("attachmentImage"));
    image_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    image_->installEventFilter(this);

    save_button_ = new QPushButton(QStringLiteral("保存文件"), this);
    save_button_->setObjectName(QStringLiteral("saveAttachmentButton"));
    save_button_->setProperty("primary", true);
    auto* buttons = new QDialogButtonBox(this);
    if (preview_)
    {
        scroll_ = new QScrollArea(this);
        scroll_->setObjectName(QStringLiteral("attachmentViewport"));
        scroll_->setFrameShape(QFrame::NoFrame);
        scroll_->setAlignment(Qt::AlignCenter);
        scroll_->setWidget(image_);
        scroll_->viewport()->installEventFilter(this);
        scroll_->setWidgetResizable(false);
        layout->addWidget(scroll_, 1);

        fit_button_ = buttons->addButton(QStringLiteral("实际尺寸"), QDialogButtonBox::ActionRole);
        fit_button_->setObjectName(QStringLiteral("attachmentFitButton"));
        fit_button_->setEnabled(false);
        copy_button_ = buttons->addButton(QStringLiteral("复制图片"), QDialogButtonBox::ActionRole);
        copy_button_->setEnabled(false);
        connect(fit_button_,
                &QPushButton::clicked,
                this,
                [this]
                {
                    zoom_ = zoom_ == 0 ? 1 : 0;
                    update_image();
                });
        connect(copy_button_, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setPixmap(preview_image_); });
        update_image();
    }
    else
    {
        image_->hide();
    }
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
void attachment_dialog::update_image()
{
    if (preview_image_.isNull())
    {
        image_->hide();
        return;
    }
    auto const natural = preview_image_.size();
    auto const viewport = scroll_->maximumViewportSize();
    auto size = natural;
    if (zoom_ > 0)
    {
        size = natural * zoom_;
    }
    else if (size.width() > viewport.width() || size.height() > viewport.height())
    {
        size.scale(viewport.expandedTo(QSize(1, 1)), Qt::KeepAspectRatio);
    }
    image_->setPixmap(preview_image_);
    image_->resize(size.expandedTo(QSize(1, 1)));
    image_->show();
    const bool overflow = size.width() > viewport.width() || size.height() > viewport.height();
    image_->setCursor(overflow ? Qt::OpenHandCursor : Qt::ArrowCursor);
    fit_button_->setText(zoom_ == 0 ? QStringLiteral("实际尺寸") : QStringLiteral("适应窗口"));
}

bool attachment_dialog::eventFilter(QObject* watched, QEvent* event)
{
    if (!scroll_ || preview_image_.isNull())
    {
        return QDialog::eventFilter(watched, event);
    }
    auto* viewport = scroll_->viewport();
    if (watched == viewport && event->type() == QEvent::Resize && zoom_ == 0)
    {
        update_image();
    }
    if ((watched == image_ || watched == viewport) && event->type() == QEvent::Wheel)
    {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (!wheel->angleDelta().y())
        {
            return false;
        }
        const auto global = wheel->globalPosition().toPoint();
        const auto cursor = viewport->mapFromGlobal(global);
        const auto image_position = image_->mapFromGlobal(global);
        const auto previous = image_->size();
        const auto ratio = double(previous.width()) / preview_image_.width();
        zoom_ = std::clamp(ratio * std::pow(1.2, wheel->angleDelta().y() / 120.0), 0.05, 4.0);
        update_image();
        scroll_->horizontalScrollBar()->setValue(image_position.x() * double(image_->width()) / previous.width() - cursor.x());
        scroll_->verticalScrollBar()->setValue(image_position.y() * double(image_->height()) / previous.height() - cursor.y());
        return true;
    }
    if (watched == image_ &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease))
    {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton &&
            (scroll_->horizontalScrollBar()->maximum() || scroll_->verticalScrollBar()->maximum()))
        {
            drag_ = mouse->globalPosition().toPoint();
            image_->setCursor(Qt::ClosedHandCursor);
            return true;
        }
        if (drag_ && event->type() == QEvent::MouseMove)
        {
            const auto point = mouse->globalPosition().toPoint();
            const auto delta = *drag_ - point;
            *drag_ = point;
            for (auto* bar : {scroll_->horizontalScrollBar(), scroll_->verticalScrollBar()})
            {
                bar->setValue(bar->value() + (bar->orientation() == Qt::Horizontal ? delta.x() : delta.y()));
            }
            return true;
        }
        if (drag_ && event->type() == QEvent::MouseButtonRelease)
        {
            drag_.reset();
            image_->setCursor(Qt::OpenHandCursor);
            return true;
        }
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
    fit_button_->setEnabled(true);
    copy_button_->setEnabled(true);
    update_image();
}
