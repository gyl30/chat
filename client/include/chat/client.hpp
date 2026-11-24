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
#include "login_session.hpp"
#include "member.hpp"
#include "presence.hpp"
#include "typing.hpp"
#include "user.hpp"
#include "friendship.hpp"

namespace chat
{

struct authentication_result
{
    bool authenticated = false;
    std::int64_t user = 0;
    avatar_state avatar;
};

struct pending_attachment
{
    std::int64_t client_id, conversation, size, reply_to, confirmed;
    std::string path, filename;
    bool group_file = false;
};

enum class outbox_state : std::uint8_t
{
    queued,
    sending,
    failed,
};

struct compose_draft
{
    std::int64_t conversation;
    std::int64_t revision;
    std::int64_t reply;
    std::string text;
};

struct pending_text
{
    std::int64_t client_id, conversation, reply_to, created;
    std::string text;
    outbox_state state = outbox_state::queued;
    int rejection = 0;
};

class client
{
   public:
    using outbox_handler = std::function<void(pending_text)>;
    using pending_texts_handler = std::function<void(std::expected<std::vector<pending_text>, error>)>;
    using outbox_operation_handler = std::function<void(std::expected<void, error>)>;
    using connection_handler = std::function<void()>;
    using error_handler = std::function<void(error const&)>;
    using authenticate_handler = std::function<void(std::expected<authentication_result, error>)>;
    using login_sessions_handler = std::function<void(std::expected<std::vector<login_session>, error>)>;
    using register_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using conversations_handler = std::function<void(std::expected<conversations_result, error>)>;
    using pin_handler = std::function<void(std::expected<bool, error>)>;
    using conversation_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using message_result_handler = std::function<void(std::expected<message, error>)>;
    using attachment_handler = std::function<void(std::expected<std::string, error>)>;
    using message_search_handler = std::function<void(std::expected<message_search_page, error>)>;
    using bookmark_state_handler = std::function<void(std::expected<bookmark_state, error>)>;
    using bookmark_page_handler = std::function<void(std::expected<bookmark_page, error>)>;
    using bookmark_changed_handler = std::function<void(std::int64_t message)>;
    using messages_handler = std::function<void(std::expected<messages_result, error>)>;
    using users_handler = std::function<void(std::expected<std::vector<user>, error>)>;
    using members_handler = std::function<void(std::expected<std::vector<conversation_member>, error>)>;
    using group_action_handler = std::function<void(std::expected<bool, error>)>;
    using group_invite_handler = std::function<void(std::expected<std::optional<std::string>, error>)>;
    using group_join_handler = std::function<void(std::expected<group_join_result, error>)>;
    using group_join_requests_handler = std::function<void(std::expected<group_join_requests_result, error>)>;
    using group_join_request_handler = std::function<void(group_join_request_event)>;
    using friendship_handler = std::function<void(std::expected<friendship_result, error>)>;
    using friend_requests_handler = std::function<void(std::expected<friend_requests_result, error>)>;
    using friendship_changed_handler = std::function<void(std::int64_t user)>;
    using user_handler = std::function<void(std::expected<user, error>)>;
    using remove_contact_handler = std::function<void(std::expected<bool, error>)>;
    using mark_read_handler = std::function<void(std::expected<std::int64_t, error>)>;
    using presences_handler = std::function<void(std::expected<std::vector<presence>, error>)>;
    using message_handler = std::function<void(message)>;
    using read_handler = std::function<void(std::int64_t conversation, std::int64_t user, std::int64_t message)>;
    using conversation_view_handler = std::function<void(conversation_view_event)>;
    using conversation_changed_handler = std::function<void(std::int64_t conversation, bool removed)>;
    using presence_handler = std::function<void(presence)>;
    using typing_handler = std::function<void(typing_event)>;
    using typing_result_handler = std::function<void(std::expected<bool, error>)>;
    using avatar_state_handler = std::function<void(std::expected<avatar_state, error>)>;
    using avatar_handler = std::function<void(std::expected<avatar, error>)>;
    using avatar_changed_handler = std::function<void(std::int64_t user, avatar_state)>;
    using reaction_handler = std::function<void(reaction_update)>;
    using reaction_result_handler = std::function<void(std::expected<reaction_update, error>)>;

    using progress_handler = std::function<void(std::int64_t, std::int64_t)>;
    using pending_attachments_handler = std::function<void(std::expected<std::vector<pending_attachment>, error>)>;
    explicit client(std::string transfer_directory = {});
    ~client();

