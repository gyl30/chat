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
#include "member.hpp"
#include "presence.hpp"
#include "typing.hpp"
#include "user.hpp"

namespace chat
{

struct authentication_result
{
    bool authenticated = false;
    std::int64_t user = 0;
    avatar_state avatar;
};

class client
{
   public:
    using connection_handler = std::function<void()>;
    using error_handler = std::function<void(error const&)>;
    using authenticate_handler = std::function<void(std::expected<authentication_result, error>)>;
    using register_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using conversations_handler = std::function<void(std::expected<conversations_result, error>)>;
    using mute_handler = std::function<void(std::expected<bool, error>)>;
    using pin_handler = std::function<void(std::expected<bool, error>)>;
    using conversation_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using message_result_handler = std::function<void(std::expected<message, error>)>;
    using attachment_handler = std::function<void(std::expected<std::string, error>)>;
    using messages_handler = std::function<void(std::expected<messages_result, error>)>;
    using send_message_handler = std::function<void(std::expected<send_message_result, error>)>;
    using users_handler = std::function<void(std::expected<std::vector<user>, error>)>;
    using members_handler = std::function<void(std::expected<std::vector<conversation_member>, error>)>;
    using group_action_handler = std::function<void(std::expected<bool, error>)>;
    using user_handler = std::function<void(std::expected<user, error>)>;
    using remove_contact_handler = std::function<void(std::expected<bool, error>)>;
    using mark_read_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using presences_handler = std::function<void(std::expected<std::vector<presence>, error>)>;
    using message_handler = std::function<void(message)>;
    using read_handler = std::function<void(std::int64_t conversation, std::int64_t user, std::int64_t message)>;
    using conversation_changed_handler = std::function<void(std::int64_t conversation, bool removed)>;
    using presence_handler = std::function<void(presence)>;
    using typing_handler = std::function<void(typing_event)>;
    using typing_result_handler = std::function<void(std::expected<bool, error>)>;
    using avatar_state_handler = std::function<void(std::expected<avatar_state, error>)>;
    using avatar_handler = std::function<void(std::expected<avatar, error>)>;
    using avatar_changed_handler = std::function<void(std::int64_t user, avatar_state)>;
    using reaction_handler = std::function<void(reaction_update)>;
    using reaction_result_handler = std::function<void(std::expected<reaction_update, error>)>;

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
    void set_message_updated_handler(message_handler handler);
    void set_read_handler(read_handler handler);
    void set_presence_handler(presence_handler handler);
    void set_typing_handler(typing_handler handler);
    void set_conversation_handler(conversation_changed_handler handler);
    void set_avatar_handler(avatar_changed_handler handler);
    void set_reaction_handler(reaction_handler handler);

    void connect(std::string url);
    void close();

    void authenticate(std::string username, std::string password, authenticate_handler handler);
    void register_user(std::string username, std::string password, register_handler handler);
    void get_conversations(std::optional<conversation_cursor> before, conversations_handler handler);
    void set_conversation_muted(std::int64_t conversation, bool muted, mute_handler handler);
    void set_conversation_pinned(std::int64_t conversation, bool pinned, pin_handler handler);
    void open_direct_conversation(std::int64_t user, conversation_handler handler);
    void create_group(std::string title, std::vector<std::int64_t> members, conversation_handler handler);
    void get_members(std::int64_t conversation, members_handler handler);
    void set_group_admin(std::int64_t conversation, std::int64_t user, bool admin, group_action_handler handler);
    void transfer_group_owner(std::int64_t conversation, std::int64_t user, group_action_handler handler);
    void remove_group_member(std::int64_t conversation, std::int64_t user, group_action_handler handler);
    void rename_group(std::int64_t conversation, std::string title, group_action_handler handler);
    void invite_group_members(std::int64_t conversation, std::vector<std::int64_t> members, group_action_handler handler);
    void leave_group(std::int64_t conversation, group_action_handler handler);
    void pin_group_message(std::int64_t conversation, std::int64_t message, group_action_handler handler);
    void unpin_group_message(std::int64_t conversation, group_action_handler handler);
    void get_contacts(users_handler handler);
    void get_presence(presences_handler handler);
    void get_messages(std::int64_t conversation, std::optional<std::int64_t> before, messages_handler handler,
                      std::optional<std::int64_t> after = {});
    void search_messages(std::int64_t conversation, std::string query, std::optional<std::int64_t> before,
                         messages_handler handler);
    void send_message(std::int64_t conversation, std::string text, send_message_handler handler,
                      std::optional<std::int64_t> reply_to = {});
    void delete_message(std::int64_t conversation, std::int64_t message, message_result_handler handler);
    void set_message_reaction(std::int64_t conversation, std::int64_t message, std::string emoji,
                              reaction_result_handler handler);
    void send_attachment(std::int64_t conversation, std::string filename, std::string data,
                         message_result_handler handler, std::optional<std::int64_t> reply_to = {});
    void get_attachment(std::int64_t conversation, std::int64_t message, attachment_handler handler);
    void edit_message(std::int64_t conversation, std::int64_t message, std::string text,
                      message_result_handler handler);
    void search_users(std::string query, users_handler handler);
    void add_contact(std::int64_t user, user_handler handler);
    void remove_contact(std::int64_t user, remove_contact_handler handler);
    void mark_read(std::int64_t conversation, std::int64_t message, mark_read_handler handler);
    void set_typing(std::int64_t conversation, bool typing, typing_result_handler handler);
    void set_avatar(std::string data, avatar_state_handler handler);
    void get_avatar(std::int64_t user, std::int64_t revision, avatar_handler handler);
    void clear_avatar(avatar_state_handler handler);

   private:
    struct impl;

    std::unique_ptr<impl> impl_;
};

}    // namespace chat

#endif
