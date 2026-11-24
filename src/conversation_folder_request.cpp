#include "chat_session.hpp"
#include "database.hpp"

#include <chat/conversation.hpp>
#include <chat/text.hpp>
#include <sqlite3.h>

namespace
{
struct[[= simdjson::deny_unknown_fields]] folder_params
{
    std::optional<std::int64_t> folder;
    std::optional<std::int64_t> conversation;
    std::optional<std::string> name;
    std::optional<bool> up;
};

// Each action is one atomic statement. The delete trigger releases all memberships.
std::string_view folder_sql(std::string_view method, folder_params const& p)
{
    if (p.name && (p.name->find('\0') != std::string::npos || chat::text_is_blank(*p.name)))
    {
        return {};
    }
    if (method == "create_conversation_folder" && p.name && !p.folder && !p.conversation && !p.up)
    {
        return "INSERT INTO conversation_folders(owner_id,name,position) SELECT ?1,?3,coalesce(max(position),0)+1 FROM conversation_folders WHERE "
               "owner_id=?1";
    }
    if (!p.folder || *p.folder < 0)
    {
        return {};
    }
    if (method == "move_conversation_to_folder" && p.conversation && *p.conversation > 0 && !p.name && !p.up)
    {
        return "UPDATE conversation_members SET folder_id=nullif(?2,0) WHERE user_id=?1 AND conversation_id=?3";
    }
    if (*p.folder == 0 || p.conversation)
    {
        return {};
    }
    if (method == "rename_conversation_folder" && p.name && !p.up)
    {
        return "UPDATE conversation_folders SET name=?3 WHERE owner_id=?1 AND id=?2";
    }
    if (method == "delete_conversation_folder" && !p.name && !p.up)
    {
        return "DELETE FROM conversation_folders WHERE owner_id=?1 AND id=?2";
    }
    if (method == "reorder_conversation_folder" && p.up && !p.name)
    {
        return R"SQL(WITH ordered AS MATERIALIZED (
            SELECT id,position,CASE WHEN ?3 THEN lag(position) OVER w ELSE lead(position) OVER w END AS neighbor
            FROM conversation_folders WHERE owner_id=?1 WINDOW w AS (ORDER BY position,id)
        ), pair AS MATERIALIZED (SELECT position,neighbor FROM ordered WHERE id=?2 AND neighbor IS NOT NULL)
        UPDATE conversation_folders SET position=CASE WHEN id=?2 THEN (SELECT neighbor FROM pair) ELSE (SELECT position FROM pair) END
        WHERE owner_id=?1 AND position IN (SELECT position FROM pair UNION ALL SELECT neighbor FROM pair))SQL";
    }
    return {};
}
}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_conversation_folder(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    folder_params p;
    if (!request.params.present || parser.iterate(request.params.json).get(doc) || doc.get(p) || !doc.at_end())
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto sql = folder_sql(request.method, p);
    if (sql.empty())
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto changed = co_await run_db(
        [&, user = *user_id_](sqlite_database& db) -> std::expected<bool, sqlite_error>
        {
            sqlite_error error;
            if (p.name)
            {
                error = db.exec(sql, user, p.folder, *p.name);
            }
            else if (p.conversation)
            {
                error = db.exec(sql, user, p.folder, *p.conversation);
            }
            else if (p.up)
            {
                error = db.exec(sql, user, p.folder, static_cast<int>(*p.up));
            }
            else
            {
                error = db.exec(sql, user, p.folder);
            }
            if (error)
            {
                if ((error.code & 255) == SQLITE_CONSTRAINT)
                {
                    return false;
                }
                return std::unexpected(error);
            }
            return db.changes() > 0;
        });
    if (!changed)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!*changed)
    {
        co_return serialize_json_rpc_error(-32006, "Folder or conversation unavailable", std::move(request.id), response);
    }
    publish_personal_view({p.conversation.value_or(0), "folders"});
    co_return serialize_json_rpc_success(R"({"changed":true})", std::move(request.id), response);
}
