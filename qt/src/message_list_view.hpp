#ifndef CHAT_QT_SRC_MESSAGE_LIST_VIEW_HPP
#define CHAT_QT_SRC_MESSAGE_LIST_VIEW_HPP

#include <QListView>
#include <QScrollBar>
#include <QTimer>
#include <functional>

// The overlay follows the real scrollbar without taking space away from message layout.
class message_list_view final : public QListView
{
    Q_OBJECT
   public:
    explicit message_list_view(QWidget* parent);
    std::function<void()> selection_finished;
    std::function<void(const std::function<void()>&)> preserve_layout;
    void show_location(QModelIndex const& index);
    bool is_location(QModelIndex const& index) const { return located_ == index; }
    bool selecting_range() const { return drag_start_.isValid(); }

   protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void dataChanged(const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) override;
    void paintEvent(QPaintEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

   private:
    void select_range(QModelIndex end);
    QPersistentModelIndex drag_start_;
    QPersistentModelIndex located_;
    QTimer location_timeout_;
    bool drag_select_ = false;
    QScrollBar overlay_;
    QTimer timer_;
};

#endif
