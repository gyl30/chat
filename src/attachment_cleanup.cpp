#include "attachment_cleanup.hpp"

#include <utility>
#include <chat/attachment.hpp>
#include "timestamp.hpp"
#include <sqlite3.h>

std::expected<void, sqlite_error> reserve_attachment_object(sqlite_database& db, std::string_view key, std::int64_t timestamp)
{
    if (auto error = db.exec(R"SQL(
        INSERT INTO attachment_cleanup(object_key,reserved_at)
        SELECT ?1,?2 WHERE NOT EXISTS(SELECT 1 FROM files WHERE object_key=?1)
    )SQL",
                             key,
                             timestamp);
        error)
    {
        return std::unexpected(std::move(error));
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
            WHERE reserved_at<=?1 AND object_key>?2 AND NOT EXISTS(SELECT 1 FROM attachment_uploads u WHERE u.object_key=attachment_cleanup.object_key)
                AND (reserved_at=0 OR NOT EXISTS(SELECT 1 FROM files f WHERE f.object_key=attachment_cleanup.object_key AND f.created_at>?3))
                AND NOT EXISTS(SELECT 1 FROM conversation_files c WHERE c.object_key=attachment_cleanup.object_key)
                AND NOT EXISTS(SELECT 1 FROM message_attachments m WHERE m.object_key=attachment_cleanup.object_key)
            ORDER BY object_key LIMIT 64
        )
        RETURNING object_key
    )SQL",
                                cutoff,
                                after,
                                epoch_milliseconds() - chat::upload_retention_seconds * 1000);
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
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(tx.error());
    }
    if (auto e =
            db.exec("DELETE FROM files WHERE object_key=?1 AND EXISTS(SELECT 1 FROM attachment_cleanup WHERE object_key=?1 AND reserved_at=0)", key);
        e)
    {
        return std::unexpected(e);
    }
    if (auto error = db.exec("DELETE FROM attachment_cleanup WHERE object_key=?1", key); error)
    {
        return std::unexpected(std::move(error));
    }
    if (auto e = tx->commit(); e)
    {
        return std::unexpected(e);
    }
    return {};
}
