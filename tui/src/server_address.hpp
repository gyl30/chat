#ifndef CHAT_TUI_SERVER_ADDRESS_HPP
#define CHAT_TUI_SERVER_ADDRESS_HPP

#include <chat/server_url.hpp>

#include <arpa/inet.h>
#include <charconv>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace chat::tui
{

// The login page's server setting: "host:port", whether the address asked for TLS, and the
// rest of the URL (path and query), kept apart from the URL they make.
struct server_address
{
    std::string host_port;
    bool tls = false;
    std::string target = "/ws";
};

// A ws:// or wss:// URL into its parts, with the client's own URL rules; nothing for any other
// text. A missing port is filled in as the one the client would use (80).
inline std::optional<server_address> split_server_url(std::string_view url)
{
    bool const tls = url.starts_with("wss://");
    auto const parts = parse_server_url(tls ? "ws://" + std::string(url.substr(6)) : std::string(url));
    if (!parts) { return std::nullopt; }
    return server_address{parts->port.empty() ? parts->host_header + ":80" : parts->host_header, tls, parts->target};
}

// "host:port" into the URL to connect to, keeping the target, or the reason it is not usable.
// The host is an IPv4 address, an ASCII name, or an IPv6 address in brackets; the port is
// required. The client has no TLS yet, so a TLS address is refused here rather than at login.
inline std::expected<std::string, std::string> join_server_url(std::string_view host_port, bool tls, std::string_view target = "/ws")
{
    if (tls) { return std::unexpected("暂不支持 TLS（wss://），请使用 ws://"); }
    while (!host_port.empty() && (host_port.front() == ' ' || host_port.front() == '\t')) { host_port.remove_prefix(1); }
    while (!host_port.empty() && (host_port.back() == ' ' || host_port.back() == '\t')) { host_port.remove_suffix(1); }
    if (host_port.find("://") != std::string_view::npos)
    { return std::unexpected("只填写 IP 和端口，例如 127.0.0.1:18080"); }
    for (unsigned char c : host_port)
    {
        if (c >= 0x80) { return std::unexpected("域名请使用 ASCII；中文域名请写成 xn-- 形式"); }
    }
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
            if (std::string_view("[]:\\\"<>^`{|}%").find(static_cast<char>(c)) != std::string_view::npos) { valid = false; }
        }
    }
    if (!valid) { return std::unexpected("IP 或域名无效；IPv6 地址请写成 [::1]:18080"); }
    unsigned value = 0;
    auto const [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
    if (port.empty() || error != std::errc{} || end != port.data() + port.size() || value == 0 || value > 65535)
    { return std::unexpected("端口须为 1–65535 的数字"); }
    if (!target.empty() && target.front() != '/' && target.front() != '?') { target = "/ws"; }
    auto url = "ws://" + std::string(host_port) + std::string(target);
    // The client's own rules have the last word, so a saved address is one it will try.
    if (!parse_server_url(url)) { return std::unexpected("地址无效，请检查 IP（或域名）和端口"); }
    return url;
}

}

#endif
