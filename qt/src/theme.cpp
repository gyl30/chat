#include "theme.hpp"
#include "theme_manager.hpp"
#include <QAbstractButton>
#include <QAccessibleObject>
#include <QAction>
#include <QApplication>
#include <QLayout>
#include <QGraphicsEffect>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QWindow>
#include "icons.hpp"
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <utility>
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
#include <QScopedPointer>
#include <xcb/xcb.h>
#endif

class accessible_feedback_notification final : public QAccessibleObject
{
   public:
    explicit accessible_feedback_notification(QObject* notification) : QAccessibleObject(notification) {}

    QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(qApp); }

    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(QAccessibleInterface const*) const override { return -1; }
    QAccessible::Role role() const override { return QAccessible::Notification; }

    QAccessible::State state() const override
    {
        QAccessible::State result;
        result.invisible = !static_cast<QLabel*>(object()->parent())->isVisible();
        result.readOnly = true;
        return result;
    }

    QString text(QAccessible::Text type) const override
    {
        if (type == QAccessible::Name)
        {
            return static_cast<QLabel*>(object()->parent())->text();
        }
        return {};
    }
};

feedback_label::feedback_label(QWidget* parent) : QLabel(parent), notification_(this)
{
    notification_.setObjectName(QStringLiteral("feedbackNotification"));
    QAccessible::registerAccessibleInterface(new accessible_feedback_notification(&notification_));
}

void feedback_label::show_error(QString message)
{
    setText(std::move(message));
    if (isVisible() && !text().isEmpty())
    {
        QAccessibleEvent event(&notification_, QAccessible::ObjectShow);
        QAccessible::updateAccessibility(&event);
    }
}

namespace
{
class draggable_title final : public QLabel
{
   public:
    using QLabel::QLabel;

   protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && window()->windowHandle()->startSystemMove())
        {
            event->accept();
            return;
        }
        QLabel::mousePressEvent(event);
    }
};
}    // namespace

QLabel* dialog_title(QDialog& dialog, QString title)
{
    auto* label = new draggable_title(std::move(title), &dialog);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName(QStringLiteral("dialogTitle"));
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

namespace
{
bool has_x11_compositor()
{
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
    auto* x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
    auto* connection = x11->connection();
    auto const display = qgetenv("DISPLAY");
    auto const suffix = display.indexOf('.', display.lastIndexOf(':') + 1);
    auto const screen = suffix < 0 ? 0 : display.mid(suffix + 1).toInt();
    auto const selection = QByteArrayLiteral("_NET_WM_CM_S") + QByteArray::number(screen);
    auto const cookie = xcb_intern_atom(connection, 1, selection.size(), selection.constData());
    QScopedPointer<xcb_intern_atom_reply_t, QScopedPointerPodDeleter> const atom(xcb_intern_atom_reply(connection, cookie, nullptr));
    if (!atom || atom->atom == XCB_ATOM_NONE)
    {
        return false;
    }
    QScopedPointer<xcb_get_selection_owner_reply_t, QScopedPointerPodDeleter> const owner(
        xcb_get_selection_owner_reply(connection, xcb_get_selection_owner(connection, atom->atom), nullptr));
    return owner && owner->owner != XCB_WINDOW_NONE;
#else
    return false;
#endif
}

class rounded_window_effect final : public QGraphicsEffect
{
   public:
    explicit rounded_window_effect(int radius) : radius_(radius) {}

   private:
    void draw(QPainter* painter) override
    {
        QPoint offset;
        auto pixmap = sourcePixmap(Qt::LogicalCoordinates, &offset, NoPad);
        if (pixmap.isNull())
        {
            return;
        }
        {
            QPainter surface(&pixmap);
            auto const rect = QRectF(QPointF{}, pixmap.deviceIndependentSize());
            // Some styles leave a translucent dialog's backing surface unfilled.
            surface.setCompositionMode(QPainter::CompositionMode_DestinationOver);
            surface.fillRect(rect, themed("panel"));
            QPainterPath corners;
            corners.addRect(rect);
            corners.addRoundedRect(rect, radius_, radius_);
            surface.setRenderHint(QPainter::Antialiasing);
            surface.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            surface.fillPath(corners, Qt::black);
        }
        painter->save();
        painter->setCompositionMode(QPainter::CompositionMode_Source);
        painter->drawPixmap(offset, pixmap);
        painter->restore();
    }

    int radius_;
};

class window_shape final : public QObject
{
   public:
    window_shape(QWidget& window, int radius) : QObject(&window), window_(window), radius_(radius)
    {
        if (has_x11_compositor())
        {
            window_.setAttribute(Qt::WA_TranslucentBackground);
            effect_ = new rounded_window_effect(radius_);
            window_.setGraphicsEffect(effect_);
        }
        window_.installEventFilter(this);
    }

   private:
    bool eventFilter(QObject*, QEvent* event) override
    {
        switch (event->type())
        {
            case QEvent::Show:
            case QEvent::Resize:
            case QEvent::WindowStateChange:
            {
                auto const rounded = !window_.isMaximized() && !window_.isFullScreen();
                if (effect_)
                {
                    effect_->setEnabled(rounded);
                }
                else if (rounded)
                {
                    QPainterPath outline;
                    outline.addRoundedRect(QRectF(window_.rect()), radius_, radius_);
                    window_.setMask(QRegion(outline.toFillPolygon().toPolygon()));
                }
                else
                {
                    window_.clearMask();
                }
                break;
            }
            default:
                break;
        }
        return false;
    }

    QWidget& window_;
    int radius_;
    rounded_window_effect* effect_ = nullptr;
};

class popup_shapes final : public QObject
{
   public:
    using QObject::QObject;

   private:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() == QEvent::Polish)
        {
            if (auto* popup = qobject_cast<QWidget*>(object);
                popup && (popup->windowType() == Qt::Popup || popup->windowType() == Qt::ToolTip))
            {
                auto const compact = popup->windowType() == Qt::ToolTip || qobject_cast<QMenu*>(popup);
                round_window(*popup, compact ? chat_theme::menu_radius : chat_theme::popup_radius);
            }
        }
        return false;
    }
};

