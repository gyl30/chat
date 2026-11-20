#include "upload_store.hpp"
#include "timestamp.hpp"
#include "conversation_store.hpp"
#include "attachment_cleanup.hpp"
#include <chat/attachment.hpp>

upload_lookup read_upload(sqlite_database& db, std::int64_t user, std::string const& device, std::int64_t client_id)
{
    auto q = db.prepare(
        "SELECT id,conversation,filename,media_type,size,checksum,object_key,multipart,part_size,state FROM attachment_uploads WHERE user_id=? AND "
        "device=? AND client_id=? AND touched>? AND state<>'discarding'",
        user,
        device,
        client_id,
        epoch_milliseconds() - chat::upload_retention_seconds * 1000);
    if (!q)
    {
        return std::unexpected(q.error());
    }
    auto row = q->step();
    if (!row)
    {
        return std::unexpected(row.error());
    }
    if (!*row)
    {
        return std::nullopt;
    }
    multipart_upload t;
    t.id = q->column_int64(0);
    t.conversation = q->column_int64(1);
    t.filename = q->column_text(2);
    t.media_type = q->column_text(3);
    t.size = q->column_int64(4);
    t.checksum = q->column_text(5);
    t.key = q->column_text(6);
    t.multipart = q->column_text(7);
    t.part_size = q->column_int64(8);
    t.state = q->column_text(9);
    t.client_id = client_id;
    auto parts = db.prepare("SELECT number,offset,length,etag FROM attachment_parts WHERE upload=? ORDER BY number", t.id);
    if (!parts)
    {
        return std::unexpected(parts.error());
    }
    while ((row = parts->step()) && *row)
    {
        t.parts.emplace_back(
            static_cast<int>(parts->column_int64(0)), parts->column_int64(1), parts->column_int64(2), std::string(parts->column_text(3)));
    }
    if (!row)
    {
        return std::unexpected(row.error());
    }
    return t;
}

upload_lookup begin_upload(sqlite_database& db, std::int64_t user, std::string const& device, upload_description const& input)
{
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(tx.error());
    }
    auto allowed = can_send_to_conversation(db, input.conversation, user);
    if (!allowed)
    {
        return std::unexpected(allowed.error());
    }
    if (!*allowed)
    {
        return std::nullopt;
    }
    auto now = epoch_milliseconds();
    if (auto e = db.exec(R"SQL(
            INSERT INTO attachment_uploads(user_id,device,client_id,conversation,filename,media_type,size,checksum,part_size,state,created_at,touched)
            VALUES(?,?,?,?,?,?,?,?,?,'active',?,?) ON CONFLICT(user_id,client_id) DO NOTHING
        )SQL",
                         user,
                         device,
                         input.client_id,
                         input.conversation,
                         input.filename,
                         input.media_type,
                         input.size,
                         input.checksum,
                         chat::attachment_part_size,
                         now,
                         now);
        e)
    {
        return std::unexpected(e);
    }
    bool const created = db.changes() == 1;
    if (created)
    {
        auto id = db.last_insert_rowid();
        if (auto e = db.exec(R"SQL(WITH RECURSIVE parts(n,off) AS (VALUES(1,0) UNION ALL SELECT n+1,off+?2 FROM parts WHERE off+?2<?3)
          INSERT INTO attachment_parts(upload,number,offset,length) SELECT ?1,n,off,min(?2,?3-off) FROM parts)SQL",
                             id,
                             chat::attachment_part_size,
                             input.size);
            e)
        {
            return std::unexpected(e);
        }
    }
    auto t = read_upload(db, user, device, input.client_id);
    if (!t || !*t)
    {
        return t;
    }
    if (created)
    {
        if (auto reserved = reserve_attachment_object(db, (*t)->key, now); !reserved)
        {
            return std::unexpected(reserved.error());
        }
    }
    if ((*t)->size != input.size || (*t)->checksum != input.checksum || (*t)->conversation != input.conversation ||
        (*t)->filename != input.filename || (*t)->media_type != input.media_type)
    {
        return std::nullopt;
    }
    if (auto e = db.exec("UPDATE attachment_uploads SET state='active',touched=? WHERE id=?", now, (*t)->id); e)
    {
        return std::unexpected(e);
    }
    if (auto e = tx->commit(); e)
    {
        return std::unexpected(e);
    }
    (*t)->state = "active";
    return t;
}

std::expected<bool, sqlite_error> register_uploaded_file(sqlite_database& db, std::int64_t upload)
{
    auto now = epoch_milliseconds();
    if (auto e = db.exec(R"SQL(INSERT INTO files(object_key,uploader_id,device,upload_client_id,filename,media_type,size,checksum,created_at)
        SELECT u.object_key,u.user_id,u.device,u.client_id,u.filename,u.media_type,u.size,u.checksum,?2 FROM attachment_uploads u
        JOIN attachment_cleanup c ON c.object_key=u.object_key AND c.reserved_at>0 WHERE u.id=?1 AND u.state='active')SQL",
                         upload,
                         now);
        e)
    {
        return std::unexpected(e);
    }
    if (!db.changes())
    {
        return false;
    }
    // Start the unpublished object's protection period at completion, even after a long upload.
    if (auto e = db.exec(
            "UPDATE attachment_cleanup SET reserved_at=? WHERE object_key=(SELECT object_key FROM attachment_uploads WHERE id=?)", now, upload);
        e)
    {
        return std::unexpected(e);
    }
    if (auto e = db.exec("DELETE FROM attachment_uploads WHERE id=?", upload); e)
    {
        return std::unexpected(e);
    }
    return true;
}

std::expected<std::string, sqlite_error> completed_upload_key(sqlite_database& db,
                                                              std::int64_t user,
                                                              std::string const& device,
                                                              std::int64_t client_id)
{
    auto q = db.prepare(R"SQL(SELECT object_key FROM files WHERE uploader_id=? AND device=? AND upload_client_id=?
           AND NOT EXISTS(SELECT 1 FROM attachment_cleanup WHERE object_key=files.object_key AND reserved_at=0))SQL",
                        user,
                        device,
                        client_id);
    if (!q)
    {
        return std::unexpected(q.error());
    }
    auto row = q->step();
    if (!row)
    {
        return std::unexpected(row.error());
    }
    return *row ? std::string(q->column_text(0)) : std::string{};
}
