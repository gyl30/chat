#include <string>
#include <utility>
#include <string_view>

#include <simdjson.h>

#include "json_rpc.hpp"

struct json_rpc_id
{
    bool present = false;
    std::string json;
};

struct json_rpc_id_adapter
{
    static simdjson::error_code deserialize(simdjson::ondemand::value& value, json_rpc_id& id);
    static void serialize(simdjson::builder::string_builder& builder, json_rpc_id const& id);
};

struct json_rpc_params
{
    bool present = false;
    std::string json;
};

struct json_rpc_request
{
    std::string jsonrpc;
    std::string method;
    [[= simdjson::default_value]] json_rpc_params params;
    [[= simdjson::default_value, = simdjson::with<json_rpc_id_adapter>]] json_rpc_id id;
};

struct [[= simdjson::deny_unknown_fields]] echo_params
{
    std::string text;
};

struct echo_result
{
    std::string text;
};

struct json_rpc_error
{
    int code = 0;
    std::string message;
};

struct json_rpc_success_response
{
    std::string jsonrpc = "2.0";
    echo_result result;
    [[= simdjson::with<json_rpc_id_adapter>]] json_rpc_id id;
};

struct json_rpc_error_response
{
    std::string jsonrpc = "2.0";
    json_rpc_error error;
    [[= simdjson::with<json_rpc_id_adapter>]] json_rpc_id id;
};

namespace
{

std::string_view trim_json_whitespace(std::string_view value)
{
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
    {
        value.remove_suffix(1);
    }
    return value;
}

}    // namespace

simdjson::error_code json_rpc_id_adapter::deserialize(simdjson::ondemand::value& value, json_rpc_id& id)
{
    simdjson::ondemand::json_type type;
    auto error = value.type().get(type);
    if (error)
    {
        return error;
    }

    if (type != simdjson::ondemand::json_type::string && type != simdjson::ondemand::json_type::number &&
        type != simdjson::ondemand::json_type::null)
    {
        return simdjson::INCORRECT_TYPE;
    }

    auto raw = trim_json_whitespace(value.raw_json_token());
    if (type == simdjson::ondemand::json_type::string)
    {
        std::string_view decoded;
        error = value.get_string().get(decoded);
        (void)decoded;
    }
    else if (type == simdjson::ondemand::json_type::number)
    {
        double number = 0;
        error = value.get_double().get(number);
        (void)number;
    }
    else
    {
        bool is_null = false;
        error = value.is_null().get(is_null);
        if (!error && !is_null)
        {
            error = simdjson::INCORRECT_TYPE;
        }
    }

    if (error)
    {
        return error;
    }

    id.present = true;
    id.json.assign(raw);
    return simdjson::SUCCESS;
}

void json_rpc_id_adapter::serialize(simdjson::builder::string_builder& builder, json_rpc_id const& id)
{
    builder.append_raw(id.json);
}

namespace simdjson
{

template <typename value_type>
error_code tag_invoke(deserialize_tag, value_type& value, json_rpc_params& params)
{
    ondemand::json_type type;
    auto error = value.type().get(type);
    if (error)
    {
        return error;
    }
    if (type != ondemand::json_type::object && type != ondemand::json_type::array)
    {
        return INCORRECT_TYPE;
    }

    std::string_view raw;
    error = value.raw_json().get(raw);
    if (error)
    {
        return error;
    }

    params.present = true;
    params.json.assign(raw);
    return SUCCESS;
}

}    // namespace simdjson

namespace
{

constexpr std::string_view kJsonRpcVersion = "2.0";
constexpr std::string_view kEchoMethod = "echo";

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

constexpr std::string_view kParseErrorMessage = "Parse error";
constexpr std::string_view kInvalidRequestMessage = "Invalid Request";
constexpr std::string_view kMethodNotFoundMessage = "Method not found";
constexpr std::string_view kInvalidParamsMessage = "Invalid params";

bool is_json_syntax_error(simdjson::error_code error)
{
    switch (error)
    {
    case simdjson::TAPE_ERROR:
    case simdjson::STRING_ERROR:
    case simdjson::T_ATOM_ERROR:
    case simdjson::F_ATOM_ERROR:
    case simdjson::N_ATOM_ERROR:
    case simdjson::NUMBER_ERROR:
    case simdjson::UTF8_ERROR:
    case simdjson::EMPTY:
    case simdjson::UNESCAPED_CHARS:
    case simdjson::UNCLOSED_STRING:
    case simdjson::INCOMPLETE_ARRAY_OR_OBJECT:
    case simdjson::TRAILING_CONTENT:
        return true;
    default:
        return false;
    }
}

json_rpc_id null_id()
{
    json_rpc_id id;
    id.present = true;
    id.json = "null";
    return id;
}

simdjson::error_code serialize_error(int code, std::string_view message, json_rpc_id id, std::string& response)
{
    json_rpc_error_response error_response{};
    error_response.error.code = code;
    error_response.error.message.assign(message);
    error_response.id = std::move(id);
    return simdjson::builder::to_json_string(error_response).get(response);
}

simdjson::error_code serialize_success(echo_result result, json_rpc_id id, std::string& response)
{
    json_rpc_success_response success_response{};
    success_response.result = std::move(result);
    success_response.id = std::move(id);
    return simdjson::builder::to_json_string(success_response).get(response);
}

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

simdjson::error_code dispatch_json_rpc(std::string& request, std::string& response)
{
    response.clear();

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    auto error = parser.iterate(request).get(document);
    if (error)
    {
        if (is_json_syntax_error(error))
        {
            return serialize_error(kParseError, kParseErrorMessage, null_id(), response);
        }
        return error;
    }

    json_rpc_request rpc_request{};
    error = document.get(rpc_request);
    if (error)
    {
        if (is_json_syntax_error(error))
        {
            return serialize_error(kParseError, kParseErrorMessage, null_id(), response);
        }
        if (error == simdjson::INCORRECT_TYPE || error == simdjson::NO_SUCH_FIELD || error == simdjson::BIGINT_ERROR ||
            error == simdjson::NUMBER_OUT_OF_RANGE)
        {
            return serialize_error(kInvalidRequest, kInvalidRequestMessage, null_id(), response);
        }
        return error;
    }

    if (!document.at_end())
    {
        document.rewind();
        std::string_view raw;
        error = document.raw_json().get(raw);
        if (error)
        {
            if (is_json_syntax_error(error))
            {
                return serialize_error(kParseError, kParseErrorMessage, null_id(), response);
            }
            return error;
        }
        if (!document.at_end())
        {
            return serialize_error(kParseError, kParseErrorMessage, null_id(), response);
        }
    }

    if (rpc_request.jsonrpc != kJsonRpcVersion)
    {
        return serialize_error(kInvalidRequest, kInvalidRequestMessage, null_id(), response);
    }

    if (rpc_request.method != kEchoMethod)
    {
        if (!rpc_request.id.present)
        {
            return simdjson::SUCCESS;
        }
        return serialize_error(kMethodNotFound, kMethodNotFoundMessage, std::move(rpc_request.id), response);
    }

    echo_params params{};
    error = parse_echo_params(rpc_request.params, params);
    if (error)
    {
        if (!rpc_request.id.present)
        {
            return simdjson::SUCCESS;
        }
        return serialize_error(kInvalidParams, kInvalidParamsMessage, std::move(rpc_request.id), response);
    }

    if (!rpc_request.id.present)
    {
        return simdjson::SUCCESS;
    }

    echo_result result{};
    result.text = std::move(params.text);
    return serialize_success(std::move(result), std::move(rpc_request.id), response);
}
