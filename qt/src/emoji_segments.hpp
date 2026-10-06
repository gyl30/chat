#pragma once

#include <QList>
#include <QPair>
#include <QString>

// All offsets refer to the original QString, in UTF-16 code units.
// Classification is display-only: neither these helpers nor the scanner rewrite text.
QList<QPair<int, int>> emoji_segments(QString const& text);
QList<int> grapheme_ends(QString const& text);
QString grapheme_prefix(QString const& text, qsizetype limit);
