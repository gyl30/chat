#include "emoji_text.hpp"

#include <algorithm>
#include <memory>

#include <QFontDatabase>
#include <QFontInfo>
#include <QGlyphRun>
#include <QInputMethodEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>

QFont emoji_input_font(QFont font)
{
    auto const available = QFontDatabase::families();
    auto const emoji = QStringLiteral("Noto Color Emoji");
    if (!available.contains(emoji)) { return font; }
    auto const primary = QFontInfo(font).family();
    if (primary.isEmpty() || !available.contains(primary)) { return font; }
    auto families = font.families();
    if (families.isEmpty() || families.front() != primary) { families.prepend(primary); }
    if (!families.contains(emoji)) { families.append(emoji); }
    font.setFamilies(families);
    return font;
}

QList<QTextLayout::FormatRange> emoji_formats(QString const& text, QFont const& font)
{
    QList<QTextLayout::FormatRange> formats;
    if (!QFontDatabase::families().contains(QStringLiteral("Noto Color Emoji"))) { return formats; }
    auto emoji_font = font;
    emoji_font.setFamily(QStringLiteral("Noto Color Emoji"));
    emoji_font.setStyleStrategy(QFont::NoFontMerging);
    for (auto const& segment : emoji_segments(text))
    {
        QTextLayout layout(text.mid(segment.first, segment.second), emoji_font);
        layout.beginLayout();
        auto line = layout.createLine();
        if (line.isValid()) { line.setLineWidth(1000000); }
        layout.endLayout();
        auto const runs = layout.glyphRuns();
        if (runs.isEmpty() || !std::ranges::all_of(runs, [](auto const& run) {
            auto const glyphs = run.glyphIndexes();
            return run.rawFont().familyName() == QStringLiteral("Noto Color Emoji") && !glyphs.isEmpty() &&
                std::ranges::none_of(glyphs, [](auto glyph) { return glyph == 0; });
        }))
        {
            continue;
        }
        int visible_glyphs = 0;
        for (auto const& run : runs)
        {
            for (auto glyph : run.glyphIndexes())
            {
                if (!run.rawFont().boundingRect(glyph).isEmpty()) { ++visible_glyphs; }
            }
        }
        // Font coverage alone does not prove that an emoji sequence composed.
        if (visible_glyphs != 1) { continue; }
        QTextCharFormat format;
        format.setFont(emoji_font);
        formats.push_back({segment.first, segment.second, format});
    }
    return formats;
}

void paint_emoji_line(QPainter& painter, QRect const& rect, QString const& text,
                      QFont const& font, QColor const& color, Qt::Alignment horizontal_alignment)
{
    if (rect.width() <= 0 || rect.height() <= 0 || text.isEmpty()) { return; }
    auto displayed = text;
    displayed.replace(QLatin1Char('\n'), QLatin1Char(' '));
    displayed.replace(QLatin1Char('\r'), QLatin1Char(' '));
    displayed.replace(QLatin1Char('\t'), QLatin1Char(' '));
    auto const make_layout = [&](QString const& value) {
        auto layout = std::make_unique<QTextLayout>(value, font);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        option.setAlignment(horizontal_alignment);
        layout->setTextOption(option);
        layout->setFormats(emoji_formats(value, font));
        layout->beginLayout();
        auto line = layout->createLine();
        if (line.isValid()) { line.setLineWidth(rect.width()); }
        layout->endLayout();
        return layout;
    };
    auto layout = make_layout(displayed);
    if (layout->lineCount() == 0) { return; }
    if (layout->lineAt(0).naturalTextWidth() > rect.width())
    {
        auto const boundaries = grapheme_ends(displayed);
        qsizetype begin = 0;
        qsizetype end = boundaries.size();
        layout = make_layout(QString(QChar(0x2026)));
        if (layout->lineAt(0).naturalTextWidth() > rect.width()) { return; }
        while (begin < end)
        {
            auto const middle = begin + (end - begin) / 2;
            auto candidate = make_layout(displayed.left(boundaries[middle]) + QChar(0x2026));
            if (candidate->lineAt(0).naturalTextWidth() <= rect.width())
            {
                layout = std::move(candidate);
                begin = middle + 1;
            }
            else
            {
                end = middle;
            }
        }
    }
    auto const height = layout->lineAt(0).height();
    auto const top = rect.top() + (rect.height() - height) / 2;
    painter.save();
    painter.setPen(color);
    painter.setClipRect(rect, Qt::IntersectClip);
    layout->draw(&painter, QPointF(rect.left(), top));
    painter.restore();
}

emoji_label::emoji_label(QWidget* parent) : QLabel(parent)
{
    setTextFormat(Qt::PlainText);
}

void emoji_label::paintEvent(QPaintEvent* event)
{
    static_cast<void>(event);
    auto rect = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
    auto const label_indent = indent() < 0 ? (frameWidth() > 0 ? fontMetrics().horizontalAdvance(QLatin1Char('x')) / 2 : 0) : indent();
    rect.adjust(label_indent, 0, 0, 0);
    QPainter painter(this);
    paint_emoji_line(painter, rect, text(), font(), palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                                                foregroundRole()));
}

emoji_highlighter::emoji_highlighter(QPlainTextEdit* editor)
    : QSyntaxHighlighter(editor->document()), editor_(editor)
{
    editor_->installEventFilter(this);
}

void emoji_highlighter::highlightBlock(QString const& text)
{
    if (!currentBlock().layout()->preeditAreaText().isEmpty()) { return; }
    for (auto const& range : emoji_formats(text, document()->defaultFont()))
    {
        setFormat(range.start, range.length, range.format);
    }
}

bool emoji_highlighter::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == editor_ && (event->type() == QEvent::InputMethod || event->type() == QEvent::FontChange))
    {
        auto const block = editor_->textCursor().block();
        if (event->type() == QEvent::InputMethod &&
            !static_cast<QInputMethodEvent*>(event)->preeditString().isEmpty())
        {
            // Qt expands display formats through inserted preedit text; never force its font.
            block.layout()->setFormats({});
            document()->markContentsDirty(block.position(), block.length());
        }
        QTimer::singleShot(0, this, [this, block, font_changed = event->type() == QEvent::FontChange] {
            if (font_changed) { rehighlight(); }
            auto const update_block = [this](QTextBlock const& target) {
                if (!target.isValid()) { return; }
                rehighlightBlock(target);
                auto* layout = target.layout();
                auto const preedit_length = layout->preeditAreaText().size();
                if (preedit_length == 0) { return; }
                auto const position = layout->preeditAreaPosition();
                auto formats = layout->formats();
                for (auto range : emoji_formats(target.text(), document()->defaultFont()))
                {
                    if (range.start >= position) { range.start += preedit_length; }
                    else if (range.start + range.length > position) { continue; }
                    formats.push_back(range);
                }
                layout->setFormats(formats);
                document()->markContentsDirty(target.position(), target.length());
            };
            update_block(block);
            auto const current = editor_->textCursor().block();
            if (current != block) { update_block(current); }
        });
    }
    return QSyntaxHighlighter::eventFilter(watched, event);
}
