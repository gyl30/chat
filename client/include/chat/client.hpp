#ifndef CHAT_CLIENT_INCLUDE_CHAT_CLIENT_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_CLIENT_HPP

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "error.hpp"

namespace chat
{

class client
{
   public:
    using connection_handler = std::function<void()>;
    using error_handler = std::function<void(error const&)>;
    using authenticate_handler = std::function<void(std::expected<bool, error>)>;

    client();
    ~client();

    client(client const&) = delete;
    client& operator=(client const&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    void set_connected_handler(connection_handler handler);
    void set_disconnected_handler(connection_handler handler);
    void set_error_handler(error_handler handler);

    void connect(std::string url);
    void close();

    void authenticate(std::string username, std::string password, authenticate_handler handler);

   private:
    struct impl;

    std::unique_ptr<impl> impl_;
};

}    // namespace chat

#endif
