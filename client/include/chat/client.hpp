#ifndef CHAT_CLIENT_INCLUDE_CHAT_CLIENT_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_CLIENT_HPP

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "error.hpp"
#include "transport.hpp"

namespace chat
{

class client : private transport_listener
{
   public:
    using connection_handler = std::function<void()>;
    using error_handler = std::function<void(error const&)>;
    using authenticate_handler = std::function<void(std::expected<bool, error>)>;

    explicit client(std::unique_ptr<transport> transport);
    ~client() override;

    void set_connected_handler(connection_handler handler);
    void set_disconnected_handler(connection_handler handler);
    void set_error_handler(error_handler handler);

    void open(std::string url);
    void close();

    void authenticate(std::string username, std::string password, authenticate_handler handler);

   private:
    struct impl;

    void on_transport_open() override;
    void on_transport_text(std::string message) override;
    void on_transport_close() override;
    void on_transport_error(std::string message) override;

    std::unique_ptr<impl> impl_;
};

}    // namespace chat

#endif
