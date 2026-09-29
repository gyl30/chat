#include <string>
#include <utility>
#include <string_view>

#include <simdjson.h>

#include "json_rpc.hpp"
#include "chat_session.hpp"

namespace
{

constexpr std::string_view kEchoMethod = "echo";

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

chat_session::chat_session(websocket_connection& connection) : connection_(connection) {}

boost::capy::task<void> chat_session::run()
{
    std::string response;

    for (;;)
    {
        auto receive_result = co_await connection_.receive();
        auto& [ec, message] = receive_result;
        if (ec || message.message_type == websocket_message::type::close)
        {
            break;
        }

        json_rpc_request request;
        auto rpc_error = parse_json_rpc_request(message.payload, request, response);
        if (rpc_error)
        {
            break;
        }

        if (response.empty())
        {
            if (request.method != kEchoMethod)
            {
                if (request.id.present)
                {
                    rpc_error = serialize_json_rpc_method_not_found(std::move(request.id), response);
                }
            }
            else
            {
                echo_params params{};
                auto params_error = parse_echo_params(request.params, params);
                if (params_error)
                {
                    if (request.id.present)
                    {
                        rpc_error = serialize_json_rpc_invalid_params(std::move(request.id), response);
                    }
                }
                else if (request.id.present)
                {
                    echo_result result{};
                    result.text = std::move(params.text);

                    std::string result_json;
                    rpc_error = simdjson::builder::to_json_string(result).get(result_json);
                    if (!rpc_error)
                    {
                        rpc_error = serialize_json_rpc_success(result_json, std::move(request.id), response);
                    }
                }
            }
        }

        if (rpc_error)
        {
            break;
        }
        if (response.empty())
        {
            continue;
        }

        auto [send_ec] = co_await connection_.send_text(response);
        if (send_ec)
        {
            break;
        }
    }
}
