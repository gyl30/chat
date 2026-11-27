#include "message_list_view.hpp"
#include "message_delegate.hpp"
#include "message_model.hpp"
#include "theme_manager.hpp"
#include <QApplication>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <algorithm>

message_list_view::message_list_view(QWidget* parent) : QListView(parent), overlay_(Qt::Vertical, this)
{
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto& themes = theme_manager::instance();
    connect(&themes, &theme_manager::background_changed, viewport(), qOverload<>(&QWidget::update));
    connect(&themes, &theme_manager::changed, viewport(), qOverload<>(&QWidget::update));
    overlay_.setObjectName(QStringLiteral("messageScrollOverlay"));
    overlay_.setFocusPolicy(Qt::NoFocus);
    overlay_.hide();
    location_timeout_.setSingleShot(true);
    connect(&location_timeout_,
            &QTimer::timeout,
            this,
            [this]
            {
                auto const old = located_;
                located_ = QModelIndex{};
                update(old);
            });
    timer_.setSingleShot(true);
    timer_.setInterval(900);
    connect(&timer_,
            &QTimer::timeout,
            this,
            [this]
            {
                if (!overlay_.isSliderDown())
                {
                    overlay_.hide();
                }
            });
    auto* scroll = verticalScrollBar();
    connect(scroll, &QScrollBar::rangeChanged, &overlay_, &QScrollBar::setRange);
    connect(scroll,
            &QScrollBar::valueChanged,
            this,
            [this, scroll](int value)
            {
                overlay_.setPageStep(scroll->pageStep());
                overlay_.setValue(value);
                overlay_.setVisible(scroll->maximum() > scroll->minimum());
                timer_.start();
            });
    connect(&overlay_, &QScrollBar::valueChanged, scroll, &QScrollBar::setValue);
    connect(&overlay_, &QScrollBar::sliderReleased, &timer_, qOverload<>(&QTimer::start));
}

void message_list_view::show_location(QModelIndex const& index)
{
    auto const old = located_;
    located_ = index;
    update(old);
    update(index);
    location_timeout_.start(1600);
}

void message_list_view::mousePressEvent(QMouseEvent* event)
{
    if (selectionMode() != ExtendedSelection)
    {
        QListView::mousePressEvent(event);
        return;
    }
    if (event->button() != Qt::LeftButton)
    {
        return;
    }
    auto hit = indexAt(event->position().toPoint());
    if (!(hit.flags() & Qt::ItemIsSelectable))
    {
        return;
    }
    setFocus(Qt::MouseFocusReason);
    selectionModel()->select(QItemSelection{}, QItemSelectionModel::Select);
    const bool range = event->modifiers().testFlag(Qt::ShiftModifier) && currentIndex().isValid();
    drag_start_ = range ? currentIndex() : hit;
    drag_select_ = range || !selectionModel()->isSelected(hit);
    if (!range)
    {
        selectionModel()->setCurrentIndex(hit, QItemSelectionModel::NoUpdate);
    }
    select_range(hit);
}

void message_list_view::mouseMoveEvent(QMouseEvent* event)
{
    if (selectionMode() != ExtendedSelection)
    {
        QListView::mouseMoveEvent(event);
        return;
    }
    if (!drag_start_.isValid() || !(event->buttons() & Qt::LeftButton))
    {
        return;
    }
    auto pos = event->position().toPoint();
    if (pos.y() < 0 || pos.y() >= viewport()->height())
    {
        verticalScrollBar()->triggerAction(pos.y() < 0 ? QAbstractSlider::SliderSingleStepSub : QAbstractSlider::SliderSingleStepAdd);
        pos.setY(std::clamp(pos.y(), 0, viewport()->height() - 1));
    }
    auto hit = indexAt(pos);
    if (hit.isValid())
    {
        QStyleOptionViewItem option;
        initViewItemOption(&option);
        option.rect = visualRect(hit);
        if (!message_delegate::on_separator(option, hit, pos))
        {
            select_range(hit);
        }
    }
}

void message_list_view::mouseReleaseEvent(QMouseEvent* event)
{
    if (selectionMode() != ExtendedSelection)
    {
        QListView::mouseReleaseEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton)
    {
        selectionModel()->select(QItemSelection{}, QItemSelectionModel::Select);
        drag_start_ = QModelIndex{};
        selection_finished();
    }
}

void message_list_view::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (selectionMode() == ExtendedSelection)
    {
        mousePressEvent(event);
    }
    else
    {
        QListView::mouseDoubleClickEvent(event);
    }
}

void message_list_view::dataChanged(const QModelIndex& first, const QModelIndex& last, const QList<int>& roles)
{
    auto update = [&] { QListView::dataChanged(first, last, roles); };
    if (preserve_layout && (roles.empty() || roles.contains(Qt::SizeHintRole) || roles.contains(message_model::text_role) ||
                            roles.contains(message_model::reply_text_role) || roles.contains(message_model::sender_name_role) ||
                            roles.contains(message_model::image_role) || roles.contains(message_model::reactions_role) ||
                            roles.contains(message_model::poll_role) || roles.contains(message_model::download_status_role)))
    {
        preserve_layout(update);
    }
    else
    {
        update();
    }
}

void message_list_view::paintEvent(QPaintEvent* event)
{
    {
        QPainter painter(viewport());
        painter.setClipRegion(event->region());
        const auto& themes = theme_manager::instance();
        paint_chat_background(painter, viewport()->rect(), themes.background(), themes.is_dark());
    }
    QListView::paintEvent(event);
}

void message_list_view::scrollContentsBy(int dx, int dy)
{
    QListView::scrollContentsBy(dx, dy);
    if (theme_manager::instance().background() != chat_background::solid)
    {
        viewport()->update();
    }
}

void message_list_view::resizeEvent(QResizeEvent* event)
{
    if (preserve_layout)
    {
        preserve_layout([&] { QListView::resizeEvent(event); });
    }
    else
    {
        QListView::resizeEvent(event);
    }
    constexpr int width = 8;
    auto const area = viewport()->geometry();
    overlay_.setGeometry(area.right() + 1 - width, area.top(), width, area.height());
}

void message_list_view::select_range(QModelIndex end)
{
    auto first = QModelIndex(drag_start_);
    if (end.row() < first.row())
    {
        std::swap(first, end);
    }
    selectionModel()->select(QItemSelection(first, end),
                             (drag_select_ ? QItemSelectionModel::Select : QItemSelectionModel::Deselect) | QItemSelectionModel::Current);
}

void message_list_view::changeEvent(QEvent* event)
{
    if (preserve_layout && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange))
    {
        preserve_layout([&] { QListView::changeEvent(event); });
    }
    else
    {
        QListView::changeEvent(event);
    }
}
