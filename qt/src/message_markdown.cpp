#include "message_markdown.hpp"
#include "message_links.hpp"
#include "theme_manager.hpp"

#include <QAbstractTextDocumentLayout>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFrame>
#include <QUrl>

namespace
{
constexpr int quote_indent = 13;
int quote_depth(QTextFormat const& format)
{
    // Keep a readable text column even in deeply nested quotations.
    return qMin(6, format.intProperty(QTextFormat::BlockQuoteLevel));
}
}    // namespace

QVariant message_markdown::loadResource(int, QUrl const&)
{
    // The default loader can open file: and relative paths. Images have no data source in chat messages.
    return QImage{};
}

message_markdown::message_markdown(QString const& source, QFont const& font, int width, QList<mention_data> const& mentions)
{
    setUndoRedoEnabled(false);
    setLayoutEnabled(false);
    setDefaultFont(font);
    setDocumentMargin(0);
    setIndentWidth(22);
    setMarkdown(source, MarkdownFeatures(MarkdownDialectGitHub) | MarkdownNoHTML);
    // Work backwards so replacing an image with its description preserves earlier positions.
    for (auto block = lastBlock(); block.isValid(); block = block.previous())
    {
        QTextCursor cursor(block);
        auto format = block.blockFormat();
        bool const fenced = format.hasProperty(QTextFormat::BlockCodeLanguage);
        int const depth = quote_depth(format);
        if (depth)
        {
            // Fenced code carries its indent on the enclosing frame.
            format.setLeftMargin(fenced ? 0 : quote_indent * depth);
            format.setRightMargin(0);
        }
        if (fenced)
        {
            format.setNonBreakableLines(false);
        }
        else if (format.headingLevel())
        {
            format.setTopMargin(block == begin() ? 0 : 8);
            format.setBottomMargin(4);
        }
        else
        {
            format.setTopMargin(block == begin() ? 0 : block.textList() ? 1 : 4);
            format.setBottomMargin(block.textList() ? 1 : 3);
        }
        cursor.setBlockFormat(format);
        QList<QTextFragment> fragments;
        for (auto it = block.begin(); !it.atEnd(); ++it)
        {
            fragments.push_back(it.fragment());
        }
        for (auto it = fragments.crbegin(); it != fragments.crend(); ++it)
        {
            auto cf = it->charFormat();
            auto const pos = it->position();
            cursor.setPosition(pos);
            cursor.setPosition(pos + it->length(), QTextCursor::KeepAnchor);
            if (depth && !fenced && !cf.isAnchor())
            {
                cf.setForeground(themed("text_secondary"));
                cursor.setCharFormat(cf);
            }
            if (!fenced && cf.fontFixedPitch())
            {
                cf.setBackground(themed("subtle"));
                cf.setForeground(themed("text"));
                cursor.setCharFormat(cf);
            }
            if (cf.isImageFormat())
            {
                cursor.insertText(cf.stringProperty(QTextFormat::ImageAltText), QTextCharFormat{});
                if (cursor.hasSelection())
                {
                    cursor.removeSelectedText();
                }
            }
            else if (cf.isAnchor())
            {
                auto url = http_message_url(cf.anchorHref());
                cf.setAnchor(!url.isEmpty());
                cf.setAnchorHref(url.toString(QUrl::FullyEncoded));
                cf.setFontUnderline(!url.isEmpty());
                cf.setForeground(url.isEmpty() ? themed("text") : themed("link"));
                cursor.setCharFormat(cf);
            }
            else if (!fenced && !cf.fontFixedPitch())
            {
                for (auto const& range : message_links(it->text()))
                {
                    cursor.setPosition(pos + range.start);
                    cursor.setPosition(pos + range.start + range.length, QTextCursor::KeepAnchor);
                    cursor.mergeCharFormat(range.format);
                }
            }
        }
        for (auto const& mention : mentions)
        {
            QRegularExpression pattern(
                QStringLiteral("(?<![\\p{L}\\p{N}_@])@%1(?![\\p{L}\\p{N}_@])").arg(QRegularExpression::escape(mention.username)));
            auto matches = pattern.globalMatch(block.text());
            while (matches.hasNext())
            {
                auto m = matches.next();
                cursor.setPosition(block.position() + m.capturedStart());
                cursor.setPosition(block.position() + m.capturedEnd(), QTextCursor::KeepAnchor);
                QTextCharFormat highlight;
                highlight.setForeground(themed("link"));
                cursor.mergeCharFormat(highlight);
            }
        }
    }
    format_code_blocks(source);
    setTextWidth(width);
    setLayoutEnabled(true);
    setTextWidth(qBound(1., idealWidth(), qreal(width)));
}