    client(client const&) = delete;
    client& operator=(client const&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    void set_connected_handler(connection_handler handler);
    void set_disconnected_handler(connection_handler handler);
    void set_login_revoked_handler(connection_handler handler);
    void set_login_sessions_changed_handler(connection_handler handler);
    void set_error_handler(error_handler handler);
    void set_message_handler(message_handler handler);
    void set_message_updated_handler(message_handler handler);
    void set_read_handler(read_handler handler);
    void set_presence_handler(presence_handler handler);
    void set_typing_handler(typing_handler handler);
    void set_conversation_view_handler(conversation_view_handler handler);
    void clear_history(std::int64_t conversation, std::int64_t through, mark_read_handler handler);
    void hide_conversation(std::int64_t conversation, group_action_handler handler);
    void set_conversation_handler(conversation_changed_handler handler);
    void set_avatar_handler(avatar_changed_handler handler);
    void set_bookmark_changed_handler(bookmark_changed_handler handler);
    void set_reaction_handler(reaction_handler handler);
    void set_group_join_request_handler(group_join_request_handler handler);
    void set_friendship_handler(friendship_changed_handler handler);

    void connect(std::string url);
    void close(bool keep_local_account = false);

    void authenticate(
        std::string username, std::string password, authenticate_handler handler, std::string device_name = {}, std::string client_type = {});
    void resume_session(std::string username, authenticate_handler handler);
    void get_login_sessions(login_sessions_handler handler);
    void revoke_login_session(std::int64_t session, group_action_handler handler);
    void revoke_other_login_sessions(group_action_handler handler);
    void logout_session(group_action_handler handler);
    void change_password(std::string current, std::string replacement, group_action_handler handler);
    void register_user(std::string username, std::string password, register_handler handler);
    void get_conversations(std::optional<conversation_cursor> before, conversations_handler handler);
    void set_notification_level(std::int64_t conversation, notification_level level, std::int64_t until, group_action_handler handler);
    void mark_conversations_read(std::optional<std::int64_t> conversation, std::optional<std::int64_t> folder, group_action_handler handler);
    void set_unread_mark(std::int64_t conversation, bool marked, group_action_handler handler);
    void create_conversation_folder(std::string name, group_action_handler handler);
    void rename_conversation_folder(std::int64_t folder, std::string name, group_action_handler handler);
    void delete_conversation_folder(std::int64_t folder, group_action_handler handler);
    void reorder_conversation_folder(std::int64_t folder, bool up, group_action_handler handler);
    void move_conversation_to_folder(std::int64_t conversation, std::int64_t folder, group_action_handler handler);
    void set_conversation_pinned(std::int64_t conversation, bool pinned, pin_handler handler);
    void open_direct_conversation(std::int64_t user, conversation_handler handler);
    void create_group(std::string title, std::vector<std::int64_t> members, conversation_handler handler);
    void get_members(std::int64_t conversation, members_handler handler);
    void set_group_admin(std::int64_t conversation, std::int64_t user, bool admin, group_action_handler handler);
    void transfer_group_owner(std::int64_t conversation, std::int64_t user, group_action_handler handler);
    void set_group_mute(std::int64_t conversation, bool muted, group_action_handler handler);
    // seconds: -1 permanent, 0 clear, positive duration measured by the server.
    void set_group_member_mute(std::int64_t conversation, std::int64_t user, std::int64_t seconds, group_action_handler handler);
    void dissolve_group(std::int64_t conversation, group_action_handler handler);
    void remove_group_member(std::int64_t conversation, std::int64_t user, group_action_handler handler);
    void rename_group(std::int64_t conversation, std::string title, group_action_handler handler);
    void set_group_announcement(std::int64_t conversation, std::string text, group_action_handler handler);
    void get_group_invite(std::int64_t conversation, group_invite_handler handler);
    void create_group_invite(std::int64_t conversation, group_invite_handler handler);
    void revoke_group_invite(std::int64_t conversation, group_invite_handler handler);
    void join_group(std::string token, group_join_handler handler);
    void set_group_join_approval(std::int64_t conversation, bool required, group_action_handler handler);
    void get_group_join_requests(std::int64_t conversation, std::optional<std::int64_t> before, group_join_requests_handler handler);
    void respond_group_join_request(std::int64_t conversation, std::int64_t user, bool accept, group_action_handler handler);
    void invite_group_members(std::int64_t conversation, std::vector<std::int64_t> members, group_action_handler handler);
    void leave_group(std::int64_t conversation, group_action_handler handler);
    void pin_group_message(std::int64_t conversation, std::int64_t message, group_action_handler handler);
    void unpin_group_message(std::int64_t conversation, group_action_handler handler);
    using contacts_handler = std::function<void(std::expected<contact_book, error>)>;
    void get_contacts(contacts_handler handler);
    void set_contacts_changed_handler(connection_handler handler);
    void set_friend_remark(std::int64_t user, std::string remark, group_action_handler handler);
    void move_friend(std::int64_t user, std::string group, group_action_handler handler);
    void create_friend_group(std::string name, group_action_handler handler);
    void rename_friend_group(std::string group, std::string name, group_action_handler handler);
    void delete_friend_group(std::string group, group_action_handler handler);
    void get_presence(presences_handler handler);
    void get_messages(std::int64_t conversation,
                      std::optional<std::int64_t> before,
                      messages_handler handler,
                      std::optional<std::int64_t> after = {},
                      bool capture_unread = false);
    void search_all_messages(message_search_filter const& filter, std::optional<std::int64_t> before, message_search_handler handler);
    void set_message_bookmark(std::int64_t message, bool bookmarked, bookmark_state_handler handler);
    void get_message_bookmark(std::int64_t message, bookmark_state_handler handler);
    void list_message_bookmarks(std::string query, std::optional<std::int64_t> before, bookmark_page_handler handler);
    using message_search_scopes_handler = std::function<void(std::expected<std::vector<message_search_scope>, error>)>;
    void get_message_search_scopes(message_search_scopes_handler handler);
    void get_categorized_history(
        std::int64_t conversation, message_category category, std::string query, std::optional<std::int64_t> before, messages_handler handler);
    void get_messages_around(std::int64_t conversation, std::int64_t around, messages_handler handler);
    void search_messages(std::int64_t conversation, std::string query, std::optional<std::int64_t> before, messages_handler handler);
    static std::int64_t new_client_id();
    // The callback reports the local commit. Confirmed messages use the message handler.
    void send_message(std::int64_t conversation,
                      std::string text,
                      outbox_operation_handler queued,
                      std::optional<std::int64_t> reply_to = {},
                      std::optional<std::int64_t> client_id = {},
                      std::int64_t draft_revision = 0);
    using drafts_handler = std::function<void(std::expected<std::vector<compose_draft>, error>)>;
    void save_draft(std::int64_t user, compose_draft draft, outbox_operation_handler handler);
    void load_drafts(std::int64_t user, drafts_handler handler);
    void set_outbox_handler(outbox_handler handler);
    void pending_texts(pending_texts_handler handler);
    void retry_text(std::int64_t client_id, outbox_operation_handler handler);
    void discard_text(std::int64_t client_id, outbox_operation_handler handler);
    void forward_message(std::int64_t conversation, std::int64_t source, std::int64_t client_id, message_result_handler handler);
    void delete_message(std::int64_t conversation, std::int64_t message, message_result_handler handler);
    void set_message_reaction(std::int64_t conversation, std::int64_t message, std::string emoji, reaction_result_handler handler);
    void send_attachment(std::int64_t conversation,
                         std::string path,
                         message_result_handler handler,
                         std::optional<std::int64_t> reply_to = {},
                         std::optional<std::int64_t> client_id = {},
                         progress_handler progress = {});
    using conversation_files_handler = std::function<void(std::expected<conversation_file_page, error>)>;
    void upload_conversation_file(
        std::int64_t conversation, std::string path, std::int64_t client_id, group_action_handler handler, progress_handler progress = {});
    void add_conversation_file(std::int64_t conversation, std::string key, group_action_handler handler);
    void remove_conversation_file(std::int64_t conversation, std::string key, group_action_handler handler);
    void list_conversation_files(std::int64_t conversation,
                                 std::string search,
                                 std::string category,
                                 std::int64_t before,
                                 std::string before_key,
                                 conversation_files_handler handler);
    void download_conversation_file(
        std::int64_t conversation, std::string key, std::string path, attachment_handler handler, download_options options = {});
    void pending_attachments(pending_attachments_handler handler);
    void interrupt_attachment(std::int64_t client_id, bool cancel, group_action_handler handler);
    void get_attachment(std::int64_t conversation,
                        std::int64_t message,
                        std::string path,
                        attachment_handler handler,
                        std::int64_t limit = max_attachment_size,
                        download_options options = {});
    void edit_message(std::int64_t conversation, std::int64_t message, std::string text, message_result_handler handler);
    void search_users(std::string query, users_handler handler);
    void get_friend_requests(friend_requests_handler handler);
    void send_friend_request(std::int64_t user, friendship_handler handler);
    void respond_friend_request(std::int64_t user, bool accept, friendship_handler handler);
    void cancel_friend_request(std::int64_t user, friendship_handler handler);
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
