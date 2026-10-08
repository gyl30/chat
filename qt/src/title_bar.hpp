#ifndef CHAT_QT_SRC_TITLE_BAR_HPP
#define CHAT_QT_SRC_TITLE_BAR_HPP

#include <QWidget>

class QLabel;
class QToolButton;

// Drawn by the application rather than the platform, so every desktop shows the same chrome.
// Moving and resizing are still delegated to the window system through QWindow.
class title_bar final : public QWidget
{
    Q_OBJECT

   public:
    explicit title_bar(QWidget* parent = nullptr);
    void set_title_visible(bool visible);
    void set_maximizable(bool maximizable);
    void add_tool_button(QToolButton* button);
    void refresh_theme();

   protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

   private:
    void toggle_maximized();
    void update_maximize_button();

    QLabel* title_ = nullptr;
    QToolButton* minimize_ = nullptr;
    QToolButton* maximize_ = nullptr;
    QToolButton* close_ = nullptr;
};

#endif
