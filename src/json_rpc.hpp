#ifndef CHAT_SRC_JSON_RPC_HPP
#define CHAT_SRC_JSON_RPC_HPP

#include <string>
#include <string_view>
#include <utility>

#include <simdjson.h>

struct json_rpc_id
{
    bool present = false;
    std::string json;
};

struct json_rpc_params
{
    bool present = false;
    std::string json;
};

struct json_rpc_request
{
    std::string method;
    json_rpc_params params;
    json_rpc_id id;
};

simdjson::error_code parse_json_rpc_request(std::string& input, json_rpc_request& request, std::string& response);

simdjson::error_code serialize_json_rpc_success(std::string_view result_json, json_rpc_id id, std::string& response);

simdjson::error_code serialize_json_rpc_error(int code, std::string_view message, json_rpc_id id, std::string& response);

simdjson::error_code serialize_json_rpc_method_not_found(json_rpc_id id, std::string& response);

simdjson::error_code serialize_json_rpc_invalid_params(json_rpc_id id, std::string& response);

template<class T>
simdjson::error_code serialize_json_rpc_value(T const& value, json_rpc_id id, std::string& response)
{
    std::string json;
    if (auto error = simdjson::builder::to_json_string(value).get(json))
    {
        return error;
    }
    return serialize_json_rpc_success(json, std::move(id), response);
}

#endif
