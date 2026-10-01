#include <string>
#include <utility>

#include <chat/typing.hpp>
#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_set_typing(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    struct [[= simdjson::deny_unknown_fields]] typing_params
    {
        std::int64_t conversation = 0;
        bool typing = false;
    };
    typing_params params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end() || params.conversation <= 0)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    {
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto result = co_await lease.connection().execute_row(
            "SELECT user_id::text FROM conversation_members WHERE conversation_id=$2::bigint AND user_id=$1::bigint",
            {std::to_string(*user_id_), std::to_string(params.conversation)});
        auto& [ec, member] = result;
        if (ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!member)
        {
            co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
        }
    }
    struct notification
    {
        std::string jsonrpc = "2.0";
        std::string method = "typing";
        chat::typing_event params;
    };
    notification value{"2.0", "typing", {params.conversation, *user_id_, username_, params.typing}};
    std::string json;
    auto error = simdjson::builder::to_json_string(value).get(json);
    if (error)
    {
        co_return error;
    }
    auto const realtime = co_await publish_conversation(params.conversation, std::move(json));
    co_return serialize_json_rpc_success(realtime ? "{\"realtime\":true}" : "{\"realtime\":false}",
                                        std::move(request.id), response);
}
