#include "chat_session.hpp"
#include <chat/error.hpp>
#include "conversation_store.hpp"
#include "database.hpp"
#include "s3_objects.hpp"
#include "timestamp.hpp"
#include <chat/attachment.hpp>

namespace
{
struct[[= simdjson::deny_unknown_fields]] file_params
{
    std::int64_t conversation_id = 0;
    std::string object_key;
};
struct[[= simdjson::deny_unknown_fields]] list_params
{
    std::int64_t conversation_id = 0;
    std::optional<std::int64_t> limit;
    std::optional<std::int64_t> cursor_created_at;
    std::optional<std::string> cursor_object_key;
    std::optional<std::string> search, category;
};

std::expected<std::optional<chat::conversation_file_page>, sqlite_error> list_files(sqlite_database& db, std::int64_t user, list_params const& p)
{
    auto snapshot = sqlite_transaction::begin_read(db);
    if (!snapshot)
    {
        return std::unexpected(snapshot.error());
    }
    auto role = read_group_role(db, p.conversation_id, user);
    if (!role)
    {
        return std::unexpected(role.error());
    }
    if (!*role)
    {
        return std::nullopt;
    }
    auto q = db.prepare(R"SQL(SELECT f.object_key,f.filename,f.media_type,f.size,f.uploader_id,u.username,c.created_at,
        CASE WHEN f.media_type LIKE 'image/%' THEN 'image'
             WHEN lower(f.filename) GLOB '*.zip' OR lower(f.filename) GLOB '*.7z' OR lower(f.filename) GLOB '*.rar'
               OR lower(f.filename) GLOB '*.tar' OR lower(f.filename) GLOB '*.gz' OR lower(f.filename) GLOB '*.xz'
               OR lower(f.filename) GLOB '*.bz2' THEN 'archive'
             WHEN lower(f.filename) GLOB '*.pdf' OR lower(f.filename) GLOB '*.txt' OR lower(f.filename) GLOB '*.md'
               OR lower(f.filename) GLOB '*.doc' OR lower(f.filename) GLOB '*.docx' OR lower(f.filename) GLOB '*.odt'
               OR lower(f.filename) GLOB '*.xls' OR lower(f.filename) GLOB '*.xlsx' OR lower(f.filename) GLOB '*.csv'
               OR lower(f.filename) GLOB '*.ppt' OR lower(f.filename) GLOB '*.pptx' THEN 'document'
             ELSE 'other' END AS category
        FROM conversation_files c JOIN files f ON f.object_key=c.object_key JOIN users u ON u.id=f.uploader_id
        WHERE c.conversation_id=?1 AND (?2 IS NULL OR (c.created_at,c.object_key)<(?2,?3))
          AND instr(lower(f.filename),lower(?4))>0 AND (?5='' OR category=?5)
        ORDER BY c.created_at DESC,c.object_key DESC LIMIT ?6)SQL",
                        p.conversation_id,
                        p.cursor_created_at,
                        p.cursor_object_key.value_or(""),
                        p.search.value_or(""),
                        p.category.value_or(""),
                        p.limit.value_or(50) + 1);
    if (!q)
    {
        return std::unexpected(q.error());
    }
    chat::conversation_file_page page;
    for (;;)
    {
        auto row = q->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            break;
        }
        if (std::ssize(page.files) == p.limit.value_or(50))
        {
            page.has_more = true;
            break;
        }
        page.files.push_back({std::string(q->column_text(0)),
                              std::string(q->column_text(1)),
                              std::string(q->column_text(2)),
                              q->column_int64(3),
                              q->column_int64(4),
                              std::string(q->column_text(5)),
                              q->column_int64(6),
                              std::string(q->column_text(7))});
    }
    return page;
}
enum class file_change
{
    unavailable,
    unchanged,
    changed,
    muted
};
std::expected<file_change, sqlite_error> change_file(sqlite_database& db, std::int64_t user, file_params const& p, bool add)
{
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(tx.error());
    }
    auto role = read_group_role(db, p.conversation_id, user);
    if (!role)
    {
        return std::unexpected(role.error());
    }
    if (!*role)
    {
        return file_change::unavailable;
    }
    if (add)
    {
        auto allowed = can_send_to_conversation(db, p.conversation_id, user);
        if (!allowed)
        {
            return std::unexpected(allowed.error());
        }
        if (*allowed != send_access::allowed)
        {
            return *allowed == send_access::muted ? file_change::muted : file_change::unavailable;
        }
        auto file = db.prepare(R"SQL(SELECT 1 FROM files WHERE object_key=? AND uploader_id=?
           AND NOT EXISTS(SELECT 1 FROM attachment_cleanup WHERE object_key=files.object_key AND reserved_at=0))SQL",
                               p.object_key,
                               user);
        if (!file)
        {
            return std::unexpected(file.error());
        }
        auto row = file->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            return file_change::unavailable;
        }
        if (auto e = db.exec("INSERT INTO conversation_files(conversation_id,object_key,created_at) VALUES(?,?,?) ON CONFLICT DO NOTHING",
                             p.conversation_id,
                             p.object_key,
                             epoch_milliseconds());
            e)
        {
            return std::unexpected(e);
        }
    }
    else
    {
        if (auto e = db.exec(R"SQL(DELETE FROM conversation_files WHERE conversation_id=? AND object_key=? AND
            (? OR EXISTS(SELECT 1 FROM files WHERE object_key=conversation_files.object_key AND uploader_id=?)))SQL",
                             p.conversation_id,
                             p.object_key,
                             **role != chat::member_role::member,
                             user);
            e)
        {
            return std::unexpected(e);
        }
        if (!db.changes())
        {
            return file_change::unavailable;
        }
    }
    bool const changed = db.changes() != 0;
    if (auto e = tx->commit(); e)
    {
        return std::unexpected(e);
    }
    return changed ? file_change::changed : file_change::unchanged;
}
}    // namespace
boost::capy::task<simdjson::error_code> chat_session::handle_conversation_file(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    auto fail = [&](int code, std::string_view msg) { return serialize_json_rpc_error(code, msg, std::move(request.id), response); };
    auto invalid = [&] { return serialize_json_rpc_invalid_params(std::move(request.id), response); };
    if (!user_id_)
    {
        co_return fail(-32001, "Authentication required");
    }
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (!request.params.present || parser.iterate(request.params.json).get(doc))
    {
        co_return invalid();
    }
    auto const user = *user_id_;
    if (request.method == "list_conversation_files")
    {
        list_params p;
        if (doc.get(p) || !doc.at_end() || p.conversation_id <= 0 || p.limit.value_or(50) < 1 || p.limit.value_or(50) > 100 ||
            p.cursor_created_at.has_value() != p.cursor_object_key.has_value() ||
            (p.cursor_created_at && (*p.cursor_created_at <= 0 || p.cursor_object_key->empty())) ||
            (p.category && !p.category->empty() && p.category != "document" && p.category != "archive" && p.category != "image" &&
             p.category != "other"))
        {
            co_return invalid();
        }
        auto page = co_await database_.run([&](sqlite_database& db) { return list_files(db, user, p); });
        if (!page)
        {
            co_return fail(-32000, "Server error");
        }
        if (!*page)
        {
            co_return fail(-32006, "Conversation unavailable");
        }
        co_return serialize_json_rpc_value(**page, std::move(request.id), response);
    }
    file_params p;
    if (doc.get(p) || !doc.at_end() || p.conversation_id <= 0 || p.object_key.empty())
    {
        co_return invalid();
    }
    auto result = co_await database_.run([&](sqlite_database& db) { return change_file(db, user, p, request.method == "add_conversation_file"); });
    if (!result)
    {
        co_return fail(-32000, "Server error");
    }
    if (*result == file_change::muted)
    {
        co_return fail(chat::group_speech_muted, "Group speech is muted");
    }
    if (*result == file_change::unavailable)
    {
        co_return fail(-32007, "Attachment unavailable");
    }
    co_return serialize_json_rpc_success(
        *result == file_change::changed ? "{\"changed\":true}" : "{\"changed\":false}", std::move(request.id), response);
}
