#pragma once

#include "state.hpp"
#include "inbox.hpp"
#include "deadline.hpp"
#include <chat/client.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace chat::tui
{
struct prompt
{
    std::string title;
    std::string text;
    bool confirmation = false;
};

// Every public action and all state access belong to the constructing/UI thread.
// SDK callbacks only enqueue value-owning tasks. No FTXUI dependency here.
class app
{
public:
    explicit app(std::function<void()> wake = {});
    ~app();
    app(app const&) = delete;
    app& operator=(app const&) = delete;

    state data;
    std::string server_url = "ws://127.0.0.1:18080/ws";
    std::string username;
    // Called after a successful sign-in (not a failed one) with the username and server; the
    // program stores them for next time. Tests leave it empty and never touch user files.
    std::function<void(std::string const&, std::string const&)> on_signed_in;
    std::string password;
    std::optional<prompt> dialog;
    bool command_mode = false;
    // An action menu drawn by the UI covers the chat: no typing, nothing read.
    bool menu_open = false;
    // The command palette (Ctrl+K) also covers the chat.
    bool palette_open = false;
    std::string command_text;
    bool exiting = false;
    int viewport_width = 80;
    // History rows visible in the chat page as last drawn; nothing is read while it is zero.
    int history_rows = 1;
    int viewport_height = 24;

    // before() runs ahead of any queued results, e.g. to finish pending pasted text.

    void drain(std::function<void()> const& before = {});
    void shutdown();
    void login(bool registration = false);
    void logout();
    void reconnect();
    void refresh();
    void conversations(bool more = false);
    void contacts();
    void friend_requests();
    void presence();
    void open_conversation(std::int64_t id);
    void history(bool older = false);
    // A newer destination than any page a pending request would still open (the account menu):
    // conversations opened by earlier "message", "create group" or "join" requests are dropped.
    void claim_destination();
    void search(std::string query, bool more = false);
    void members();
    void requests(bool more = false);
    void navigate(page target);
    void back();
    void activate();
    void command(std::string text);
    void submit_prompt();
    void cancel_prompt();
    void compose_changed();
    void send();
    void stop_composing();
    void sync_focus();
    // Applies an authoritative conversation list (a refresh or a further page).
    void apply_conversation_snapshot(conversations_result result, bool append);
    // The draft put away for a conversation that is not open.
    std::string saved_draft(std::int64_t conversation) const
    {
        auto found = drafts_.find(conversation);
        return found == drafts_.end() ? std::string{} : found->second;
    }
    // Runs before the composer stops taking input or its draft is put away, so input the UI
    // still holds (the tail of a paste) reaches the draft it was typed into.
    std::function<void()> before_input_change;
    // Asks for a read check; check_read() performs it after the UI knows the visible history.
    void mark_visible_read();
    void check_read();
    bool read_check = false;
    void select_message(int delta);
    void toggle_pick();
    void finish_pick();
    void tick();
    // Status set directly is an error; notify() is for success (expires) or progress (sticky).
    void notify(std::string text, bool sticky = false);
    void observe_status();
    void dismiss_error();

private:
    using clock = std::chrono::steady_clock;
    std::shared_ptr<inbox> inbox_;
    std::unique_ptr<deadline_timer> timer_;
    std::unique_ptr<chat::client> client_;
    std::thread::id ui_thread_;
    std::uint64_t session_ = 0;
    std::uint64_t view_ = 0;
    // Bumped by navigation and claim_destination(); a late request that would open a conversation
    // checks it, so it never replaces something the person chose after asking.
    std::uint64_t destination_ = 0;
    std::uint64_t list_request_ = 0;
    std::uint64_t search_request_ = 0;
    std::uint64_t contacts_request_ = 0;
    std::uint64_t friends_request_ = 0;
    std::uint64_t presence_request_ = 0;
    std::uint64_t members_request_ = 0;
    std::uint64_t history_request_ = 0;
    bool registering_ = false;
    bool reconnect_enabled_ = false;
    bool conversations_busy_ = false;
    bool conversations_again_ = false;
    bool history_busy_ = false;
    // PgUp arrived while a history page was loading.
    bool older_again_ = false;
    bool search_busy_ = false;
    bool requests_busy_ = false;
    bool requests_again_ = false;
    bool sending_ = false;
    bool typing_sent_ = false;
    int retry_ = 0;
    std::optional<clock::time_point> reconnect_at_;
    std::optional<clock::time_point> typing_stop_at_;
    std::optional<clock::time_point> status_expires_;
public:
    // How long a success notice stays; tests shorten it.
    std::chrono::milliseconds notice_duration{5000};
private:
    std::string status_set_;
    clock::time_point typing_last_{};
    std::unordered_map<std::int64_t, std::pair<std::string, clock::time_point>> typing_;
    std::unordered_map<std::int64_t, std::string> drafts_;
    std::vector<page> pages_;
    std::function<void(std::string)> prompt_action_;
    std::string pick_action_;
    std::string group_title_;
    std::int64_t pending_open_ = 0;
    std::int64_t marked_read_ = 0;
    int help_return_selected_ = 0;

    void assert_ui() const;
    void start_connection();
    void conversations_page(std::optional<conversation_cursor> cursor, bool append, std::uint64_t request,
                            std::size_t target, std::vector<chat::conversation> values = {});
    void disconnected();
    void schedule();
    void error(chat::error const& value);
    void ask(std::string title, std::string initial, std::function<void(std::string)> action, bool confirmation = false);
    void confirm(std::string title, std::function<void()> action);
    void group_command(std::string const& name, std::string argument);
    void message_command(std::string const& name, std::string argument);
    void profile_command(std::string const& name, std::string argument);
    bool online();
    bool writable();
    void group_done(bool left = false);
    void stop_typing();
    void incoming(chat::message value);
    void updated(chat::message value);
    void changed(std::int64_t conversation, bool removed);

    template<class F> auto callback(F action)
    {
        std::weak_ptr<inbox> weak = inbox_;
        auto const session = session_;
        return [this, weak, session, action = std::move(action)](auto value) mutable {
            if (auto queue = weak.lock())
            {
                queue->post([this, session, action, value = std::move(value)]() mutable {
                    if (!exiting && session == session_) { action(std::move(value)); }
                });
            }
        };
    }
    template<class F> auto result(F action)
    {
        return callback([this, action = std::move(action)](auto value) mutable {
            if (!value) { error(value.error()); return; }
            action(std::move(*value));
        });
    }
public:
    std::string typing_text() const;
};
}
