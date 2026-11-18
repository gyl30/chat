#include "attachment_cleanup.hpp"

#include <utility>
#include <sqlite3.h>

sqlite_error reserve_attachment_object(sqlite_database& db, std::string_view key, std::int64_t timestamp)
{
    auto statement = db.prepare(R"SQL(
        INSERT INTO attachment_cleanup(object_key,reserved_at)
        SELECT ?1,?2 WHERE NOT EXISTS(SELECT 1 FROM message_attachments WHERE object_key=?1)
    )SQL", key, timestamp);
    if (!statement)
    {
        return std::move(statement.error());
    }
    auto result = statement->step();
    if (!result)
    {
        return std::move(result.error());
    }
    if (db.changes() != 1)
    {
        return {SQLITE_CONSTRAINT, "attachment object already belongs to a message"};
    }
    return {};
}

std::expected<std::vector<attachment_cleanup_entry>, sqlite_error> attachment_cleanup_batch(
    sqlite_database& db, std::int64_t cutoff, std::optional<attachment_cleanup_entry> const& after)
{
    std::optional<std::int64_t> after_time;
    if (after)
    {
        after_time = after->reserved_at;
    }
    auto statement = db.prepare(R"SQL(
        SELECT reserved_at,object_key FROM attachment_cleanup
        WHERE reserved_at<=?1 AND (?2 IS NULL OR (reserved_at,object_key)>(?2,?3))
        ORDER BY reserved_at,object_key LIMIT 64
    )SQL", cutoff, after_time, after ? std::string_view(after->object_key) : std::string_view{});
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<attachment_cleanup_entry> entries;
    for (;;)
    {
        auto row = statement->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row)
        {
            return entries;
        }
        entries.push_back({statement->column_int64(0), std::string(statement->column_text(1))});
    }
}

sqlite_error acknowledge_attachment_cleanup(sqlite_database& db, std::string_view key)
{
    auto statement = db.prepare("DELETE FROM attachment_cleanup WHERE object_key=?1", key);
    if (!statement)
    {
        return std::move(statement.error());
    }
    auto result = statement->step();
    return result ? sqlite_error{} : std::move(result.error());
}
