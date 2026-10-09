#ifndef CHAT_TUI_SERVER_ADDRESS_HPP
#define CHAT_TUI_SERVER_ADDRESS_HPP

#include <arpa/inet.h>
#include <charconv>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace chat::tui
{

// The login page's server setting: "host:port" and TLS, kept apart from the URL they make.
struct server_address
{
    std::string host_port;
    bool tls = false;
    // The server's WebSocket path; "/ws" unless the address given on the command line said otherwise.
    std::string path = "/ws";
};

// "ws://h:p/path" or "wss://h:p/path" into its parts; nothing for any other text.
inline std::optional<server_address> split_server_url(std::string_view url)
{
    server_address value;
    if (url.starts_with("wss://")) { value.tls = true; url.remove_prefix(6); }
    else if (url.starts_with("ws://")) { url.remove_prefix(5); }
    else { return std::nullopt; }
    auto const slash = url.find('/');
    value.host_port = std::string(url.substr(0, slash));
    if (slash != std::string_view::npos) { value.path = std::string(url.substr(slash)); }
    return value;
}

// "host:port" with TLS on or off into the URL to connect to, or the reason it is not usable.
// The host is an IPv4 address, a name, or an IPv6 address in brackets; the port is required.
inline std::expected<std::string, std::string> join_server_url(std::string_view host_port, bool tls, std::string_view path = "/ws")
{
    while (!host_port.empty() && (host_port.front() == ' ' || host_port.front() == '\t')) { host_port.remove_prefix(1); }
    while (!host_port.empty() && (host_port.back() == ' ' || host_port.back() == '\t')) { host_port.remove_suffix(1); }
    if (host_port.find("://") != std::string_view::npos)
    { return std::unexpected("只填写 IP 和端口，例如 127.0.0.1:18080；加密连接请勾选 TLS"); }
    if (host_port.find_first_of(" \t/?#@") != std::string_view::npos)
    { return std::unexpected("地址只能包含 IP（或域名）和端口，例如 127.0.0.1:18080"); }
    auto const colon = host_port.rfind(':');
    if (colon == std::string_view::npos) { return std::unexpected("请填写端口，例如 127.0.0.1:18080"); }
    auto const host = host_port.substr(0, colon);
    auto const port = host_port.substr(colon + 1);
    // An IPv6 address goes in brackets and must parse as one; other hosts are IPv4 addresses
    // or names, without control characters or the characters URLs give a meaning to.
    bool valid = !host.empty();
    // No control characters anywhere (a NUL would also cut short what inet_pton reads).
    for (unsigned char c : host) { if (c < 0x21 || c == 0x7f) { valid = false; } }
    if (valid && host.starts_with('['))
    {
        unsigned char parsed[16];
        auto const inside = std::string(host.substr(1, host.size() > 1 ? host.size() - 2 : 0));
        valid = host.size() > 2 && host.ends_with(']') && ::inet_pton(AF_INET6, inside.c_str(), parsed) == 1;
    }
    else if (valid)
    {
        for (unsigned char c : host)
        {
            if (c < 0x21 || c == 0x7f || std::string_view("[]:\\\"<>^`{|}%").find(static_cast<char>(c)) != std::string_view::npos)
            { valid = false; }
        }
    }
    if (!valid) { return std::unexpected("IP 或域名无效；IPv6 地址请写成 [::1]:18080"); }
    unsigned value = 0;
    auto const [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
    if (port.empty() || error != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535)
    { return std::unexpected("端口须为 1–65535 的数字"); }
    if (path.empty() || path.front() != '/') { path = "/ws"; }
    return std::string(tls ? "wss://" : "ws://") + std::string(host_port) + std::string(path);
}

}

#endif
