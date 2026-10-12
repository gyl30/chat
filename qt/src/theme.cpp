#include "theme.hpp"
#include "theme_manager.hpp"
#include <QAbstractButton>
#include <QAccessibleObject>
#include <QApplication>
#include <QLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <utility>

class accessible_feedback_notification final : public QAccessibleObject
{
   public:
    explicit accessible_feedback_notification(QObject* notification)
        : QAccessibleObject(notification)
    {
    }

    QAccessibleInterface* parent() const override
    {
        return QAccessible::queryAccessibleInterface(qApp);
    }

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
    dialog.layout()->setContentsMargins(chat_theme::dialog_padding, chat_theme::dialog_padding,
                                       chat_theme::dialog_padding, chat_theme::dialog_padding);
    dialog.layout()->setSpacing(chat_theme::dialog_spacing);
    return dialog.exec() == QMessageBox::Yes;
}

QString chat_style_sheet() { return theme_manager::instance().style_sheet(); }