namespace
{
constexpr int kCodeBlock = QTextFormat::UserProperty + 1;

bool is_code(QTextBlock const& block) { return block.isValid() && block.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage); }

void decorate_code_block(QTextBlock const& block, QTextBlock const& last, QString const& language)
{
    QTextCursor cursor(block);
    cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    QTextFrameFormat frame_format;
    frame_format.setBackground(themed("subtle"));
    frame_format.setPadding(7);
    int const depth = quote_depth(block.blockFormat());
    frame_format.setLeftMargin(depth * quote_indent);
    frame_format.setProperty(QTextFormat::BlockQuoteLevel, depth);
    frame_format.setTopMargin(4);
    frame_format.setBottomMargin(4);
    frame_format.setProperty(kCodeBlock, true);
    auto* frame = cursor.insertFrame(frame_format);

    cursor = frame->firstCursorPosition();
    auto const body_format = cursor.blockFormat();
    cursor.insertBlock(body_format);
    cursor = frame->firstCursorPosition();
    QTextBlockFormat header;
    header.setAlignment(Qt::AlignRight);
    header.setBottomMargin(4);
    cursor.setBlockFormat(header);
    QTextCharFormat label;
    label.setForeground(themed("text_secondary"));
    if (!language.isEmpty())
    {
        cursor.insertText(language + QStringLiteral("   "), label);
    }
    label.setAnchor(true);
    label.setAnchorHref(QStringLiteral("chat-copy:%1").arg(frame->objectIndex()));
    label.setFontUnderline(true);
    label.setForeground(themed("link"));
    cursor.insertText(QStringLiteral("复制代码"), label);
    // insertFrame leaves an empty separator paragraph on each side of the selected lines.
    for (auto separator : {frame->firstCursorPosition().block().previous(), frame->lastCursorPosition().block().next()})
    {
        if (separator.isValid() && separator.length() == 1)
        {
            QTextBlockFormat gap;
            gap.setLineHeight(1, QTextBlockFormat::FixedHeight);
            QTextCursor(separator).setBlockFormat(gap);
        }
    }
}
}    // namespace

