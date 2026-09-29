#ifndef CHAT_SRC_JSON_RPC_HPP
#define CHAT_SRC_JSON_RPC_HPP

#include <string>

#include <simdjson.h>

simdjson::error_code dispatch_json_rpc(std::string& request, std::string& response);

#endif
