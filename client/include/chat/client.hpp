#ifndef CHAT_CLIENT_INCLUDE_CHAT_CLIENT_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_CLIENT_HPP

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "conversation.hpp"
#include "error.hpp"
#include "message.hpp"

namespace chat
{

class client
{
   public:
    using connection_handler = std::function<void()>;
    using error_handler = std::function<void(error const&)>;
    using authenticate_handler = std::function<void(std::expected<bool, error>)>;
    using conversations_handler = std::function<void(std::expected<std::vector<conversation>, error>)>;
    using messages_handler = std::function<void(std::expected<std::vector<message>, error>)>;
    using send_message_handler = std::function<void(std::expected<send_message_result, error>)>;
    using mark_read_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using message_handler = std::function<void(message)>;

    client();
    ~client();

    client(client const&) = delete;
    client& operator=(client const&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    void set_connected_handler(connection_handler handler);
    void set_disconnected_handler(connection_handler handler);
    void set_error_handler(error_handler handler);
    void set_message_handler(message_handler handler);

    void connect(std::string url);
    void close();

    void authenticate(std::string username, std::string password, authenticate_handler handler);
    void get_conversations(std::optional<std::int64_t> before, conversations_handler handler);
    void get_messages(std::int64_t user, std::optional<std::int64_t> before, messages_handler handler);
    void send_message(std::int64_t user, std::string text, send_message_handler handler);
    void mark_read(std::int64_t user, std::int64_t message, mark_read_handler handler);

   private:
    struct impl;

    std::unique_ptr<impl> impl_;
};

}    // namespace chat

#endif
