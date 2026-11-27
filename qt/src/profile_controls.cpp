#include "profile_controls.hpp"
#include "icons.hpp"
#include "theme_manager.hpp"

#include <QPushButton>
#include <QPainter>
#include <QStyleOptionButton>
#include <QToolButton>

namespace
{
class profile_row final : public QPushButton
{
   public:
    profile_row(QString const& text, QWidget* parent, bool danger, bool navigation)
        : QPushButton(text, parent), danger_(danger), navigation_(navigation)
    {
    }

   protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        QPainter painter(this);
        style()->drawControl(QStyle::CE_PushButtonBevel, &option, &painter, this);
        constexpr int inset = 14;
        constexpr int text_column = 60;
        auto const size = iconSize();
        icon().paint(&painter,
                     {inset, (height() - size.height()) / 2, size.width(), size.height()},
                     Qt::AlignCenter,
                     isEnabled() ? QIcon::Normal : QIcon::Disabled);
        int const right = navigation_ ? 40 : inset;
        auto const available = width() - text_column - right;
        painter.setPen(themed(!isEnabled() ? "text_muted" : danger_ ? "danger" : "text"));
        painter.drawText(QRect{text_column, 0, available, height()},
                         Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(text(), Qt::ElideRight, available));
        if (navigation_)
        {
            auto const arrow = svg_icon(u"chevron-right", themed("text_muted"), {16, 16});
            arrow.paint(&painter, {width() - inset - 16, (height() - 16) / 2, 16, 16});
        }
    }

   private:
    bool danger_;
    bool navigation_;
};

void profile_icon(QAbstractButton& button, QStringView icon, bool danger)
{
    if (icon.isEmpty())
    {
        return;
    }
    auto refresh = [&button, name = icon.toString(), danger]
    { button.setIcon(svg_icon(name, themed(danger ? "danger" : "text_secondary"), button.iconSize())); };
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
    button->setFixedHeight(60);
    profile_icon(*button, icon, danger);
    return button;
}

QPushButton* profile_entry(QWidget* parent, QString const& text, QStringView icon, bool danger, bool navigation)
{
    auto* button = new profile_row(text, parent, danger, navigation);
    button->setAutoDefault(false);
    button->setProperty("profileEntry", true);
    button->setProperty("danger", danger);
    button->setIconSize({24, 24});
    button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    profile_icon(*button, icon, danger);
    return button;
}

void display_notification_action(QToolButton& button, bool muted)
{
    auto const action = muted ? QStringLiteral("取消静音") : QStringLiteral("静音");
    button.setText(action);
    button.setToolTip(action);
    button.setAccessibleName(action);
    button.setIcon(svg_icon(muted ? u"bell-off" : u"bell", themed("text_secondary"), button.iconSize()));
}
