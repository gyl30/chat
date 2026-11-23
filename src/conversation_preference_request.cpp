#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <simdjson.h>
#include <chat/conversation.hpp>

#include "chat_session.hpp"
#include "database.hpp"
#include "timestamp.hpp"

namespace
{
struct[[= simdjson::deny_unknown_fields]] view_params
{
    std::int64_t conversation = 0;
    std::optional<std::int64_t> through;
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
        if (hide == params.through.has_value() || params.through.value_or(0) < 0)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        auto floor = co_await run_db(
            [=, user = *user_id_](sqlite_database& db) -> std::expected<std::int64_t, sqlite_error>
            {
                auto stmt = hide ? db.prepare(R"SQL(UPDATE conversation_members SET hidden_at=(SELECT activity FROM conversations WHERE id=?1)
                    WHERE conversation_id=?1 AND user_id=?2 RETURNING cleared_message_id)SQL",
                                              params.conversation,
                                              user)
                                 : db.prepare(R"SQL(UPDATE conversation_members SET cleared_message_id=max(cleared_message_id,?3),
                        unread_mark_message_id=CASE WHEN unread_mark_message_id>?3 THEN unread_mark_message_id END
                    WHERE conversation_id=?1 AND user_id=?2 AND (?3=0 OR EXISTS(SELECT 1 FROM messages WHERE id=?3 AND conversation_id=?1))
                    RETURNING cleared_message_id)SQL",
                                              params.conversation,
                                              user,
                                              *params.through);
                if (!stmt)
                {
                    return std::unexpected(stmt.error());
                }
                auto row = stmt->step();
                if (!row)
                {
                    return std::unexpected(row.error());
                }
                if (!*row)
                {
                    return -1;
                }
                auto actual = stmt->column_int64(0);
                if (auto done = stmt->step(); !done)
                {
                    return std::unexpected(done.error());
                }
                return actual;
            });
        if (!floor || *floor < 0)
        {
            co_return floor ? serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response)
                            : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        publish_personal_view({params.conversation, hide ? "hide" : "clear", hide ? std::nullopt : std::optional(*floor)});
        co_return serialize_json_rpc_success(
            hide ? R"({"changed":true})" : R"({"through":)" + std::to_string(*floor) + "}", std::move(request.id), response);
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
    auto updated = co_await run_db(
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
    if (notify)
    {
        publish_personal_view({.conversation = conversation, .action = "notifications", .level = value, .until = until});
    }
    else
    {
        publish_personal_view({conversation, "preferences"});
    }
    co_return serialize_json_rpc_success(
        pin ? (value ? R"({"pinned":true})" : R"({"pinned":false})") : R"({"changed":true})", std::move(request.id), response);
}

void chat_session::publish_personal_view(chat::conversation_view_event const& view)
{
    std::string payload;
    if (simdjson::builder::to_json_string(view).get(payload))
    {
        return;
    }
    auto json = R"({"jsonrpc":"2.0","method":"conversation_view","params":)" + payload + "}";
    for (auto* session : users_.sessions(*user_id_))
    {
        session->enqueue_message(json);
    }
}
