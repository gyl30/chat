#include "message_markdown.hpp"
#include "message_links.hpp"
#include "theme_manager.hpp"

#include <QAbstractTextDocumentLayout>
#include <QImage>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>

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
    setMarkdown(source, MarkdownFeatures(MarkdownDialectGitHub) | MarkdownNoHTML);
    // Work backwards so replacing an image with its description preserves earlier positions.
    for (auto block = lastBlock(); block.isValid(); block = block.previous())
    {
        QTextCursor cursor(block);
        auto format = block.blockFormat();
        bool const fenced = format.hasProperty(QTextFormat::BlockCodeLanguage);
        if (fenced)
        {
            format.setNonBreakableLines(false);
            format.setBackground(themed("subtle"));
            cursor.setBlockFormat(format);
        }
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
    setTextWidth(width);
    setLayoutEnabled(true);
    setTextWidth(qBound(1., idealWidth(), qreal(width)));
}

QString message_markdown::code_at(QPointF point) const
{
    int pos = documentLayout()->hitTest(point, Qt::ExactHit);
    if (pos < 0)
    {
        return {};
    }
    auto block = findBlock(pos);
    auto code = [](QTextBlock const& b) { return b.isValid() && b.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage); };
    if (!code(block))
    {
        return {};
    }
    while (code(block.previous()))
    {
        block = block.previous();
    }
    QStringList lines;
    for (; code(block); block = block.next())
    {
        lines << block.text();
    }
    return lines.join(u'\n');
}
