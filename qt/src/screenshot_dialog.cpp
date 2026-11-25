#include "theme.hpp"
#include "screenshot_dialog.hpp"

#include <algorithm>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRubberBand>
#include <QScreen>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#if defined(Q_OS_LINUX)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

class screenshot_preview final : public QLabel
{
   public:
    explicit screenshot_preview(QWidget* parent) : QLabel(parent), band_(QRubberBand::Rectangle, this)
    {
        setObjectName(QStringLiteral("screenshotPreview"));
        setCursor(Qt::CrossCursor);
    }

    void display(QImage image)
    {
        source_ = std::move(image);
        source_.setDevicePixelRatio(1);
        auto available = screen()->availableGeometry().size() - QSize(60, 150);
        auto size = source_.size().scaled(available.boundedTo(QSize(900, 580)), Qt::KeepAspectRatio).boundedTo(source_.size());
        setFixedSize(size);
        setPixmap(QPixmap::fromImage(source_.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        band_.hide();
        show();
    }

    QImage selected() const
    {
        if (band_.isHidden())
        {
            return source_;
        }
        QRectF area = band_.geometry();
        const auto sx = qreal(source_.width()) / width(), sy = qreal(source_.height()) / height();
        return source_.copy(QRectF(area.x() * sx, area.y() * sy, area.width() * sx, area.height() * sy).toAlignedRect());
    }

   protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            return;
        }
        start_ = event->position().toPoint();
        band_.hide();
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!event->buttons().testFlag(Qt::LeftButton))
        {
            return;
        }
        auto selection = QRect(start_, event->position().toPoint()).normalized().intersected(rect());
        band_.setGeometry(selection);
        band_.setVisible(selection.width() > 1 && selection.height() > 1);
    }

   private:
    QImage source_;
    QPoint start_;
    QRubberBand band_;
};

screenshot_dialog::screenshot_dialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("截图预览"));
    setObjectName(QStringLiteral("screenshotDialog"));
    auto* layout = new QVBoxLayout(this);
    frame_dialog(*this);
    status_ = new QLabel(this);
    layout->addWidget(status_);
    preview_ = new screenshot_preview(this);
    layout->addWidget(preview_, 0, Qt::AlignCenter);
    preview_->hide();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    send_ = buttons->button(QDialogButtonBox::Ok);
    send_->setText(QStringLiteral("发送截图"));
    send_->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

#if defined(Q_OS_LINUX)
namespace
{
const QString portal = QStringLiteral("org.freedesktop.portal.Desktop");
const QString request_interface = QStringLiteral("org.freedesktop.portal.Request");
}    // namespace
#endif

screenshot_dialog::~screenshot_dialog()
{
#if defined(Q_OS_LINUX)
    if (!request_path_.isEmpty())
    {
        QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(portal, request_path_, request_interface, "Close"));
    }
#endif
}

void screenshot_dialog::request_screenshot()
{
#if defined(Q_OS_LINUX)
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) ||
        qEnvironmentVariable("XDG_SESSION_TYPE") == QStringLiteral("wayland"))
    {
        auto bus = QDBusConnection::sessionBus();
        auto token = QStringLiteral("chat") + QUuid::createUuid().toString(QUuid::Id128);
        auto sender = bus.baseService().mid(1).replace('.', '_');
        request_path_ = "/org/freedesktop/portal/desktop/request/" + sender + '/' + token;
        if (!bus.connect(portal, request_path_, request_interface, "Response", this, SLOT(portal_response(uint, QVariantMap))))
        {
            show_image({});
            return;
        }
        auto call = QDBusMessage::createMethodCall(portal, "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Screenshot", "Screenshot");
        call << QString{} << QVariantMap{{"handle_token", token}, {"interactive", false}};
        auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
        connect(watcher,
                &QDBusPendingCallWatcher::finished,
                this,
                [this, watcher]
                {
                    QDBusPendingReply<QDBusObjectPath> reply = *watcher;
                    watcher->deleteLater();
                    if (reply.isError())
                    {
                        request_path_.clear();
                        show_image({});
                    }
                });
        return;
    }
#endif
    auto screens = QGuiApplication::screens();
    QRect desktop;
    qreal pixel_ratio = 1;
    for (auto* screen : screens)
    {
        desktop |= screen->geometry();
        pixel_ratio = std::max(pixel_ratio, screen->devicePixelRatio());
    }
    if (desktop.isEmpty())
    {
        show_image({});
        return;
    }
    QImage canvas(desktop.size() * pixel_ratio, QImage::Format_RGB32);
    canvas.setDevicePixelRatio(pixel_ratio);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    for (auto* screen : screens)
    {
        auto shot = screen->grabWindow(0);
        if (shot.isNull())
        {
            painter.end();
            show_image({});
            return;
        }
        painter.drawPixmap(screen->geometry().translated(-desktop.topLeft()), shot, shot.rect());
    }
    painter.end();
    show_image(std::move(canvas));
}

void screenshot_dialog::portal_response(uint code, QVariantMap values)
{
    request_path_.clear();
    if (code == 1)
    {
        reject();
        return;
    }
    auto uri = QUrl(values.value("uri").toString());
    show_image(code == 0 && uri.isLocalFile() ? QImage(uri.toLocalFile()) : QImage{});
}

void screenshot_dialog::show_image(QImage image)
{
    if (image.isNull())
    {
        status_->setText(QStringLiteral("当前桌面未能提供截图。可使用系统截图后，在聊天输入框中粘贴图片。"));
        open();
        return;
    }
    preview_->display(std::move(image));
    status_->setText(QStringLiteral("拖动选取区域，单击恢复整幅画面。"));
    send_->setEnabled(true);
    adjustSize();
    open();
}

QImage screenshot_dialog::selected_image() const { return preview_->selected(); }
