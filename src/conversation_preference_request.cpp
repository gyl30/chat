#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "conversation_store.hpp"

namespace
{
struct[[= simdjson::deny_unknown_fields]] view_params
{
    std::int64_t conversation = 0;
};

struct[[= simdjson::deny_unknown_fields]] mute_params
{
    std::int64_t conversation = 0;
    std::optional<bool> muted;
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
    auto const pin = request.method == "set_conversation_pinned";
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
                UPDATE conversation_members SET cleared_message_id=max(cleared_message_id,(SELECT id FROM boundary)),
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
    std::int64_t conversation = 0;
    std::optional<bool> value;
    if (pin)
    {
        pin_params params;
        if (document.get(params))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        conversation = params.conversation;
        value = params.pinned;
    }
    else
    {
        mute_params params;
        if (document.get(params))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        conversation = params.conversation;
        value = params.muted;
    }
    if (!document.at_end() || conversation <= 0 || !value)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto updated = co_await database_.run(
        [=, user = *user_id_](sqlite_database& db)
        { return set_conversation_preference(db, conversation, user, pin ? conversation_preference::pin : conversation_preference::mute, *value); });
    if (!updated || !*updated)
    {
        co_return updated ? serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response)
                          : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    publish_personal_view(conversation, "preferences");
    co_return serialize_json_rpc_success(
        std::string(pin ? "{\"pinned\":" : "{\"muted\":") + (*value ? "true}" : "false}"), std::move(request.id), response);
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