bool supports_window_shape() { return QGuiApplication::platformName() == QStringLiteral("xcb"); }
}    // namespace

void round_window(QWidget& window, int radius)
{
    if (supports_window_shape())
    {
        new window_shape(window, radius);
    }
}

void round_popups()
{
    if (supports_window_shape())
    {
        qApp->installEventFilter(new popup_shapes(qApp));
    }
}

void frame_dialog(QDialog& dialog, bool resizable)
{
    dialog.setWindowFlag(Qt::FramelessWindowHint);
    dialog.setProperty("chatPopup", true);
    round_window(dialog, chat_theme::dialog_radius);
    auto* bar = new QWidget(&dialog);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(16, 6, 8, 4);
    auto* title = dialog_title(dialog, dialog.windowTitle());
    row->addWidget(title, 1);
    QObject::connect(&dialog, &QWidget::windowTitleChanged, title, &QLabel::setText);
    auto* close = new QToolButton(bar);
    close->setObjectName(QStringLiteral("popupCloseButton"));
    close->setAccessibleName(QStringLiteral("关闭"));
    close->setToolTip(close->accessibleName());
    close->setFixedSize(32, 32);
    close->setAutoRaise(true);
    close->setFocusPolicy(Qt::ClickFocus);
    auto recolor = [close] { close->setIcon(svg_icon(u"close", themed("text_secondary"), {16, 16})); };
    recolor();
    QObject::connect(&theme_manager::instance(), &theme_manager::changed, close, recolor);
    QObject::connect(close, &QToolButton::clicked, &dialog, &QWidget::close);
    row->addWidget(close);
    dialog.layout()->setMenuBar(bar);
    dialog.setSizeGripEnabled(resizable);
}

void style_search_field(QLineEdit& field)
{
    field.setProperty("searchField", true);
    field.setClearButtonEnabled(true);
    auto* action = field.addAction(QIcon{}, QLineEdit::LeadingPosition);
    auto recolor = [action] { action->setIcon(svg_icon(u"search", themed("text_muted"), {16, 16})); };
    recolor();
    QObject::connect(&theme_manager::instance(), &theme_manager::changed, action, recolor);
}

QString prompt_text(QWidget* parent, QString const& title, QString const& label, QString const& initial, bool& accepted, bool multiline)
{
    QInputDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setOption(QInputDialog::UsePlainTextEditForTextInput, multiline);
    dialog.setLabelText(label);
    dialog.setTextValue(initial);
    dialog.setOkButtonText(QStringLiteral("确定"));
    dialog.setCancelButtonText(QStringLiteral("取消"));
    frame_dialog(dialog, multiline);
    accepted = dialog.exec() == QDialog::Accepted;
    return dialog.textValue();
}

bool confirm_action(QWidget* parent, QString const& title, QString const& text, QString const& action)
{
    QMessageBox dialog(QMessageBox::NoIcon, title, text, QMessageBox::Yes | QMessageBox::No, parent);
    dialog.setObjectName(QStringLiteral("confirmationDialog"));
    dialog.setTextFormat(Qt::PlainText);
    dialog.button(QMessageBox::Yes)->setText(action);
    dialog.button(QMessageBox::Yes)->setObjectName(QStringLiteral("confirmationActionButton"));
    dialog.button(QMessageBox::No)->setText(QStringLiteral("取消"));
    dialog.button(QMessageBox::No)->setObjectName(QStringLiteral("confirmationCancelButton"));
    for (auto* button : dialog.buttons())
    {
        button->setIcon({});
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
    dialog.setDefaultButton(QMessageBox::No);
    dialog.setEscapeButton(QMessageBox::No);
    dialog.layout()->setContentsMargins(
        chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding, chat_theme::dialog_padding);
    dialog.layout()->setSpacing(chat_theme::dialog_spacing);
    frame_dialog(dialog, false);
    return dialog.exec() == QMessageBox::Yes;
}

QString chat_style_sheet() { return theme_manager::instance().style_sheet(); }
