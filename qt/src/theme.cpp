#include "theme.hpp"
#include "theme_manager.hpp"
#include <QAbstractButton>
#include <QAccessibleObject>
#include <QApplication>
#include <QLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMouseEvent>
#include <QToolButton>
#include <QWindow>
#include "icons.hpp"
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <utility>

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

void frame_dialog(QDialog& dialog, bool resizable)
{
    dialog.setWindowFlag(Qt::FramelessWindowHint);
    dialog.setProperty("chatPopup", true);
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
