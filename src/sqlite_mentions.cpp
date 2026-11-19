#include "sqlite_mentions.hpp"

#include <algorithm>
#include <vector>

#include <boost/locale/utf.hpp>
#include <utf8proc.h>
#include <chat/message.hpp>

namespace
{
bool mention_word(std::string_view::const_iterator character, std::string_view::const_iterator end)
{
    if (character == end)
    {
        return false;
    }
    auto const value = boost::locale::utf::utf_traits<char>::decode(character, end);
    if (value == '@' || value == '_')
    {
        return true;
    }
    if (value > 0x10ffff)
    {
        return false;
    }
    auto const category = utf8proc_category(value);
    return (category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_LO) ||
           (category >= UTF8PROC_CATEGORY_ND && category <= UTF8PROC_CATEGORY_NO);
}
}    // namespace

sqlite_error replace_message_mentions(sqlite_database& db, std::int64_t conversation, std::int64_t message, std::string_view text)
{
    if (auto clear = db.prepare("DELETE FROM message_mentions WHERE message_id=?1", message); clear)
    {
        if (auto done = clear->step(); !done)
        {
            return std::move(done.error());
        }
    }
    else
    {
        return std::move(clear.error());
    }
    if (!text.contains('@'))
    {
        return {};
    }
    auto users = db.prepare(R"SQL(
        SELECT u.id,u.username FROM conversation_members m JOIN users u ON u.id=m.user_id
        JOIN conversations c ON c.id=m.conversation_id
        WHERE c.id=?1 AND c.kind='group' AND instr(?2,'@'||u.username)>0
    )SQL",
                            conversation,
                            text);
    if (!users)
    {
        return std::move(users.error());
    }
    std::vector<chat::mention> candidates;
    for (;;)
    {
        auto row = users->step();
        if (!row)
        {
            return std::move(row.error());
        }
        if (!*row)
        {
            break;
        }
        candidates.push_back({users->column_int64(0), std::string(users->column_text(1))});
    }
    if (candidates.empty())
    {
        return {};
    }
    std::ranges::sort(candidates, [](auto const& left, auto const& right) { return left.username.size() > right.username.size(); });
    auto write = db.prepare(R"SQL(
        INSERT INTO message_mentions(message_id,user_id) VALUES(?1,?2)
        ON CONFLICT(message_id,user_id) DO NOTHING
    )SQL");
    if (!write)
    {
        return std::move(write.error());
    }
    if (auto error = write->bind(1, message); error)
    {
        return error;
    }

    for (auto position = text.find('@'); position != std::string_view::npos; position = text.find('@', position + 1))
    {
        if (position > 0)
        {
            auto previous = text.begin() + position - 1;
            while (previous != text.begin() && (static_cast<unsigned char>(*previous) & 0xc0) == 0x80)
            {
                --previous;
            }
            if (mention_word(previous, text.begin() + position))
            {
                continue;
            }
        }
        auto const suffix = text.substr(position + 1);
        for (auto const& candidate : candidates)
        {
            if (!suffix.starts_with(candidate.username) || mention_word(suffix.begin() + candidate.username.size(), suffix.end()))
            {
                continue;
            }
            if (auto error = write->bind(2, candidate.user); error)
            {
                return error;
            }
            if (auto result = write->step(); !result)
            {
                return std::move(result.error());
            }
            if (auto error = write->reset(); error)
            {
                return error;
            }
            position += candidate.username.size();
            break;
        }
    }
    return {};
}
