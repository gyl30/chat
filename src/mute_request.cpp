#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{
struct [[= simdjson::deny_unknown_fields]] mute_params
{
    std::int64_t conversation = 0;
    std::optional<bool> muted;
};
}

boost::capy::task<simdjson::error_code> chat_session::handle_conversation_mute(json_rpc_request& request,
                                                                           std::string& response)
{
    if (!request.id.present) { co_return simdjson::SUCCESS; }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    mute_params params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end() || params.conversation <= 0 || !params.muted)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto& connection = lease.connection();
    auto begun = co_await connection.execute_row("BEGIN");
    if (std::get<0>(begun))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto locked = co_await connection.execute_row("SELECT id::text FROM conversations WHERE id=$1::bigint FOR UPDATE",
                                                 {std::to_string(params.conversation)});
    if (std::get<0>(locked))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto updated = co_await connection.execute_row(
        "UPDATE conversation_members SET muted=$3::boolean WHERE conversation_id=$1::bigint AND user_id=$2::bigint "
        "RETURNING muted::text",
        {std::to_string(params.conversation), std::to_string(*user_id_), *params.muted ? "true" : "false"});
    auto& [update_ec, row] = updated;
    if (update_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto ended = co_await connection.execute_row(row ? "COMMIT" : "ROLLBACK");
    if (std::get<0>(ended))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!row)
    {
        co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
    }
    co_return serialize_json_rpc_success("{\"muted\":" + row->front() + "}", std::move(request.id), response);
}
