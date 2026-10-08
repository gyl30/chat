#include "title_bar.hpp"
#include "theme_manager.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>
#include <QWindow>

#include "icons.hpp"

namespace
{

QToolButton* make_button(QWidget* parent, QString const& name, QString const& accessible_name)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(name);
    button->setAccessibleName(accessible_name);
    button->setToolTip(accessible_name);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(18, 18));
    button->setFixedSize(46, 34);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

}

title_bar::title_bar(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("windowTitleBar"));
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(34);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 0, 0, 0);
    layout->setSpacing(0);
    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("windowTitle"));
    layout->addWidget(title_);
    layout->addStretch();
    QColor const icon_color(QStringLiteral("#3F4542"));
    minimize_ = make_button(this, QStringLiteral("windowMinimizeButton"), QStringLiteral("最小化"));
    minimize_->setIcon(svg_icon(u"window-minimize", icon_color, QSize(18, 18)));
    maximize_ = make_button(this, QStringLiteral("windowMaximizeButton"), QStringLiteral("最大化"));
    close_ = make_button(this, QStringLiteral("windowCloseButton"), QStringLiteral("关闭"));
    close_->setIcon(svg_icon(u"close", icon_color, QSize(18, 18)));
    layout->addWidget(minimize_);
    layout->addWidget(maximize_);
    layout->addWidget(close_);
    connect(minimize_, &QToolButton::clicked, this, [this] { window()->showMinimized(); });
    connect(maximize_, &QToolButton::clicked, this, [this] { toggle_maximized(); });
    connect(close_, &QToolButton::clicked, this, [this] { window()->close(); });
    if (parent) { parent->window()->installEventFilter(this); }
    title_->setText(window()->windowTitle());
    update_maximize_button();
}

void title_bar::add_tool_button(QToolButton* button)
{
    button->setParent(this);
    button->setFixedSize(46, 34);
    auto* row = static_cast<QHBoxLayout*>(layout());
    row->insertWidget(row->indexOf(minimize_), button);
}

void title_bar::refresh_theme()
{
    QColor const icon_color(QStringLiteral("#3F4542"));
    minimize_->setIcon(svg_icon(u"window-minimize", icon_color, QSize(18, 18)));
    close_->setIcon(svg_icon(u"close", icon_color, QSize(18, 18)));
    update_maximize_button();
}

void title_bar::set_title_visible(bool visible) { title_->setVisible(visible); }

void title_bar::set_maximizable(bool maximizable)
{
    maximize_->setVisible(maximizable);
}

void title_bar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && window()->windowHandle() && !window()->isMaximized())
    {
        window()->windowHandle()->startSystemMove();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void title_bar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && maximize_->isVisible())
    {
        toggle_maximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool title_bar::eventFilter(QObject* object, QEvent* event)
{
    if (object == window())
    {
        if (event->type() == QEvent::WindowStateChange) { update_maximize_button(); }
        else if (event->type() == QEvent::WindowTitleChange) { title_->setText(window()->windowTitle()); }
    }
    return QWidget::eventFilter(object, event);
}

void title_bar::toggle_maximized()
{
    if (window()->isMaximized()) { window()->showNormal(); }
    else { window()->showMaximized(); }
}

void title_bar::update_maximize_button()
{
    auto const maximized = window()->isMaximized();
    maximize_->setIcon(svg_icon(maximized ? u"window-restore" : u"window-maximize", QColor(QStringLiteral("#3F4542")), QSize(18, 18)));
    auto const name = maximized ? QStringLiteral("还原") : QStringLiteral("最大化");
    maximize_->setAccessibleName(name);
    maximize_->setToolTip(name);
}
