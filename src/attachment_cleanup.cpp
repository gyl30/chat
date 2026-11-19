#include "attachment_cleanup.hpp"

#include <utility>
#include <sqlite3.h>

std::expected<void, sqlite_error> reserve_attachment_object(sqlite_database& db, std::string_view key, std::int64_t timestamp)
{
    auto statement = db.prepare(R"SQL(
        INSERT INTO attachment_cleanup(object_key,reserved_at)
        SELECT ?1,?2 WHERE NOT EXISTS(SELECT 1 FROM message_attachments WHERE object_key=?1)
    )SQL",
                                key,
                                timestamp);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    auto result = statement->step();
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    if (db.changes() != 1)
    {
        return std::unexpected(sqlite_error{SQLITE_CONSTRAINT, "attachment object already belongs to a message"});
    }
    return {};
}

std::expected<std::vector<std::string>, sqlite_error> claim_attachment_cleanup_batch(sqlite_database& db, std::int64_t cutoff, std::string_view after)
{
    auto statement = db.prepare(R"SQL(
        UPDATE attachment_cleanup SET reserved_at=0 WHERE object_key IN (
            SELECT object_key FROM attachment_cleanup
            WHERE reserved_at<=?1 AND object_key>?2 ORDER BY object_key LIMIT 64
        )
        RETURNING object_key
    )SQL",
                                cutoff,
                                after);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<std::string> entries;
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
        entries.emplace_back(statement->column_text(0));
    }
}

std::expected<void, sqlite_error> acknowledge_attachment_cleanup(sqlite_database& db, std::string_view key)
{
    auto statement = db.prepare("DELETE FROM attachment_cleanup WHERE object_key=?1", key);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    auto result = statement->step();
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    return {};
}
