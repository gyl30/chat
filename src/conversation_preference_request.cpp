#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "timestamp.hpp"

namespace
{
struct[[= simdjson::deny_unknown_fields]] view_params
{
    std::int64_t conversation = 0;
};

struct[[= simdjson::deny_unknown_fields]] notification_params
{
    std::int64_t conversation = 0;
    std::int64_t level = -1;
    std::optional<std::int64_t> until;
};
struct[[= simdjson::deny_unknown_fields]] mark_params
{
    std::int64_t conversation = 0;
    std::optional<bool> marked;
};
struct[[= simdjson::deny_unknown_fields]] pin_params
{
    std::int64_t conversation = 0;
    std::optional<bool> pinned;
};
}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_conversation_preference(json_rpc_request& request, std::string& response)
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
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    if (request.method == "clear_history" || request.method == "hide_conversation")
    {
        view_params params;
        if (document.get(params) || !document.at_end() || params.conversation <= 0)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        bool const hide = request.method == "hide_conversation";
        auto changed = co_await database_.run(
            [=, user = *user_id_](sqlite_database& db) -> std::expected<bool, sqlite_error>
            {
                // One SQLite statement reads the committed conversation boundary and updates only this membership.
                auto sql = hide ? R"SQL(UPDATE conversation_members SET hidden_at=(SELECT activity FROM conversations WHERE id=?1)
                WHERE conversation_id=?1 AND user_id=?2)SQL"
                                :
                                R"SQL(WITH boundary(id) AS (SELECT coalesce(max(id),0) FROM messages WHERE conversation_id=?1)
                UPDATE conversation_members SET unread_mark_message_id=NULL,cleared_message_id=max(cleared_message_id,(SELECT id FROM boundary)),
                    last_read_message_id=max(last_read_message_id,(SELECT id FROM boundary))
                WHERE conversation_id=?1 AND user_id=?2)SQL";
                auto stmt = db.prepare(sql, params.conversation, user);
                if (!stmt)
                {
                    return std::unexpected(stmt.error());
                }
                if (auto done = stmt->step(); !done)
                {
                    return std::unexpected(done.error());
                }
                return db.changes() == 1;
            });
        if (!changed || !*changed)
        {
            co_return changed ? serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response)
                              : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        publish_personal_view(params.conversation, hide ? "hide" : "clear");
        co_return serialize_json_rpc_success(R"({"changed":true})", std::move(request.id), response);
    }
    std::int64_t conversation = 0, value = 0, until = 0;
    std::string_view sql;
    bool const pin = request.method == "set_conversation_pinned";
    bool const notify = request.method == "set_notification_level";
    if (pin)
    {
        pin_params p;
        if (document.get(p) || !p.pinned)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        conversation = p.conversation;
        value = *p.pinned;
        sql = "UPDATE conversation_members SET pinned=?3 WHERE conversation_id=?1 AND user_id=?2";
    }
    else if (notify)
    {
        notification_params p;
        if (document.get(p) || p.level < 0 || p.level > 2 || (p.until.value_or(0) != 0 && (p.level != 2 || *p.until <= epoch_milliseconds())))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        conversation = p.conversation;
        value = p.level;
        until = p.until.value_or(0);
        sql = "UPDATE conversation_members SET notification_level=?3,mute_until=?4 WHERE conversation_id=?1 AND user_id=?2";
    }
    else
    {
        mark_params p;
        if (document.get(p) || !p.marked)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        conversation = p.conversation;
        value = *p.marked;
        sql = R"SQL(UPDATE conversation_members SET unread_mark_message_id=CASE WHEN ?3 THEN
            (SELECT coalesce(max(id),0) FROM messages WHERE conversation_id=?1) END WHERE conversation_id=?1 AND user_id=?2)SQL";
    }
    if (!document.at_end() || conversation <= 0)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto updated = co_await database_.run(
        [=, user = *user_id_](sqlite_database& db) -> std::expected<bool, sqlite_error>
        {
            auto err = notify ? db.exec(sql, conversation, user, value, until) : db.exec(sql, conversation, user, value);
            if (err)
            {
                return std::unexpected(err);
            }
            return db.changes() == 1;
        });
    if (!updated || !*updated)
    {
        co_return updated ? serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response)
                          : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    publish_personal_view(conversation, "preferences");
    co_return serialize_json_rpc_success(
        pin ? (value ? R"({"pinned":true})" : R"({"pinned":false})") : R"({"changed":true})", std::move(request.id), response);
}

void chat_session::publish_personal_view(std::int64_t conversation, std::string_view action)
{
    auto json = R"({"jsonrpc":"2.0","method":"conversation_view","params":{"conversation":)" + std::to_string(conversation) + R"(,"action":")" +
                std::string(action) + "\"}}";
    for (auto* session : users_.sessions(*user_id_))
    {
        session->enqueue_message(json);
    }
}
