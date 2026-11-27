#pragma once

#include <QWidget>

class QLabel;

// A status in the viewport, independent of the list model and its selection.
class empty_state final : public QWidget
{
   public:
    explicit empty_state(QWidget& viewport, QString name = {});
    void set_message(QString const& text = {}, QStringView icon = {}, QString const& hint = {});

   private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void refresh_icon();

    QLabel* icon_;
    QLabel* text_;
    QLabel* hint_;
    QString glyph_;
};
