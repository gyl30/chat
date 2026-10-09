#ifndef CHAT_TUI_RECENT_LOGIN_HPP
#define CHAT_TUI_RECENT_LOGIN_HPP

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace chat::tui
{

// The last account that signed in successfully, as Qt keeps it: the username and server address,
// never the password.
struct recent_login
{
    std::string username;
    std::string server_url;
};

// $XDG_CONFIG_HOME/chat/tui-login, else ~/.config/chat/tui-login; none without a home directory.
inline std::optional<std::filesystem::path> recent_login_path()
{
    if (auto const* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
    { return std::filesystem::path(config) / "chat" / "tui-login"; }
    if (auto const* home = std::getenv("HOME"); home && *home)
    { return std::filesystem::path(home) / ".config" / "chat" / "tui-login"; }
    return std::nullopt;
}

inline std::optional<recent_login> load_recent_login()
{
    auto const path = recent_login_path();
    if (!path) { return std::nullopt; }
    std::ifstream input(*path);
    recent_login value;
    if (!std::getline(input, value.username) || !std::getline(input, value.server_url) || value.username.empty())
    { return std::nullopt; }
    return value;
}

// Written whole to a temporary file and renamed, so a crash never leaves half a record.
inline void save_recent_login(recent_login const& value)
{
    auto const path = recent_login_path();
    if (!path || value.username.empty()) { return; }
    std::error_code error;
    std::filesystem::create_directories(path->parent_path(), error);
    if (error) { return; }
    auto temporary = *path;
    temporary += ".new";
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << value.username << '\n' << value.server_url << '\n';
        if (!output) { return; }
    }
    std::filesystem::rename(temporary, *path, error);
}

}

#endif
