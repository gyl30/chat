#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "message_history.hpp"

namespace
{

struct[[= simdjson::deny_unknown_fields]] mark_read_params
{
    std::int64_t conversation = 0;
    std::int64_t message = 0;
};

simdjson::error_code parse_mark_read_params(json_rpc_params& params, mark_read_params& value)
{
    if (!params.present)
    {
        return simdjson::NO_SUCH_FIELD;
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

    if (value.conversation <= 0 || value.message <= 0)
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_mark_read_result(std::string_view message, json_rpc_id id, std::string& response)
{
    std::string result = R"({"message":)";
    result.append(message);
    result.push_back('}');
    return serialize_json_rpc_success(result, std::move(id), response);
}

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
    auto params_error = parse_mark_read_params(request.params, params);
    if (params_error)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    auto position = co_await database_.run([=, user = *user_id_](sqlite_database& db)
                                           { return update_read_position(db, user, params.conversation, params.message); });
    if (!position || !*position)
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        co_return position ? serialize_json_rpc_invalid_params(std::move(request.id), response)
                           : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto const read_message = std::to_string((*position)->message);

    if ((*position)->advanced)
    {
        std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":\"read\",\"params\":{\"conversation\":";
        notification.append(std::to_string(params.conversation));
        notification.append(",\"user\":" + std::to_string(*user_id_) + ",\"message\":" + read_message + "}}");
        co_await publish_conversation(params.conversation, std::move(notification), false, (*position)->message);
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    co_return serialize_mark_read_result(read_message, std::move(request.id), response);
}
