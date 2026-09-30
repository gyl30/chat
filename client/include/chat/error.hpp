#ifndef CHAT_CLIENT_INCLUDE_CHAT_ERROR_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_ERROR_HPP

#include <string>

namespace chat
{

enum class error_kind
{
    transport,
    protocol,
    rpc,
};

struct error
{
    error_kind kind = error_kind::protocol;
    int code = 0;
    std::string message;
};

}    // namespace chat

#endif
