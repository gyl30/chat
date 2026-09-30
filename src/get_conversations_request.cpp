#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

struct [[= simdjson::deny_unknown_fields]] get_conversations_params
{
    std::optional<std::int64_t> before;
};

simdjson::error_code parse_get_conversations_params(json_rpc_params& params, get_conversations_params& value)
{
    if (!params.present)
    {
        return simdjson::SUCCESS;
    }

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    auto error = parser.iterate(params.json).get(document);
    if (error)
    {
        return error;
    }

    error = document.get(value);
    if (error)
    {
        return error;
    }

    if (!document.at_end())
    {
        return simdjson::TRAILING_CONTENT;
    }

    if (value.before && *value.before <= 0)
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_get_conversations_result(std::string_view conversations,
                                                        json_rpc_id id,
                                                        std::string& response)
{
    std::string result = R"({"conversations":)";
    result.append(conversations);
    result.push_back('}');
    return serialize_json_rpc_success(result, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_conversations(json_rpc_request& request,
                                                                               std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    get_conversations_params params{};
    auto params_error = parse_get_conversations_params(request.params, params);
    if (params_error)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    std::string conversations_json;
    {
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }

        std::vector<std::string> parameters;
        parameters.emplace_back(std::to_string(*user_id_));
        parameters.emplace_back(std::to_string(params.before.value_or(std::numeric_limits<std::int64_t>::max())));

        auto query_result = co_await lease.connection().execute_scalar(
            "WITH latest AS ("
            "SELECT DISTINCT ON (peer) peer, id, sender_id, body, created_at FROM ("
            "SELECT id, sender_id, recipient_id, body, created_at, "
            "CASE WHEN sender_id = $1::bigint THEN recipient_id ELSE sender_id END AS peer "
            "FROM messages WHERE sender_id = $1::bigint OR recipient_id = $1::bigint"
            ") AS conversation_messages "
            "ORDER BY peer, id DESC"
            "), unread AS ("
            "SELECT messages.sender_id AS peer, count(*) AS count FROM messages "
            "LEFT JOIN message_read_positions ON "
            "message_read_positions.user_id = $1::bigint "
            "AND message_read_positions.peer_user_id = messages.sender_id "
            "WHERE messages.recipient_id = $1::bigint "
            "AND messages.id > COALESCE(message_read_positions.last_read_message_id, 0) "
            "GROUP BY messages.sender_id"
            ") "
            "SELECT COALESCE("
            "array_to_json(array_agg(row_to_json(conversation) ORDER BY page.id DESC)), "
            "'[]'::json"
            ")::text "
            "FROM ("
            "SELECT latest.peer, users.username, latest.id, latest.sender_id, latest.body, latest.created_at, "
            "COALESCE(unread.count, 0) AS unread "
            "FROM latest JOIN users ON users.id = latest.peer "
            "LEFT JOIN unread ON unread.peer = latest.peer "
            "WHERE latest.id < $2::bigint "
            "ORDER BY latest.id DESC LIMIT 50"
            ") AS page "
            "CROSS JOIN LATERAL ("
            "SELECT page.peer AS \"user\", page.username, row_to_json(last_message) AS last, page.unread "
            "FROM LATERAL ("
            "SELECT page.id, page.sender_id AS \"from\", "
            "((extract(epoch from page.created_at) * 1000)::bigint) AS timestamp, page.body AS text"
            ") AS last_message"
            ") AS conversation",
            std::move(parameters));
        auto& [query_ec, query_json] = query_result;
        if (query_ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        conversations_json = std::move(query_json);
    }

    co_return serialize_get_conversations_result(conversations_json, std::move(request.id), response);
}
