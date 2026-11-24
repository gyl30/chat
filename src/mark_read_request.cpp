#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "message_history.hpp"
#include <chat/conversation.hpp>

namespace
{

struct[[= simdjson::deny_unknown_fields]] mark_read_params
{
    std::int64_t conversation = 0;
    std::int64_t message = 0;
};

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_mark_read(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    mark_read_params params{};
    if (!request.params.read(params) || params.conversation <= 0 || params.message <= 0)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    auto position =
        co_await run_db([=, user = *user_id_](sqlite_database& db) { return update_read_position(db, user, params.conversation, params.message); });
    if (!position || !*position)
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        co_return position ? serialize_json_rpc_invalid_params(std::move(request.id), response)
                           : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }

    if ((*position)->advanced)
    {
        co_await publish_read(params.conversation, (*position)->message);
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    struct read_result
    {
        std::int64_t message;
    };
    co_return serialize_json_rpc_value(read_result{(*position)->message}, std::move(request.id), response);
}

boost::capy::task<> chat_session::publish_read(std::int64_t conversation, std::int64_t message)
{
    std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":\"read\",\"params\":{\"conversation\":";
    notification.append(std::to_string(conversation));
    notification.append(",\"user\":" + std::to_string(*user_id_) + ",\"message\":" + std::to_string(message) + "}}");
    co_await publish_conversation(conversation, std::move(notification), false, message);
}

namespace
{
struct[[= simdjson::deny_unknown_fields]] read_scope
{
    std::optional<std::int64_t> conversation;
    std::optional<std::int64_t> folder;
};
struct read_target
{
    std::int64_t conversation;
    std::int64_t tail;
    bool advanced;
};
std::expected<std::vector<read_target>, sqlite_error> mark_listed_read(sqlite_database& db, std::int64_t user, read_scope const& scope)
{
    auto tx = sqlite_transaction::begin_immediate(db);
    if (!tx)
    {
        return std::unexpected(tx.error());
    }
    auto rows = db.prepare(R"SQL(WITH targets AS MATERIALIZED (
        SELECT conversation_id,last_read_message_id,unread_mark_message_id,coalesce(
            (SELECT max(m.id) FROM messages m WHERE m.conversation_id=own.conversation_id AND m.id>own.visible_after AND m.deleted=0),0) AS tail
        FROM listed_conversation_members own WHERE user_id=?1 AND (?2 IS NULL OR conversation_id=?2)
            AND (?3 IS NULL OR coalesce(folder_id,0)=?3)
        ) SELECT conversation_id,tail,tail>last_read_message_id FROM targets
        WHERE tail>last_read_message_id OR unread_mark_message_id IS NOT NULL)SQL",
                           user,
                           scope.conversation,
                           scope.folder);
    if (!rows)
    {
        return std::unexpected(rows.error());
    }
    std::vector<read_target> targets;
    while (true)
    {
        auto row = rows->step();
        if (!row)
        {
            return std::unexpected(row.error());
        }
        if (!*row)
        {
            break;
        }
        targets.push_back({rows->column_int64(0), rows->column_int64(1), rows->column_int64(2) != 0});
    }
    for (auto const& target : targets)
    {
        if (auto err = db.exec("UPDATE conversation_members SET last_read_message_id=max(last_read_message_id,?3),unread_mark_message_id=NULL WHERE "
                               "conversation_id=?1 AND user_id=?2",
                               target.conversation,
                               user,
                               target.tail);
            err)
        {
            return std::unexpected(err);
        }
    }
    if (auto err = tx->commit(); err)
    {
        return std::unexpected(err);
    }
    return targets;
}
}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_bulk_read(json_rpc_request& request, std::string& response)
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
    read_scope scope;
    if (!request.params.present || parser.iterate(request.params.json).get(doc) || doc.get(scope) || !doc.at_end() ||
        (scope.conversation && (scope.folder || *scope.conversation <= 0)) || scope.folder.value_or(0) < 0)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto targets = co_await run_db([&, user = *user_id_](sqlite_database& db) { return mark_listed_read(db, user, scope); });
    if (!targets)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!targets->empty())
    {
        publish_personal_view({scope.conversation.value_or(0), "read"});
        for (auto const& target : *targets)
        {
            if (target.advanced)
            {
                co_await publish_read(target.conversation, target.tail);
            }
        }
    }
    co_return serialize_json_rpc_success(targets->empty() ? R"({"changed":false})" : R"({"changed":true})", std::move(request.id), response);
}
