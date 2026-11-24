#include "message_links.hpp"
#include "theme_manager.hpp"

#include <QRegularExpression>

QUrl http_message_url(QString const& target)
{
    QUrl url(target, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")))
    {
        return {};
    }
    return url;
}

QList<QTextLayout::FormatRange> message_links(QString const& body)
{
    static QRegularExpression const pattern(QStringLiteral(R"re((?<![a-zA-Z0-9_+./:-])https?://[^\s<>"“”‘’《》，。！？；：…、（）【】]+)re"),
                                            QRegularExpression::CaseInsensitiveOption);
    QList<QTextLayout::FormatRange> ranges;
    auto matches = pattern.globalMatch(body);
    while (matches.hasNext())
    {
        auto match = matches.next();
        auto target = match.captured();
        auto unmatched_parens = target.count(u')') - target.count(u'(');
        while (!target.isEmpty())
        {
            auto last = target.back();
            if (last == u')' && unmatched_parens > 0)
            {
                --unmatched_parens;
            }
            else if (!QStringLiteral(".,;:!?'").contains(last))
            {
                break;
            }
            target.chop(1);
        }
        if (auto url = http_message_url(target); !url.isEmpty())
        {
            QTextCharFormat format;
            format.setAnchor(true);
            format.setAnchorHref(url.toString(QUrl::FullyEncoded));
            format.setFontUnderline(true);
            format.setForeground(themed("link"));
            ranges.push_back({static_cast<int>(match.capturedStart()), static_cast<int>(target.size()), format});
        }
    }
    return ranges;
}
