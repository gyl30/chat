#ifndef CHAT_SERVER_URL_HPP
#define CHAT_SERVER_URL_HPP

#include <optional>
#include <string>
#include <string_view>

namespace chat
{

// A server address the client can connect to, split as the connection uses it.
struct server_url
{
    std::string host;         // for name resolution: an IPv6 address without its brackets
    std::string port;         // as written; empty means 80 for ws:// or 443 for wss://
    std::string host_header;  // the Host header: the host as written, with a nonempty ":port" if given
    std::string target;       // path and query as written; empty means "/"
    bool tls = false;
};

// A ws:// or wss:// URL with a host and no user info or fragment; nothing for any other text.
// The same rules decide whether connect() tries the address.
std::optional<server_url> parse_server_url(std::string_view url);

}

#endif