void message_markdown::format_code_blocks(QString const& source)
{
    // Qt preserves the language and delimiter character, but merges the formats of adjacent fences.
    // Use the original delimiters to partition only the lines its Markdown reader marked as code.
    static QRegularExpression const fence(QStringLiteral(R"(^[ \t>]*(?:(?:[-+*]|\d+[.)])\s+)?(\x60{3,}|~{3,})(.*)$)"));
    auto const lines = QStringView(source).split(u'\n');
    auto block = begin();
    for (qsizetype i = 0; i < lines.size(); ++i)
    {
        while (block.isValid() && !block.blockFormat().hasProperty(QTextFormat::BlockCodeFence))
        {
            block = block.next();
        }
        if (!block.isValid())
        {
            break;
        }
        auto const open = fence.matchView(lines[i]);
        auto const marker = open.capturedView(1);
        auto const language = block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage);
        if (!open.hasMatch() || !marker.startsWith(block.blockFormat().stringProperty(QTextFormat::BlockCodeFence)) ||
            open.captured(2).simplified().section(u' ', 0, 0) != language)
        {
            continue;
        }
        auto const prefix = lines[i].first(open.capturedStart(1));
        bool const in_list = block.textList() || block.blockFormat().indent() > 0;
        auto next = block;
        qsizetype end = i + 1;
        for (; end < lines.size(); ++end)
        {
            auto line = lines[end];
            if (line.endsWith(u'\r'))
            {
                line.chop(1);
            }
            auto const close = fence.matchView(line);
            if (close.hasMatch() && close.capturedView(1).startsWith(marker) && close.capturedView(2).trimmed().isEmpty())
            {
                auto const preceding = line.first(close.capturedStart(1));
                auto const indent = preceding.sliced(preceding.lastIndexOf(u'>') + 1);
                auto const base = in_list ? prefix.size() - prefix.lastIndexOf(u'>') - 1 : qsizetype(preceding.contains(u'>'));
                if (preceding.count(u'>') == prefix.count(u'>') && indent.trimmed().isEmpty() && indent.size() <= base + 3)
                {
                    break;
                }
            }
            if (!is_code(next) || next.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage) != language)
            {
                break;
            }
            // The native reader expands leading tabs to spaces; embedded tabs stay literal.
            auto const text = next.text();
            if (!line.endsWith(text) && !(line.contains(u'\t') && line.endsWith(QStringView(text).trimmed())))
            {
                break;
            }
            next = next.next();
        }
        if (next == block)
        {
            continue;
        }
        decorate_code_block(block, next.isValid() ? next.previous() : lastBlock(), language);
        block = next;
        i = end - 1;
    }
    // Indented Markdown code has no source fence or language; keep its native contiguous range.
    for (auto first = begin(); first.isValid();)
    {
        auto is_indented = [](QTextBlock const& b) { return is_code(b) && !b.blockFormat().hasProperty(QTextFormat::BlockCodeFence); };
        if (!is_indented(first))
        {
            first = first.next();
            continue;
        }
        auto last = first;
        while (is_indented(last.next()))
        {
            last = last.next();
        }
        auto next = last.next();
        decorate_code_block(first, last, {});
        first = next;
    }
}

namespace
{
QString frame_code(QTextFrame const* frame)
{
    if (!frame || !frame->frameFormat().boolProperty(kCodeBlock))
    {
        return {};
    }
    QStringList lines;
    for (auto block = frame->firstCursorPosition().block().next(); block.isValid() && block.position() <= frame->lastPosition(); block = block.next())
    {
        lines.append(block.text());
    }
    return lines.join(u'\n');
}
}    // namespace

QString message_markdown::code_at(QPointF point) const
{
    int const pos = documentLayout()->hitTest(point, Qt::ExactHit);
    return pos < 0 ? QString{} : frame_code(frameAt(pos));
}

QString message_markdown::code_for_link(QUrl const& url) const
{
    return url.scheme() == QStringLiteral("chat-copy") ? frame_code(qobject_cast<QTextFrame*>(object(url.path().toInt()))) : QString{};
}

void message_markdown::draw_quote_guides(QPainter& painter) const
{
    // Use native document coordinates for both the delegate and the continuous reader.
    int previous_depth = 0;
    qreal previous_bottom = 0;
    for (auto it = rootFrame()->begin(); !it.atEnd(); ++it)
    {
        auto* frame = it.currentFrame();
        auto block = it.currentBlock();
        int const depth = frame ? quote_depth(frame->frameFormat()) : quote_depth(block.blockFormat());
        if (!depth)
        {
            if (block.isValid() && block.length() > 1)
            {
                previous_depth = 0;
            }
            continue;
        }
        QRectF rect = frame ? documentLayout()->frameBoundingRect(frame) : documentLayout()->blockBoundingRect(block);
        for (int level = 0; level < depth; ++level)
        {
            qreal top = level < previous_depth ? qMin(previous_bottom, rect.top()) : rect.top();
            painter.fillRect(QRectF(documentMargin() + level * quote_indent + 2, top, 2, rect.bottom() - top), themed("border_strong"));
        }
        previous_depth = depth;
        previous_bottom = rect.bottom();
    }
}
