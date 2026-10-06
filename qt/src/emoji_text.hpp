#pragma once

#include <QLabel>
#include <QSyntaxHighlighter>
#include <QTextLayout>

#include "emoji_segments.hpp"

class QPainter;
class QPlainTextEdit;

QList<QTextLayout::FormatRange> emoji_formats(QString const& text, QFont const& font);
void paint_emoji_line(QPainter& painter, QRect const& rect, QString const& text,
                      QFont const& font, QColor const& color);

class emoji_label final : public QLabel
{
public:
    explicit emoji_label(QWidget* parent);

protected:
    void paintEvent(QPaintEvent* event) override;
};

class emoji_highlighter final : public QSyntaxHighlighter
{
public:
    explicit emoji_highlighter(QPlainTextEdit* editor);

protected:
    void highlightBlock(QString const& text) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QPlainTextEdit* editor_;
};
