#include "bundle_store.hpp"
#include "chat_session.hpp"
#include <chat/message.hpp>

std::expected<bool, sqlite_error> store_bundle(sqlite_database& db, std::int64_t parent, std::int64_t sender, std::span<std::int64_t const> sources)
{
    int position = 0;
    for (auto source : sources)
    {
        auto err = db.exec(R"SQL(
            INSERT INTO bundle_items(parent,position,author,created,body,markdown,object_key)
            SELECT ?1,?2,coalesce(nullif(u.nickname,''),u.username),m.created_at,m.body,m.markdown,ref.object_key
            FROM readable_messages m JOIN conversation_members own ON own.conversation_id=m.conversation_id
            JOIN users u ON u.id=coalesce(m.original_sender,m.sender_id)
            LEFT JOIN message_attachments ref ON ref.message_id=m.id
            WHERE m.id=?3 AND own.user_id=?4 AND m.id>own.visible_after AND NOT m.deleted AND m.content='message'
              AND m.conversation_id=(SELECT conversation_id FROM messages WHERE id=?5)
        )SQL",
                           parent,
                           ++position,
                           source,
                           sender,
                           sources.front());
        if (err)
        {
            return std::unexpected(err);
        }
        if (!db.changes())
        {
            return false;
        }
    }
    auto e = db.exec(R"SQL(
        UPDATE messages SET body='聊天记录'||char(10)||(SELECT group_concat(line,char(10)) FROM
            (SELECT author||'：'||CASE WHEN object_key IS NULL THEN '' ELSE '[文件] ' END||substr(body,1,64) AS line
             FROM bundle_items WHERE parent=?1 ORDER BY position LIMIT 3))||char(10)||'共 '||?2||' 条聊天记录' WHERE id=?1
    )SQL",
                     parent,
                     position);
    if (e)
    {
        return std::unexpected(e);
    }
    return true;
}

namespace
{
struct[[= simdjson::deny_unknown_fields]] bundle_params
{
    std::int64_t conversation = 0, message = 0;
    std::optional<std::int64_t> after;
};

std::expected<std::optional<chat::forwarded_page>, sqlite_error> read_bundle(sqlite_database& db, std::int64_t user, bundle_params const& p)
{
    auto snapshot = sqlite_transaction::begin_read(db);
    if (!snapshot)
    {
        return std::unexpected(snapshot.error());
    }
    auto visible = db.prepare(R"SQL(SELECT 1 FROM messages m JOIN conversation_members own ON own.conversation_id=m.conversation_id
        WHERE own.user_id=? AND m.conversation_id=? AND m.id=? AND m.id>own.visible_after AND NOT m.deleted AND m.content='bundle')SQL",
                              user,
                              p.conversation,
                              p.message);
    if (!visible)
    {
        return std::unexpected(visible.error());
    }
    auto found = visible->step();
    if (!found)
    {
        return std::unexpected(found.error());
    }
    if (!*found)
    {
        return std::nullopt;
    }
    auto q = db.prepare(R"SQL(SELECT b.position,b.created,b.author,b.body,b.markdown,coalesce(f.filename,''),coalesce(f.media_type,'')
       FROM bundle_items b LEFT JOIN files f ON f.object_key=b.object_key WHERE b.parent=? AND b.position>? ORDER BY b.position LIMIT 11)SQL",
                        p.message,
                        p.after.value_or(0));
    if (!q)
    {
        return std::unexpected(q.error());
    }
    chat::forwarded_page page;
    std::size_t bytes = 0;
    for (;;)
    {
        auto row = q->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            return page;
        }
        chat::forwarded_item item{q->column_int64(0),
                                  q->column_int64(1),
                                  std::string(q->column_text(2)),
                                  std::string(q->column_text(3)),
                                  q->column_int64(4) != 0,
                                  std::string(q->column_text(5)),
                                  std::string(q->column_text(6))};
        std::string encoded;
        if (simdjson::builder::to_json_string(item).get(encoded))
        {
            return std::unexpected(sqlite_error{1, "cannot serialize bundle item"});
        }
        // A source already fits the 64 KiB live-message envelope. Keep complete items and room for the RPC envelope.
        if (!page.items.empty() && (page.items.size() == 10 || bytes + encoded.size() > 256 * 1024))
        {
            page.next = page.items.back().position;
            return page;
        }
        bytes += encoded.size() + 1;
        page.items.push_back(std::move(item));
    }
}
}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_bundle(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    bundle_params p;
    if (!request.params.read(p) || p.conversation <= 0 || p.message <= 0 || p.after.value_or(0) < 0)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto page = co_await run_db([&, user = *user_id_](sqlite_database& db) { return read_bundle(db, user, p); });
    if (!page)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!*page)
    {
        co_return serialize_json_rpc_error(-32006, "Message unavailable", std::move(request.id), response);
    }
    co_return serialize_json_rpc_value(**page, std::move(request.id), response);
}
