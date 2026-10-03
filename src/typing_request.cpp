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
    auto const realtime = co_await publish_conversation(params.conversation, std::move(json), true);
    if (!realtime)
    {
        auto const denied = realtime.error() == std::errc::permission_denied;
        co_return serialize_json_rpc_error(denied ? -32006 : -32000,
            denied ? "Communication not allowed" : "Server error", std::move(request.id), response);
    }
    co_return serialize_json_rpc_success(*realtime ? "{\"realtime\":true}" : "{\"realtime\":false}",
                                        std::move(request.id), response);
}
