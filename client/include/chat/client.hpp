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
#include "presence.hpp"
#include "user.hpp"

namespace chat
{

struct authentication_result
{
    bool authenticated = false;
    std::int64_t user = 0;
};

class client
{
   public:
    using connection_handler = std::function<void()>;
    using error_handler = std::function<void(error const&)>;
    using authenticate_handler = std::function<void(std::expected<authentication_result, error>)>;
    using register_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using conversations_handler = std::function<void(std::expected<conversations_result, error>)>;
    using conversation_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using messages_handler = std::function<void(std::expected<messages_result, error>)>;
    using send_message_handler = std::function<void(std::expected<send_message_result, error>)>;
    using users_handler = std::function<void(std::expected<std::vector<user>, error>)>;
    using user_handler = std::function<void(std::expected<user, error>)>;
    using mark_read_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using presences_handler = std::function<void(std::expected<std::vector<presence>, error>)>;
    using message_handler = std::function<void(message)>;
    using read_handler = std::function<void(std::int64_t conversation, std::int64_t user, std::int64_t message)>;
    using conversation_changed_handler = std::function<void(std::int64_t conversation)>;
    using presence_handler = std::function<void(presence)>;

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
    void set_read_handler(read_handler handler);
    void set_presence_handler(presence_handler handler);
    void set_conversation_handler(conversation_changed_handler handler);

    void connect(std::string url);
    void close();

    void authenticate(std::string username, std::string password, authenticate_handler handler);
    void register_user(std::string username, std::string password, register_handler handler);
    void get_conversations(std::optional<conversation_cursor> before, conversations_handler handler);
    void open_direct_conversation(std::int64_t user, conversation_handler handler);
    void create_group(std::string title, std::vector<std::int64_t> members, conversation_handler handler);
    void get_members(std::int64_t conversation, users_handler handler);
    void get_contacts(users_handler handler);
    void get_presence(presences_handler handler);
    void get_messages(std::int64_t conversation, std::optional<std::int64_t> before, messages_handler handler,
                      std::optional<std::int64_t> after = {});
    void send_message(std::int64_t conversation, std::string text, send_message_handler handler,
                      std::optional<std::int64_t> reply_to = {});
    void search_users(std::string query, users_handler handler);
    void add_contact(std::int64_t user, user_handler handler);
    void mark_read(std::int64_t conversation, std::int64_t message, mark_read_handler handler);

   private:
    struct impl;

    std::unique_ptr<impl> impl_;
};

}    // namespace chat

#endif
