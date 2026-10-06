#include "emoji_segments.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <vector>

#include <utf8proc.h>

#include "emoji_properties.hpp"

namespace
{

using emoji_text_iter_t = unsigned char const*;
#include <emoji_presentation_scanner.c>

struct scalar
{
    utf8proc_int32_t value;
    int begin;
    int end;
};

std::vector<scalar> scalars(QString const& text)
{
    std::vector<scalar> result;
    if (text.size() > std::numeric_limits<int>::max()) { return result; }
    for (int position = 0; position < text.size();)
    {
        auto const begin = position;
        auto const first = text.at(position++);
        auto value = static_cast<utf8proc_int32_t>(first.unicode());
        if (first.isHighSurrogate() && position < text.size() && text.at(position).isLowSurrogate())
        {
            value = static_cast<utf8proc_int32_t>(QChar::surrogateToUcs4(first, text.at(position++)));
        }
        else if (first.isSurrogate())
        {
            value = -1; // Preserve an unpaired original code unit as its own boundary.
        }
        result.push_back({value, begin, position});
    }
    return result;
}

QList<int> ends(std::vector<scalar> const& values)
{
    QList<int> result;
    utf8proc_int32_t state = 0;
    for (std::size_t i = 1; i < values.size(); ++i)
    {
        if (values[i - 1].value < 0 || values[i].value < 0)
        {
            result.push_back(values[i - 1].end);
            state = 0;
        }
        else if (utf8proc_grapheme_break_stateful(values[i - 1].value, values[i].value, &state))
        {
            result.push_back(values[i - 1].end);
        }
    }
    if (!values.empty()) { result.push_back(values.back().end); }
    return result;
}

template<std::size_t N>
bool property(chat_unicode::interval const (&ranges)[N], utf8proc_int32_t value)
{
    if (value < 0) { return false; }
    auto const point = static_cast<char32_t>(value);
    auto const found = std::lower_bound(std::begin(ranges), std::end(ranges), point,
        [](auto const& interval, char32_t point) { return interval.last < point; });
    return found != std::end(ranges) && found->first <= point;
}

unsigned char category(utf8proc_int32_t value)
{
    // Categories are the unmodified Google 0.4.0 grammar's public input alphabet.
    if (value == 0x20e3) { return 8; }
    if (value == 0x20e0) { return 9; }
    if (value == 0x200d) { return 10; }
    if (value == 0xfe0e) { return 11; }
    if (value == 0xfe0f) { return 12; }
    if (value == 0x1f3f4) { return 13; }
    if ((value >= 0xe0030 && value <= 0xe0039) || (value >= 0xe0061 && value <= 0xe007a)) { return 14; }
    if (value == 0xe007f) { return 15; }
    if (property(chat_unicode::Emoji_Modifier_Base, value)) { return 3; }
    if (property(chat_unicode::Emoji_Modifier, value)) { return 4; }
    if (value >= 0x1f1e6 && value <= 0x1f1ff) { return 6; }
    if (value == '#' || value == '*' || (value >= '0' && value <= '9')) { return 7; }
    if (property(chat_unicode::Emoji_Presentation, value)) { return 2; }
    if (property(chat_unicode::Emoji, value)) { return 1; }
    return 16; // Unknown/text: the upstream scanner's any branch.
}

struct token
{
    int begin;
    int end;
    bool emoji;
};

std::vector<token> tokens(std::vector<scalar> const& values)
{
    std::vector<unsigned char> units;
    std::vector<QPair<int, int>> offsets;
    units.reserve(values.size());
    offsets.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        auto const begin = values[i].begin;
        auto const value = values[i].value;
        units.push_back(category(value));
        // UTS#51 compatibility relationship: precisely base + one VS16 + modifier.
        // Only the classification unit spans VS16; original UTF-16 remains untouched.
        if (property(chat_unicode::Emoji_Modifier_Base, value) && i + 2 < values.size() &&
            values[i + 1].value == 0xfe0f && property(chat_unicode::Emoji_Modifier, values[i + 2].value))
        {
            ++i;
        }
        offsets.push_back({begin, values[i].end});
    }
    std::vector<token> result;
    if (units.empty()) { return result; }
    auto const finish = units.data() + units.size();
    for (emoji_text_iter_t current = units.data(); current < finish;)
    {
        bool is_emoji = false;
        bool has_vs = false;
        auto const next = scan_emoji_presentation(current, finish, &is_emoji, &has_vs);
        if (next <= current || next > finish) { return {}; }
        auto const first = static_cast<std::size_t>(current - units.data());
        auto const last = static_cast<std::size_t>(next - units.data() - 1);
        result.push_back({offsets[first].first, offsets[last].second, is_emoji});
        current = next;
    }
    return result;
}

} // namespace

QList<int> grapheme_ends(QString const& text)
{
    return ends(scalars(text));
}

QString grapheme_prefix(QString const& text, qsizetype limit)
{
    if (limit <= 0) { return {}; }
    if (limit >= text.size()) { return text; }
    auto const boundaries = grapheme_ends(text);
    auto const after = std::upper_bound(boundaries.cbegin(), boundaries.cend(), limit);
    return after == boundaries.cbegin() ? QString{} : text.left(*std::prev(after));
}

QList<QPair<int, int>> emoji_segments(QString const& text)
{
    auto const values = scalars(text);
    auto const boundaries = ends(values);
    auto const classified = tokens(values);
    QList<QPair<int, int>> result;
    std::size_t first_token = 0;
    int begin = 0;
    for (auto const boundary : boundaries)
    {
        while (first_token < classified.size() && classified[first_token].end <= begin) { ++first_token; }
        auto index = first_token;
        auto all_emoji = index < classified.size() && classified[index].begin == begin;
        auto last_end = begin;
        while (index < classified.size() && classified[index].begin < boundary)
        {
            auto const& current = classified[index++];
            all_emoji = all_emoji && current.emoji && current.begin >= begin && current.end <= boundary;
            last_end = current.end;
        }
        if (all_emoji && last_end == boundary) { result.push_back({begin, boundary - begin}); }
        begin = boundary;
    }
    return result;
}
