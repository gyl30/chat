#include "empty_state.hpp"
#include "icons.hpp"
#include "theme_manager.hpp"

#include <QEvent>
#include <QLabel>
#include <QVBoxLayout>

empty_state::empty_state(QWidget& viewport, QString name) : QWidget(&viewport)
{
    setObjectName(name.isEmpty() ? QStringLiteral("emptyState") : std::move(name));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 4, 12, 4);
    layout->setSpacing(5);
    layout->addStretch(2);
    icon_ = new QLabel(this);
    layout->addWidget(icon_, 0, Qt::AlignHCenter);
    text_ = new QLabel(this);
    text_->setObjectName(QStringLiteral("emptyStateText"));
    hint_ = new QLabel(this);
    hint_->setObjectName(QStringLiteral("emptyStateHint"));
    for (auto* label : {text_, hint_})
    {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);
    }
    layout->addStretch(3);
    viewport.installEventFilter(this);
    setGeometry(viewport.rect());
    hide();
    connect(&theme_manager::instance(), &theme_manager::changed, this, &empty_state::refresh_icon);
}

void empty_state::set_message(QString const& text, QStringView icon, QString const& hint)
{
    text_->setText(text);
    setAccessibleName(text);
    hint_->setText(hint);
    hint_->setVisible(!hint.isEmpty());
    if (glyph_ != icon)
    {
        glyph_ = icon.toString();
        refresh_icon();
    }
    icon_->setVisible(!glyph_.isEmpty());
    setVisible(!text.isEmpty());
    if (!text.isEmpty())
    {
        raise();
    }
}

void empty_state::refresh_icon()
{
    if (!glyph_.isEmpty())
    {
        icon_->setPixmap(svg_icon(glyph_, themed("text_muted"), {32, 32}).pixmap(QSize(32, 32), devicePixelRatioF()));
    }
}

bool empty_state::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize)
    {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}
