#include <string>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"

namespace
{

struct [[= simdjson::deny_unknown_fields]] echo_params
{
    std::string text;
};

struct echo_result
{
    std::string text;
};

simdjson::error_code parse_echo_params(json_rpc_params& params, echo_params& value)
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

    return simdjson::SUCCESS;
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_echo(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    echo_params params{};
    auto params_error = parse_echo_params(request.params, params);
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

    echo_result result{};
    result.text = std::move(params.text);

    std::string result_json;
    auto rpc_error = simdjson::builder::to_json_string(result).get(result_json);
    if (rpc_error)
    {
        co_return rpc_error;
    }

    co_return serialize_json_rpc_success(result_json, std::move(request.id), response);
}
