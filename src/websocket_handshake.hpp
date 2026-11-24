#ifndef CHAT_SRC_WEBSOCKET_HANDSHAKE_HPP
#define CHAT_SRC_WEBSOCKET_HANDSHAKE_HPP

#include <string>
#include <string_view>

namespace chat::detail
{
std::string_view trim_http_whitespace(std::string_view value);
bool has_websocket_upgrade(std::string_view connection, std::string_view upgrade);
bool make_websocket_accept(std::string_view key, std::string& accept);
}    // namespace chat::detail

#endif
