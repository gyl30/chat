#ifndef CHAT_TUI_RECENT_LOGIN_HPP
#define CHAT_TUI_RECENT_LOGIN_HPP

#include <chat/text.hpp>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>

namespace chat::tui
{

// The last account that signed in successfully, as Qt keeps it: the username and server address,
// never the password.
struct recent_login
{
    std::string username;
    std::string server_url;
};

// Both ends use this bound, so whatever is saved can be read back.
inline constexpr std::size_t max_recent_login_size = 64 * 1024;

// $XDG_CONFIG_HOME/chat/tui-login, else ~/.config/chat/tui-login; none without a home directory.
inline std::optional<std::filesystem::path> recent_login_path()
{
    if (auto const* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
    { return std::filesystem::path(config) / "chat" / "tui-login"; }
    if (auto const* home = std::getenv("HOME"); home && *home)
    { return std::filesystem::path(home) / ".config" / "chat" / "tui-login"; }
    return std::nullopt;
}

// Exactly "username\nserver\n", both valid; anything else is treated as no record.
inline std::optional<recent_login> load_recent_login()
{
    auto const path = recent_login_path();
    if (!path) { return std::nullopt; }
    std::ifstream input(*path, std::ios::binary);
    if (!input) { return std::nullopt; }
    std::string const content{std::istreambuf_iterator<char>(input), {}};
    if (content.size() > max_recent_login_size || !content.ends_with('\n')) { return std::nullopt; }
    auto const first = content.find('\n');
    auto const second = content.find('\n', first + 1);
    if (second + 1 != content.size()) { return std::nullopt; }
    recent_login value{content.substr(0, first), content.substr(first + 1, second - first - 1)};
    auto const url_ok = (value.server_url.starts_with("ws://") || value.server_url.starts_with("wss://")) &&
                        value.server_url.find_first_of(std::string_view("\r\t \0", 4)) == std::string::npos;
    if (!chat::valid_username(value.username) || value.username.find('\r') != std::string::npos || !url_ok)
    { return std::nullopt; }
    return value;
}

// Written whole to a temporary file of this process's own and renamed over the record, so a crash
// or another client signing in at the same moment never leaves half a record.
inline void save_recent_login(recent_login const& value)
{
    auto const path = recent_login_path();
    if (!path || value.username.empty() || value.username.find_first_of("\r\n") != std::string::npos ||
        value.server_url.find_first_of("\r\n") != std::string::npos)
    { return; }
    if (value.username.size() + value.server_url.size() + 2 > max_recent_login_size) { return; }
    std::error_code error;
    std::filesystem::create_directories(path->parent_path(), error);
    if (error) { return; }
    auto pattern = path->string() + ".XXXXXX";
    int const descriptor = ::mkstemp(pattern.data());
    if (descriptor < 0) { return; }
    auto const content = value.username + '\n' + value.server_url + '\n';
    bool written = true;
    for (std::size_t offset = 0; written && offset < content.size();)
    {
        auto const count = ::write(descriptor, content.data() + offset, content.size() - offset);
        if (count < 0 && errno == EINTR) { continue; }
        written = count > 0;
        if (written) { offset += static_cast<std::size_t>(count); }
    }
    written = ::fsync(descriptor) == 0 && written;
    written = ::close(descriptor) == 0 && written;
    if (!written) { ::unlink(pattern.c_str()); return; }
    std::filesystem::rename(pattern, *path, error);
    if (error) { ::unlink(pattern.c_str()); }
}

}

#endif
