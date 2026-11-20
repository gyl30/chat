#include <chat/attachment.hpp>
#include "attachment_io.hpp"

#include <algorithm>
#include <utility>

#include "attachment_cleanup.hpp"
#include "database.hpp"
#include "s3_objects.hpp"
#include "timestamp.hpp"

boost::capy::task<bool> discard_upload(database& db, s3_objects& objects, std::int64_t id, std::string const& key, std::string const& multipart)
{
    if (!(co_await objects.abort(key, multipart)))
    {
        co_return false;
    }
    auto cleared = co_await db.run(
        [&](sqlite_database& sql) -> std::expected<void, sqlite_error>
        {
            if (auto e = sql.exec("DELETE FROM attachment_uploads WHERE id=? AND state='discarding'", id); e)
            {
                return std::unexpected(e);
            }
            return {};
        });
    co_return cleared.has_value();
}

boost::capy::task<std::int64_t> recover_attachment_objects(database& db, s3_objects& objects, std::int64_t cutoff)
{
    bool removal_failed = false;
    std::int64_t removed = 0;
    struct stale_upload
    {
        std::int64_t id;
        std::string key, multipart;
    };
    std::int64_t cursor = 0;
    for (;;)
    {
        auto stale = co_await db.run(
            [&](sqlite_database& sql) -> std::expected<std::vector<stale_upload>, sqlite_error>
            {
                auto q = sql.prepare(R"SQL(UPDATE attachment_uploads SET state='discarding' WHERE id IN
                (SELECT id FROM attachment_uploads WHERE id>?1 AND (state='discarding' OR touched<=?2) ORDER BY id LIMIT 64)
                RETURNING id,object_key,multipart)SQL",
                                     cursor,
                                     epoch_milliseconds() - chat::upload_retention_seconds * 1000);
                if (!q)
                {
                    return std::unexpected(q.error());
                }
                std::vector<stale_upload> rows;
                for (;;)
                {
                    auto row = q->step();
                    if (!row)
                    {
                        return std::unexpected(row.error());
                    }
                    if (!*row)
                    {
                        return rows;
                    }
                    rows.emplace_back(q->column_int64(0), std::string(q->column_text(1)), std::string(q->column_text(2)));
                }
            });
        if (!stale)
        {
            co_return -1;
        }
        if (stale->empty())
        {
            break;
        }
        for (auto const& t : *stale)
        {
            cursor = std::max(cursor, t.id);
            if (!(co_await discard_upload(db, objects, t.id, t.key, t.multipart)))
            {
                removal_failed = true;
            }
        }
    }
    std::string after;

    for (;;)
    {
        auto claim_batch = [&](sqlite_database& connection) { return claim_attachment_cleanup_batch(connection, cutoff, after); };
        auto batch = co_await db.run(claim_batch);
        if (!batch)
        {
            co_return -1;
        }
        if (batch->empty())
        {
            co_return removal_failed ? -1 : removed;
        }
        after = *std::ranges::max_element(*batch);

        for (auto const& key : *batch)
        {
            if (auto result = co_await objects.remove(key); !result)
            {
                removal_failed = true;
                continue;
            }

            auto acknowledge = [&key](sqlite_database& connection) { return acknowledge_attachment_cleanup(connection, key); };
            if (auto result = co_await db.run(acknowledge); !result)
            {
                co_return -1;
            }
            ++removed;
        }
    }
}
