#ifndef CHAT_SRC_JSON_RPC_HPP
#define CHAT_SRC_JSON_RPC_HPP

#include <string>
#include <string_view>

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

#endif
