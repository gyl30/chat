#ifndef CHAT_CLIENT_INCLUDE_CHAT_DETAIL_WEBSOCKET_LIMITS_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_DETAIL_WEBSOCKET_LIMITS_HPP

#include <cstddef>

namespace chat::detail
{

// Complete client-to-server WebSocket JSON payload, including RPC metadata and escaping.
inline constexpr std::size_t max_websocket_request_size = 64 * 1024;

} // namespace chat::detail

#endif
