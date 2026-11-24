#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "conversation_store.hpp"

namespace
{

struct[[= simdjson::deny_unknown_fields]] conversation_cursor
{
    std::int64_t activity = 0;
    std::int64_t id = 0;
    std::optional<bool> pinned;
};

struct[[= simdjson::deny_unknown_fields]] get_conversations_params
{
    std::optional<conversation_cursor> before;
};

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_conversations(json_rpc_request& request, std::string& response)
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
    if ((request.params.present && !request.params.read(params)) ||
        (params.before && (params.before->activity <= 0 || params.before->id <= 0 || !params.before->pinned)))
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

    std::optional<chat::conversation_cursor> cursor;
    if (params.before)
    {
        cursor = chat::conversation_cursor{params.before->activity, params.before->id, *params.before->pinned};
    }
    auto result = co_await run_db([=, user = *user_id_](sqlite_database& db) { return read_conversation_list(db, user, cursor); });
    if (!result)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    co_return serialize_json_rpc_success(*result, std::move(request.id), response);
}
