#include "profile_controls.hpp"
#include "icons.hpp"
#include "theme_manager.hpp"

#include <QPushButton>
#include <QToolButton>

namespace
{
void profile_icon(QAbstractButton& button, QStringView icon, bool danger)
{
    if (icon.isEmpty())
    {
        return;
    }
    auto refresh = [&button, name = icon.toString(), danger]
    { button.setIcon(svg_icon(name, themed(danger ? "danger" : "accent"), button.iconSize())); };
    refresh();
    QObject::connect(&theme_manager::instance(), &theme_manager::changed, &button, refresh);
}
}    // namespace

QToolButton* profile_action(QWidget* parent, QString const& text, QStringView icon, bool danger)
{
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setProperty("profileAction", true);
    button->setProperty("danger", danger);
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setIconSize({24, 24});
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setFixedHeight(58);
    profile_icon(*button, icon, danger);
    return button;
}

QPushButton* profile_entry(QWidget* parent, QString const& text, QStringView icon, bool danger)
{
    auto* button = new QPushButton(text, parent);
    button->setAutoDefault(false);
    button->setProperty("profileEntry", true);
    button->setProperty("danger", danger);
    button->setIconSize({23, 23});
    button->setMinimumHeight(42);
    profile_icon(*button, icon, danger);
    return button;
}

void display_notification_action(QToolButton& button, bool muted)
{
    auto const action = muted ? QStringLiteral("取消静音") : QStringLiteral("静音");
    button.setText(action);
    button.setToolTip(action);
    button.setAccessibleName(action);
    button.setIcon(svg_icon(muted ? u"bell-off" : u"bell", themed("accent"), button.iconSize()));
}
