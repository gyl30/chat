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
    co_return serialize_json_rpc_success(
        std::string(pin ? "{\"pinned\":" : "{\"muted\":") + (*value ? "true}" : "false}"), std::move(request.id), response);
}
