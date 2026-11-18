#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "conversation_store.hpp"

namespace
{

struct [[= simdjson::deny_unknown_fields]] get_unread_count_params
{
    std::int64_t conversation = 0;
};

simdjson::error_code parse_get_unread_count_params(json_rpc_params& params, get_unread_count_params& value)
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

    if (value.conversation <= 0)
    {
        return simdjson::INCORRECT_TYPE;
    }

    return simdjson::SUCCESS;
}

simdjson::error_code serialize_get_unread_count_result(std::string_view count, json_rpc_id id, std::string& response)
{
    std::string result = R"({"count":)";
    result.append(count);
    result.push_back('}');
    return serialize_json_rpc_success(result, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_unread_count(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    get_unread_count_params params{};
    auto params_error = parse_get_unread_count_params(request.params, params);
    if (params_error)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    auto count = co_await database_.run([=, user=*user_id_](sqlite_database& db) {
        return read_unread_count(db, params.conversation, user);
    });
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!count || !*count)
    {
        co_return count ? serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response)
                        : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    co_return serialize_get_unread_count_result(std::to_string(**count), std::move(request.id), response);
}
