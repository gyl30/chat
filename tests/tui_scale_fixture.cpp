// Explicit diagnostic fixture, not part of CTest. All business mutations use chat::client.
// JSON lines below are local test controls; no JSON-RPC or WebSocket is implemented here.
#include <chat/client.hpp>
#include <chat/text.hpp>
#include "../tui/src/file_io.hpp"
#include <filesystem>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
namespace json = boost::json;
using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool value, std::string const& reason) { if (!value) { throw std::runtime_error(reason); } }
std::string text(json::object const& value, std::string_view key, std::string fallback = {})
{
    auto p = value.if_contains(key);
    if (!p) { return fallback; }
    require(p->is_string(), std::string(key) + " must be a string");
    return std::string(p->as_string());
}
std::int64_t number(json::object const& value, std::string_view key, std::int64_t fallback = 0)
{
    auto p = value.if_contains(key);
    if (!p || p->is_null()) { return fallback; }
    require(p->is_int64(), std::string(key) + " must be an integer");
    return p->as_int64();
}
bool flag(json::object const& value, std::string_view key, bool fallback = false)
{
    auto p = value.if_contains(key);
    if (!p) { return fallback; }
    require(p->is_bool(), std::string(key) + " must be boolean");
    return p->as_bool();
}
std::optional<std::int64_t> cursor(json::object const& value, std::string_view key)
{
    auto p = value.if_contains(key);
    return !p || p->is_null() ? std::nullopt : std::optional(number(value, key));
}
std::string role(chat::member_role value)
{
    return value == chat::member_role::owner ? "owner" : value == chat::member_role::admin ? "admin" : "member";
}
template<class T> json::value dto(T const& v)
{
    if constexpr (std::same_as<T, bool>) { return v; }
    else if constexpr (std::integral<T>) { return static_cast<std::int64_t>(v); }
    else if constexpr (std::same_as<T, std::string>) { return json::string(v); }
    else if constexpr (std::same_as<T, chat::avatar_state>) { return json::object{{"revision", v.revision}, {"present", v.present}}; }
    else if constexpr (std::same_as<T, chat::user>) { return json::object{{"id", v.id}, {"username", v.username}, {"avatar", dto(v.avatar)}}; }
    else if constexpr (std::same_as<T, chat::friendship_result>)
    {
        auto state = v.state == chat::friendship_state::accepted ? "accepted" :
            v.state == chat::friendship_state::outgoing_pending ? "outgoing_pending" :
            v.state == chat::friendship_state::incoming_pending ? "incoming_pending" : "none";
        return json::object{{"user", dto(v.user)}, {"state", state}};
    }
    else if constexpr (std::same_as<T, chat::friend_request>)
    { return json::object{{"user", dto(v.user)}, {"created_at", v.created_at}}; }
    else if constexpr (std::same_as<T, chat::friend_requests_result>)
    { return json::object{{"incoming", dto(v.incoming)}, {"outgoing", dto(v.outgoing)}}; }
    else if constexpr (std::same_as<T, chat::presence>) { return json::object{{"user", v.user}, {"online", v.online}, {"last_seen", v.last_seen}}; }
    else if constexpr (std::same_as<T, chat::conversation_member>) { return json::object{{"id", v.id}, {"username", v.username}, {"role", role(v.role)}}; }
    else if constexpr (std::same_as<T, chat::read_position>) { return json::object{{"user", v.user}, {"message", v.message}}; }
    else if constexpr (std::same_as<T, chat::reaction>) { return json::object{{"emoji", v.emoji}, {"users", dto(v.users)}}; }
    else if constexpr (std::same_as<T, chat::mention>) { return json::object{{"user", v.user}, {"username", v.username}}; }
    else if constexpr (std::same_as<T, chat::quoted_message>)
    { return json::object{{"id", v.id}, {"from", v.from}, {"username", v.username}, {"text", v.text}, {"deleted", v.deleted}, {"edited_at", dto(v.edited_at)}}; }
    else if constexpr (std::same_as<T, chat::attachment_info>)
    { return json::object{{"filename", v.filename}, {"media_type", v.media_type}, {"size", v.size}}; }
    else if constexpr (std::same_as<T, chat::message>)
    {
        return json::object{{"id", v.id}, {"conversation", v.conversation}, {"from", v.from}, {"username", v.username},
            {"text", v.text}, {"timestamp", v.timestamp}, {"deleted", v.deleted}, {"edited_at", dto(v.edited_at)},
            {"reply", dto(v.reply)}, {"attachment", dto(v.attachment)}, {"mentions", dto(v.mentions)},
            {"reaction_revision", v.reaction_revision}, {"reactions", dto(v.reactions)}};
    }
    else if constexpr (std::same_as<T, chat::send_message_result>)
    { return json::object{{"message_id", v.message_id}, {"timestamp", v.timestamp}, {"realtime", v.realtime}, {"reply", dto(v.reply)}, {"mentions", dto(v.mentions)}}; }
    else if constexpr (std::same_as<T, chat::reaction_update>)
    { return json::object{{"conversation", v.conversation}, {"message", v.message}, {"revision", v.revision}, {"reactions", dto(v.reactions)}}; }
    else if constexpr (std::same_as<T, chat::messages_result>)
    { return json::object{{"messages", dto(v.messages)}, {"read_positions", dto(v.read_positions)}, {"has_more", v.has_more}}; }
    else if constexpr (std::same_as<T, chat::conversation_cursor>)
    { return json::object{{"activity", v.activity}, {"id", v.id}, {"pinned", v.pinned}}; }
    else if constexpr (std::same_as<T, chat::conversation>)
    {
        return json::object{{"id", v.id}, {"kind", v.kind == chat::conversation_kind::group ? "group" : "direct"},
            {"user", v.user}, {"username", v.username}, {"member_count", v.member_count}, {"unread", v.unread},
            {"muted", v.muted}, {"pinned", v.pinned}, {"can_send", v.can_send}, {"last", dto(v.last)},
            {"announcement", v.announcement}, {"join_approval", v.join_approval}, {"pinned_message", dto(v.pinned_message)}};
    }
    else if constexpr (std::same_as<T, chat::conversations_result>)
    { return json::object{{"conversations", dto(v.conversations)}, {"next", dto(v.next)}}; }
    else if constexpr (std::same_as<T, chat::direct_conversation_result>)
    { return json::object{{"conversation", v.conversation}, {"can_send", v.can_send}}; }
    else if constexpr (std::same_as<T, chat::group_join_result>)
    {
        return json::object{{"conversation", v.conversation}, {"title", v.title}, {"member_count", v.member_count},
            {"state", v.state == chat::group_join_state::pending ? "pending" : v.state == chat::group_join_state::joined ? "joined" : "member"}};
    }
    else if constexpr (std::same_as<T, chat::group_join_request>)
    { return json::object{{"applicant", dto(v.applicant)}, {"created_at", v.created_at}}; }
    else if constexpr (std::same_as<T, chat::group_join_requests_result>)
    { return json::object{{"requests", dto(v.requests)}, {"next", dto(v.next)}}; }
    else if constexpr (requires { v.has_value(); *v; }) { return v ? dto(*v) : json::value(nullptr); }
    else if constexpr (requires { v.begin(); v.end(); })
    {
        json::array result;
        for (auto const& item : v) { result.push_back(dto(item)); }
        return result;
    }
    else { static_assert(sizeof(T) == 0, "Missing diagnostic DTO serializer"); }
}
struct sdk_error : std::runtime_error
{
    int code;
    sdk_error(chat::error const& value) : std::runtime_error(value.message), code(value.code) {}
};
template<class T, class F> T rpc(F operation)
{
    auto result = std::make_shared<std::promise<std::expected<T, chat::error>>>();
    auto future = result->get_future();
    operation([result](auto value) { result->set_value(std::move(value)); });
    require(future.wait_for(15s) == std::future_status::ready, "SDK RPC timeout");
    auto value = future.get();
    if (!value) { throw sdk_error(value.error()); }
    return std::move(*value);
}
template<class T, class F> json::value measured(F operation)
{
    auto start = clock_type::now();
    try
    {
        auto value = rpc<T>(std::move(operation));
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock_type::now() - start).count();
        return json::object{{"ok", true}, {"value", dto(value)}, {"elapsed_us", elapsed}, {"measurement", "SDK request to callback"}};
    }
    catch (sdk_error const& error)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock_type::now() - start).count();
        return json::object{{"ok", false}, {"error", json::object{{"code", error.code}, {"message", error.what()}}},
            {"elapsed_us", elapsed}, {"measurement", "SDK request to callback"}};
    }
}
// Fixture preparation only: the map is complete before these workers start.
// Every actor belongs to one worker; all futures are joined even if a worker fails.
template<class F> void parallel_preparation(std::size_t count, F operation)
{
    auto workers = std::min<std::size_t>(8, count);
    std::vector<std::future<void>> pending;
    for (std::size_t worker = 0; worker < workers; ++worker)
    {
        pending.push_back(std::async(std::launch::async, [&, worker, workers] { operation(worker, workers); }));
    }
    std::exception_ptr failed;
    for (auto& future : pending)
    {
        try { future.get(); }
        catch (...) { if (!failed) { failed = std::current_exception(); } }
    }
    if (failed) { std::rethrow_exception(failed); }
}

std::int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count();
}
struct event_counts
{
    std::map<std::int64_t, std::uint64_t> messages;
    std::uint64_t updates = 0, reactions = 0, read = 0, typing_true = 0, typing_false = 0;
    std::int64_t first_ns = 0, last_ns = 0, reaction_revision = 0;
};
struct event_log
{
    std::mutex mutex;
    std::condition_variable changed;
    bool enabled = false;
    std::int64_t conversation = 0, message = 0, started_ns = 0;
    std::string marker;
    std::map<std::string, event_counts> actors;
    void arrival(event_counts& value)
    {
        value.last_ns = now_ns();
        if (!value.first_ns) { value.first_ns = value.last_ns; }
    }
    void incoming(std::string const& alias, chat::message const& value, bool updated)
    {
        std::lock_guard lock(mutex);
        if (!enabled || value.conversation != conversation) { return; }
        if (message && value.id != message) { return; }
        if (!message && !marker.empty() && value.text.find(marker) == std::string::npos) { return; }
        if (!message && !marker.empty()) { message = value.id; }
        auto& count = actors[alias];
        if (updated) { ++count.updates; } else { ++count.messages[value.id]; }
        arrival(count);
        changed.notify_all();
    }
    void reaction(std::string const& alias, chat::reaction_update const& value)
    {
        std::lock_guard lock(mutex);
        if (!enabled || value.conversation != conversation || (message && value.message != message)) { return; }
        auto& count = actors[alias];
        ++count.reactions;
        count.reaction_revision = std::max(count.reaction_revision, value.revision);
        arrival(count);
        changed.notify_all();
    }
    void typing(std::string const& alias, chat::typing_event const& value)
    {
        std::lock_guard lock(mutex);
        if (!enabled || value.conversation != conversation) { return; }
        auto& count = actors[alias];
        if (value.typing) { ++count.typing_true; } else { ++count.typing_false; }
        arrival(count);
        changed.notify_all();
    }
    void read(std::string const& alias, std::int64_t room, std::int64_t read_message)
    {
        std::lock_guard lock(mutex);
        if (!enabled || room != conversation || (message && read_message != message)) { return; }
        auto& count = actors[alias];
        ++count.read;
        arrival(count);
        changed.notify_all();
    }
};
struct account
{
    std::string alias, username;
    std::int64_t id = 0;
    std::shared_ptr<event_log> events;
    std::mutex mutex;
    std::condition_variable changed;
    bool connected = false;
    bool authenticated = false; // Control thread only.
    std::string connection_error;
    std::unique_ptr<chat::client> client; // Destroyed before callback targets.

    account(std::string label, std::string name, std::shared_ptr<event_log> log)
        : alias(std::move(label)), username(std::move(name)), events(std::move(log)) {}
    ~account() { close(); }
    void close()
    {
        if (client) { client->close(); client.reset(); }
        authenticated = false;
        std::lock_guard lock(mutex);
        connected = false;
    }
    void connect(std::string const& url)
    {
        close();
        { std::lock_guard lock(mutex); connection_error.clear(); }
        client = std::make_unique<chat::client>();
        client->set_connected_handler([this] {
            { std::lock_guard lock(mutex); connected = true; }
            changed.notify_all();
        });
        client->set_disconnected_handler([this] {
            { std::lock_guard lock(mutex); connected = false; }
            changed.notify_all();
        });
        client->set_error_handler([this](auto const& error) {
            { std::lock_guard lock(mutex); connection_error = error.message; }
            changed.notify_all();
        });
        client->set_message_handler([this](auto value) { events->incoming(alias, value, false); });
        client->set_message_updated_handler([this](auto value) { events->incoming(alias, value, true); });
        client->set_reaction_handler([this](auto value) { events->reaction(alias, value); });
        client->set_typing_handler([this](auto value) { events->typing(alias, value); });
        client->set_read_handler([this](auto conversation, auto, auto message) { events->read(alias, conversation, message); });
        client->set_presence_handler([this](auto) { events->changed.notify_all(); });
        client->connect(url);
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 15s, [&] { return connected || !connection_error.empty(); }), "Connect timeout: " + alias);
        require(connected, "Connect failed " + alias + ": " + connection_error);
    }
    json::object description() const { return {{"alias", alias}, {"id", id}, {"username", username}}; }
};
class fixture
{
    std::shared_ptr<event_log> events_ = std::make_shared<event_log>();
    std::map<std::string, std::unique_ptr<account>> accounts_;
    std::string url_, prefix_, password_;
    std::int64_t group_ = 0;
    json::object manifest_;
    bool configured_ = false, seeded_ = false, measured_sizes_ = false;
    account& actor(std::string const& alias)
    {
        auto found = accounts_.find(alias);
        require(found != accounts_.end(), "Unknown fixture actor: " + alias);
        return *found->second;
    }
    chat::client& connected(std::string const& alias)
    {
        auto& value = actor(alias);
        require(value.client && value.authenticated, "Actor is not authenticated: " + alias);
        return *value.client;
    }
    void connect_actor(std::string const& alias)
    {
        auto& value = actor(alias);
        require(!value.authenticated, "Actor already connected: " + alias);
        value.connect(url_);
        try
        {
            auto identity = rpc<chat::authentication_result>([&](auto done) { value.client->authenticate(value.username, password_, done); });
            require(identity.authenticated && identity.user == value.id, "Authentication identity mismatch: " + alias);
            value.authenticated = true;
        }
        catch (...) { value.close(); throw; }
    }
    void disconnect_actor(std::string const& alias)
    {
        auto id = actor(alias).id;
        actor(alias).close();
        // A closed SDK socket is not proof the server processed logout yet.
        // For real TUI handoff, wait for the observer's authoritative presence.
        auto observer = accounts_.find("S005");
        if (alias == "S005" || observer == accounts_.end() || !observer->second->authenticated) { return; }
        auto until = clock_type::now() + 15s;
        for (;;)
        {
            auto snapshot = rpc<std::vector<chat::presence>>([&](auto done) { observer->second->client->get_presence(done); });
            auto found = std::ranges::find(snapshot, id, &chat::presence::user);
            if (found == snapshot.end() || !found->online) { return; }
            require(clock_type::now() < until, "Server did not release actor: " + alias);
            std::unique_lock lock(events_->mutex);
            events_->changed.wait_for(lock, 50ms);
        }
    }
    void add(std::string alias, std::string name)
    {
        require(chat::valid_username(name), "Generated username is invalid");
        auto key = alias;
        accounts_.emplace(std::move(key), std::make_unique<account>(std::move(alias), std::move(name), events_));
    }
    std::vector<chat::conversation_member> members()
    {
        return rpc<std::vector<chat::conversation_member>>([&](auto done) { connected("S005").get_members(group_, done); });
    }
    std::vector<std::string> aliases(json::object const& input)
    {
        auto p = input.if_contains("actors");
        require(p && p->is_array(), "actors must be an array of fixture aliases");
        std::vector<std::string> result;
        for (auto const& value : p->as_array())
        {
            require(value.is_string(), "Actor alias must be a string");
            result.emplace_back(value.as_string());
            actor(result.back());
        }
        return result;
    }
    std::vector<std::int64_t> ids(json::object const& input, std::string_view field)
    {
        std::vector<std::int64_t> result;
        auto p = input.if_contains(field);
        require(p && p->is_array(), std::string(field) + " must be an array");
        for (auto const& value : p->as_array())
        {
            if (value.is_string()) { result.push_back(actor(std::string(value.as_string())).id); }
            else { require(value.is_int64(), "Member must be an id or fixture alias"); result.push_back(value.as_int64()); }
        }
        return result;
    }
    std::int64_t user(json::object const& input)
    {
        auto p = input.if_contains("user");
        require(p != nullptr, "user is required");
        return p->is_string() ? actor(std::string(p->as_string())).id : number(input, "user");
    }

    void befriend(std::string const& first, std::string const& second)
    {
        auto contacts = rpc<std::vector<chat::user>>([&](auto done) { connected(first).get_contacts(done); });
        if (std::ranges::find(contacts, actor(second).id, &chat::user::id) != contacts.end()) { return; }
        auto request = rpc<chat::friendship_result>([&](auto done) { connected(first).send_friend_request(actor(second).id, done); });
        auto accepted = request.state == chat::friendship_state::incoming_pending
            ? rpc<chat::friendship_result>([&](auto done) { connected(first).respond_friend_request(actor(second).id, true, done); })
            : rpc<chat::friendship_result>([&](auto done) { connected(second).respond_friend_request(actor(first).id, true, done); });
        require(accepted.state == chat::friendship_state::accepted, "Friendship was not confirmed");
    }

public:
    ~fixture() { close(); }
    void close()
    {
        for (auto& [name, value] : accounts_) { (void)name; value->close(); }
    }
    json::value seed_navigation()
    {
        require(configured_ && !seeded_, "configure once before seed; never reuse a partial seed");
        seeded_ = true;
        for (auto const& [alias, name] : std::vector<std::pair<std::string, std::string>>{
                 {"A", "导航A_"}, {"B", "历史B_"}, {"C", "好友C_"}, {"D", "申请D_"},
                 {"E", "等待E_"}, {"S005", "观察S005_"}})
        { add(alias, name + prefix_); }
        {
            account registrar("registrar", "", events_);
            registrar.connect(url_);
            for (auto const& [alias, value] : accounts_)
            {
                (void)alias;
                value->id = rpc<std::int64_t>([&](auto done) {
                    registrar.client->register_user(value->username, password_, done);
                });
            }
        }
        for (auto const& [alias, value] : accounts_) { (void)value; connect_actor(alias); }
        for (auto const* alias : {"A", "B", "C", "D", "E"}) { befriend("S005", alias); }
        befriend("A", "B");
        befriend("A", "C");
        auto title = "导航测试群_" + prefix_;
        group_ = rpc<std::int64_t>([&](auto done) {
            connected("A").create_group(title, {actor("B").id, actor("C").id, actor("S005").id}, done);
        });
        auto invite = rpc<std::optional<std::string>>([&](auto done) { connected("A").create_group_invite(group_, done); });
        require(invite.has_value(), "Navigation group invite missing");
        auto direct = rpc<chat::direct_conversation_result>([&](auto done) {
            connected("A").open_direct_conversation(actor("B").id, done);
        });
        auto marker = "readonly_history_" + prefix_;
        auto message = rpc<chat::send_message_result>([&](auto done) {
            connected("A").send_message(direct.conversation, marker, done);
        });
        rpc<bool>([&](auto done) { connected("A").remove_contact(actor("B").id, done); });
        auto incoming = rpc<chat::friendship_result>([&](auto done) {
            connected("D").send_friend_request(actor("A").id, done);
        });
        auto outgoing = rpc<chat::friendship_result>([&](auto done) {
            connected("A").send_friend_request(actor("E").id, done);
        });
        auto contacts = rpc<std::vector<chat::user>>([&](auto done) { connected("A").get_contacts(done); });
        auto requests = rpc<chat::friend_requests_result>([&](auto done) { connected("A").get_friend_requests(done); });
        require(contacts.size() == 2 && requests.incoming.size() == 1 && requests.outgoing.size() == 1 &&
                incoming.state == chat::friendship_state::outgoing_pending &&
                outgoing.state == chat::friendship_state::outgoing_pending, "Navigation fixture relationships mismatch");
        json::object actors;
        for (auto const* alias : {"A", "B", "C", "D", "E"}) { actors[alias] = actor(alias).description(); }
        manifest_ = {{"prefix", prefix_}, {"url", url_}, {"actors", std::move(actors)},
            {"sdk", json::array{actor("S005").description()}}, {"group", json::object{{"id", group_}, {"title", title}}},
            {"invite_token", *invite}, {"navigation", json::object{{"readonly_conversation", direct.conversation},
                {"history_message", message.message_id}, {"history_marker", marker},
                {"accepted", json::array{"C", "S005"}}, {"incoming", "D"}, {"outgoing", "E"}}}};
        for (auto const* alias : {"A", "B", "C", "D", "E"}) { disconnect_actor(alias); }
        return manifest_;
    }
    json::value seed(json::object const& input)
    {
        require(configured_ && !seeded_, "configure once before seed; never reuse a partial seed");
        seeded_ = true; // A failed seed is cleaned by dropping the dedicated test database.
        int message_count = static_cast<int>(number(input, "messages", 150));
        int search_count = static_cast<int>(number(input, "search_matches", 120));
        int request_count = static_cast<int>(number(input, "pending", 59));
        int conversation_count = static_cast<int>(number(input, "conversations", 65));
        require(number(input, "members", 100) == 100 && message_count >= 150 && search_count >= 120 &&
            search_count <= message_count && request_count >= 0 && request_count <= 100 &&
            conversation_count > 60 && conversation_count <= 100, "Invalid 100-member fixture dimensions");
        require(message_count <= 1000, "Fixture message count exceeds explicit diagnostic limit");
        add("A", "群主A_" + prefix_);
        add("B", "管理B_" + prefix_);
        add("C", "张 三C_" + prefix_);
        add("D", "成员D_" + prefix_);
        add("E", "Alice Bob E_" + prefix_);
        auto padded = [](char letter, int n) {
            std::ostringstream out; out << letter << std::setfill('0') << std::setw(3) << n; return out.str();
        };
        for (int i = 5; i <= 100; ++i) { auto name = padded('S', i); add(name, (i == 10 ? "Dot.Name-User_" : name + "_") + prefix_); }
        for (int i = 1; i <= request_count; ++i) { auto name = padded('R', i); add(name, name + "_" + prefix_); }

        for (int i = 1; i <= 10; ++i) { auto name = padded('F', i); add(name, name + "_" + prefix_); }

        auto& owner = actor("A");
        owner.connect(url_);
        std::vector<account*> registrations;
        for (auto const& [alias, value] : accounts_) { (void)alias; registrations.push_back(value.get()); }
        parallel_preparation(registrations.size(), [&](std::size_t worker, std::size_t workers) {
            account registrar("registrar" + std::to_string(worker), "", events_);
            registrar.connect(url_);
            for (std::size_t index = worker; index < registrations.size(); index += workers)
            {
                auto& value = *registrations[index];
                value.id = rpc<std::int64_t>([&](auto done) {
                    registrar.client->register_user(value.username, password_, done);
                });
            }
        });
        auto authenticated = rpc<chat::authentication_result>([&](auto done) { owner.client->authenticate(owner.username, password_, done); });
        require(authenticated.authenticated && authenticated.user == owner.id, "Owner authentication failed");
        owner.authenticated = true;
        // Authenticate fixture accounts before any handshake. The five TUI actors are
        // handed off only after all observer friendships have explicit acceptance.
        std::vector<std::string> authentication;
        for (auto const& [alias, value] : accounts_)
        {
            (void)value;
            if (alias != "A" && !alias.starts_with('R') && !alias.starts_with('F')) { authentication.push_back(alias); }
        }
        parallel_preparation(authentication.size(), [&](std::size_t worker, std::size_t workers) {
            for (std::size_t index = worker; index < authentication.size(); index += workers)
            { connect_actor(authentication[index]); }
        });
        std::vector<std::int64_t> initial_members;
        std::vector<std::string> member_aliases;
        json::array owner_friends;
        for (auto const& [alias, account] : accounts_)
        {
            if (alias == "A" || alias == "E" || alias.starts_with('R') || alias.starts_with('F')) { continue; }
            member_aliases.push_back(alias);
            if (initial_members.size() < 20)
            {
                befriend("A", alias);
                initial_members.push_back(account->id);
                owner_friends.push_back(json::string(alias));
            }
        }
        require(member_aliases.size() == 99 && initial_members.size() == 20, "Invalid initial member/friend counts");
        auto title = "百人验证_" + prefix_;
        group_ = rpc<std::int64_t>([&](auto done) { owner.client->create_group(title, initial_members, done); });
        auto invite = rpc<std::optional<std::string>>([&](auto done) { owner.client->create_group_invite(group_, done); });
        require(invite.has_value(), "Group invite missing");
        for (auto const& alias : member_aliases)
        {
            if (std::ranges::find(initial_members, actor(alias).id) != initial_members.end()) { continue; }
            auto joined = rpc<chat::group_join_result>([&](auto done) { connected(alias).join_group(*invite, done); });
            require(joined.state == chat::group_join_state::joined, "Public-link group join failed");
        }
        for (auto const* alias : {"B", "S005", "S006"})
        { rpc<bool>([&](auto done) { owner.client->set_group_admin(group_, actor(alias).id, true, done); }); }
        auto admin_limit = measured<bool>([&](auto done) {
            owner.client->set_group_admin(group_, actor("C").id, true, done);
        }).as_object();
        require(!admin_limit.at("ok").as_bool() &&
            std::string(admin_limit.at("error").as_object().at("message").as_string()).find("three administrators") != std::string::npos,
            "Server must reject an actual fourth-administrator request");
        auto limited_roles = members();
        auto owner_count = std::ranges::count(limited_roles, chat::member_role::owner, &chat::conversation_member::role);
        auto admin_count = std::ranges::count(limited_roles, chat::member_role::admin, &chat::conversation_member::role);
        auto ordinary_count = std::ranges::count(limited_roles, chat::member_role::member, &chat::conversation_member::role);
        require(limited_roles.size() == 100 && owner_count == 1 && admin_count == 3 && ordinary_count == 96,
                "Rejected fourth administrator must preserve the authoritative 1/3/96 roles");
        json::object admin_limit_evidence{{"attempted_user", actor("C").id}, {"rpc", std::move(admin_limit)},
            {"members", limited_roles.size()}, {"owners", owner_count}, {"admins", admin_count}, {"ordinary_members", ordinary_count}};

        json::array conversations{json::object{{"id", group_}, {"title", title}, {"pinned", true}}};
        rpc<bool>([&](auto done) { owner.client->set_conversation_pinned(group_, true, done); });
        for (int i = 0; i < conversation_count - 1; ++i)
        {
            std::int64_t id;
            std::string name;
            if (i < 20)
            {
                auto& peer = actor(member_aliases[static_cast<std::size_t>(i)]);
                auto direct = rpc<chat::direct_conversation_result>([&](auto done) { owner.client->open_direct_conversation(peer.id, done); });
                id = direct.conversation;
                name = peer.username;
            }
            else
            {
                name = "分页群_" + std::to_string(i + 1) + "_" + prefix_;
                id = rpc<std::int64_t>([&](auto done) { owner.client->create_group(name, {actor("S005").id}, done); });
            }
            bool pinned = i < 2;
            if (pinned) { rpc<bool>([&](auto done) { owner.client->set_conversation_pinned(id, true, done); }); }
            conversations.push_back(json::object{{"id", id}, {"title", name}, {"pinned", pinned}});
        }
        // Presence observer remains an ordinary account: every subscription is accepted
        // by the other party. Owner already includes this observer in its twenty friends.
        for (auto const& [alias, account] : accounts_)
        {
            (void)account;
            if (alias == "S005" || alias.starts_with('R') || alias.starts_with('F')) { continue; }
            befriend("S005", alias);
        }
        json::array friend_pairs;
        for (int i = 30; i < 50; i += 2)
        {
            auto first = padded('S', i), second = padded('S', i + 1);
            befriend(first, second);
            auto direct = rpc<chat::direct_conversation_result>([&](auto done) {
                connected(first).open_direct_conversation(actor(second).id, done);
            });
            friend_pairs.push_back(json::object{{"actors", json::array{json::string(first), json::string(second)}},
                {"conversation", direct.conversation}});
        }
        json::array friend_incoming, friend_outgoing;
        for (int i = 1; i <= 10; ++i)
        {
            auto alias = padded('F', i);
            connect_actor(alias);
            auto& sender = i <= 5 ? connected(alias) : *owner.client;
            auto target = i <= 5 ? owner.id : actor(alias).id;
            auto requested = rpc<chat::friendship_result>([&](auto done) { sender.send_friend_request(target, done); });
            require(requested.state == chat::friendship_state::outgoing_pending, "Friend request must remain pending");
            (i <= 5 ? friend_incoming : friend_outgoing).push_back(actor(alias).description());
            actor(alias).close();
        }
        auto confirmed = rpc<std::vector<chat::user>>([&](auto done) { owner.client->get_contacts(done); });
        require(confirmed.size() == 20, "Owner must have exactly twenty confirmed friends");
        auto search_query = "find_" + prefix_;
        json::array messages;
        for (int i = 0; i < message_count; ++i)
        {
            auto marker = (i < search_count ? search_query : "other_" + prefix_) + "_" + std::to_string(i + 1);
            auto value = rpc<chat::send_message_result>([&](auto done) {
                owner.client->send_message(group_, marker + " 中文群消息 @" + actor("C").username, done);
            });
            messages.push_back(json::object{{"id", value.message_id}, {"marker", marker}});
        }
        rpc<bool>([&](auto done) { owner.client->set_group_join_approval(group_, true, done); });
        json::array pending;
        for (auto const& [alias, account] : accounts_)
        {
            if (!alias.starts_with('R')) { continue; }
            connect_actor(alias);
            auto result = rpc<chat::group_join_result>([&](auto done) { account->client->join_group(*invite, done); });
            require(result.state == chat::group_join_state::pending, "Applicant did not remain pending");
            account->close();
            pending.push_back(account->description());
        }
        json::object actors;
        for (auto const* alias : {"A", "B", "C", "D", "E"}) { actors[alias] = actor(alias).description(); }
        json::array sdk;
        for (auto const& [alias, account] : accounts_) { if (alias.starts_with('S')) { sdk.push_back(account->description()); } }
        manifest_ = json::object{{"prefix", prefix_}, {"url", url_}, {"group", json::object{{"id", group_}, {"title", title}}},
            {"actors", std::move(actors)}, {"sdk", std::move(sdk)}, {"reserved", actor("S100").description()},
            {"admins", json::array{"B", "S005", "S006"}}, {"admin_limit_rejection", std::move(admin_limit_evidence)}, {"messages", std::move(messages)},
            {"search_query", search_query}, {"conversations", std::move(conversations)},
            {"pending", std::move(pending)}, {"invite_token", *invite}, {"owner_friends", std::move(owner_friends)},
            {"friend_pairs", std::move(friend_pairs)},
            {"friend_pending", json::object{{"incoming", std::move(friend_incoming)}, {"outgoing", std::move(friend_outgoing)}}}};
        auto snapshot = members();
        require(snapshot.size() == 100 &&
            std::ranges::count(snapshot, chat::member_role::owner, &chat::conversation_member::role) == 1 &&
            std::ranges::count(snapshot, chat::member_role::admin, &chat::conversation_member::role) == 3,
            "Authoritative seeded group shape mismatch");
        for (auto const* alias : {"A", "B", "C", "D", "E"}) { disconnect_actor(alias); }
        return manifest_;
    }
    json::value exercise_friend_pairs()
    {
        json::array evidence;
        for (auto const& value : manifest_.at("friend_pairs").as_array())
        {
            auto const& pair = value.as_object();
            auto first = std::string(pair.at("actors").as_array()[0].as_string());
            auto second = std::string(pair.at("actors").as_array()[1].as_string());
            auto room = pair.at("conversation").as_int64();
            json::array directions;
            for (int direction = 0; direction != 2; ++direction)
            {
                auto const& sender = direction == 0 ? first : second;
                auto const& recipient = direction == 0 ? second : first;
                auto message = rpc<chat::send_message_result>([&](auto done) {
                    connected(sender).send_message(room, "好友双向验证 " + sender + " → " + recipient, done);
                });
                auto edited = rpc<chat::message>([&](auto done) {
                    connected(sender).edit_message(room, message.message_id, "已编辑的好友消息 " + sender, done);
                });
                require(edited.edited_at.has_value(), "Friend edit missing authoritative revision");
                auto reply = rpc<chat::send_message_result>([&](auto done) {
                    connected(recipient).send_message(room, "回复 " + sender, done, message.message_id);
                });
                require(reply.reply && reply.reply->id == message.message_id, "Friend reply missing quote");
                auto reaction = rpc<chat::reaction_update>([&](auto done) {
                    connected(recipient).set_message_reaction(room, message.message_id, "👍", done);
                });
                require(reaction.revision > 0, "Friend reaction missing revision");
                auto typed = rpc<bool>([&](auto done) { connected(sender).set_typing(room, true, done); });
                require(typed, "Online confirmed friend did not receive typing");
                rpc<bool>([&](auto done) { connected(sender).set_typing(room, false, done); });
                auto bytes = "attachment:" + sender + ":" + prefix_;
                auto file = rpc<chat::message>([&](auto done) {
                    connected(sender).send_attachment(room, "friend-" + sender + ".txt", bytes, done);
                });
                auto downloaded = rpc<std::string>([&](auto done) { connected(recipient).get_attachment(room, file.id, done); });
                require(downloaded == bytes, "Friend attachment roundtrip mismatch");
                rpc<std::int64_t>([&](auto done) { connected(recipient).mark_read(room, file.id, done); });
                auto history = rpc<chat::messages_result>([&](auto done) { connected(sender).get_messages(room, {}, done); });
                require(std::ranges::any_of(history.read_positions, [&](auto const& read) {
                    return read.user == actor(recipient).id && read.message >= file.id;
                }), "Friend read position did not persist");
                auto deleted = rpc<chat::message>([&](auto done) { connected(sender).delete_message(room, message.message_id, done); });
                require(deleted.deleted, "Friend delete missing tombstone");
                directions.push_back(json::object{{"sender", sender}, {"recipient", recipient},
                    {"message", message.message_id}, {"reply", reply.message_id}, {"attachment", file.id},
                    {"reaction_revision", reaction.revision}, {"read", file.id}, {"deleted", true}});
            }
            evidence.push_back(json::object{{"conversation", room}, {"directions", std::move(directions)}});
        }
        auto const& pair = manifest_.at("friend_pairs").as_array().front().as_object();
        auto first = std::string(pair.at("actors").as_array()[0].as_string());
        auto second = std::string(pair.at("actors").as_array()[1].as_string());
        auto room = pair.at("conversation").as_int64();
        disconnect_actor(second);
        auto offline = rpc<chat::send_message_result>([&](auto done) {
            connected(first).send_message(room, "offline_delivery_" + prefix_, done);
        });
        require(!offline.realtime, "Offline friend unexpectedly reported realtime delivery");
        connect_actor(second);
        auto recovered = rpc<chat::messages_result>([&](auto done) { connected(second).get_messages(room, {}, done); });
        require(std::ranges::find(recovered.messages, offline.message_id, &chat::message::id) != recovered.messages.end(),
                "Offline friend message missing after reconnect");
        return json::object{{"pairs", std::move(evidence)}, {"pair_count", 10}, {"directions", 20},
            {"offline", json::object{{"sender", first}, {"recipient", second}, {"message", offline.message_id},
                {"realtime", offline.realtime}, {"recovered", true}}}};
    }

    json::value measure_sizes()
    {
        require(!measured_sizes_, "Size measurements may be prepared only once per fixture");
        measured_sizes_ = true;
        auto& owner = connected("S005");
        std::vector<std::string> targets;
        // The main hundred-member group and the five real TUI actors are untouched.
        // Reuse 94 connected SDK identities, then register 105 temporary identities.
        for (int i = 6; i <= 99; ++i)
        {
            std::ostringstream name;
            name << "S" << std::setfill('0') << std::setw(3) << i;
            connected(name.str());
            targets.push_back(name.str());
        }
        struct measured_group
        {
            int size;
            std::int64_t id;
            std::string token;
        };
        std::vector<measured_group> groups;
        for (int size : {3, 10, 50, 200})
        {
            auto room = rpc<std::int64_t>([&](auto done) {
                owner.create_group("规模测量_" + std::to_string(size) + "_" + prefix_, {actor("S006").id}, done);
            });
            auto token = rpc<std::optional<std::string>>([&](auto done) { owner.create_group_invite(room, done); });
            require(token.has_value(), "Measurement invite missing");
            groups.push_back({size, room, *token});
        }
        // Existing confirmed friend S006 was the initial member. All others join
        // through the ordinary SDK invite-link flow, without additional friendships.
        for (std::size_t i = 1; i < targets.size(); ++i)
        {
            for (auto const& group : groups)
            {
                if (i >= static_cast<std::size_t>(group.size - 1)) { continue; }
                auto joined = rpc<chat::group_join_result>([&](auto done) { connected(targets[i]).join_group(group.token, done); });
                require(joined.state == chat::group_join_state::joined, "Measurement group join failed");
            }
        }
        for (int i = 1; i <= 105; ++i)
        {
            std::ostringstream name;
            name << "M" << std::setfill('0') << std::setw(3) << i;
            auto alias = name.str();
            add(alias, alias + "_" + prefix_);
            auto& temporary = actor(alias);
            temporary.id = rpc<std::int64_t>([&](auto done) { owner.register_user(temporary.username, password_, done); });
            connect_actor(alias);
            auto joined = rpc<chat::group_join_result>([&](auto done) { connected(alias).join_group(groups.back().token, done); });
            require(joined.state == chat::group_join_state::joined, "Temporary measurement member did not join");
            temporary.close();
        }
        json::array results;
        for (auto const& group : groups)
        {
            json::array samples;
            double members_ms = 0, history_ms = 0, send_ms = 0;
            for (int sample = 0; sample < 3; ++sample)
            {
                auto start = clock_type::now();
                auto listed = rpc<std::vector<chat::conversation_member>>([&](auto done) { owner.get_members(group.id, done); });
                auto members_done = clock_type::now();
                auto history = rpc<chat::messages_result>([&](auto done) { owner.get_messages(group.id, {}, done); });
                auto history_done = clock_type::now();
                auto sent = rpc<chat::send_message_result>([&](auto done) { owner.send_message(group.id, "规模样本", done); });
                auto send_done = clock_type::now();
                require(listed.size() == static_cast<std::size_t>(group.size) &&
                    history.read_positions.size() == static_cast<std::size_t>(group.size) && sent.message_id > 0,
                    "Member/read/publish paths must remain complete at measured size");
                auto members_elapsed = std::chrono::duration<double, std::milli>(members_done - start).count();
                auto history_elapsed = std::chrono::duration<double, std::milli>(history_done - members_done).count();
                auto send_elapsed = std::chrono::duration<double, std::milli>(send_done - history_done).count();
                members_ms += members_elapsed / 3;
                history_ms += history_elapsed / 3;
                send_ms += send_elapsed / 3;
                samples.push_back(json::object{{"members", listed.size()}, {"read_positions", history.read_positions.size()},
                    {"message", sent.message_id}, {"get_members_ms", members_elapsed},
                    {"history_ms", history_elapsed}, {"send_ms", send_elapsed}});
            }
            results.push_back(json::object{{"size", group.size}, {"conversation", group.id}, {"samples", std::move(samples)},
                {"get_members_ms", members_ms}, {"history_ms", history_ms}, {"send_ms", send_ms}});
        }
        return json::object{{"measurement", "SDK request to callback, arithmetic mean of three samples"},
            {"sizes", std::move(results)}, {"temporary_accounts_registered", 105},
            {"preparation", "SDK register/authenticate, existing accepted initial friend and public invite links"}};
    }

    json::value status(bool verify)
    {
        json::array online;
        for (auto const& [alias, value] : accounts_)
        {
            std::lock_guard lock(value->mutex);
            if (value->connected && value->authenticated) { online.push_back(json::string(alias)); }
        }
        json::object result{{"sdk_online", std::move(online)}, {"sdk_online_count", 0}};
        result["sdk_online_count"] = result["sdk_online"].as_array().size();
        if (verify)
        {
            auto people = members();
            auto presences = rpc<std::vector<chat::presence>>([&](auto done) { connected("S005").get_presence(done); });
            json::array present, absent;
            for (auto const& person : people)
            {
                auto found = std::ranges::find(presences, person.id, &chat::presence::user);
                if (person.id == actor("S005").id || (found != presences.end() && found->online)) { present.push_back(person.id); }
                else { absent.push_back(person.id); }
            }
            result["members"] = dto(people);
            result["member_count"] = people.size();
            result["owner_count"] = std::ranges::count(people, chat::member_role::owner, &chat::conversation_member::role);
            result["admin_count"] = std::ranges::count(people, chat::member_role::admin, &chat::conversation_member::role);
            result["online_member_ids"] = present;
            result["online_member_count"] = present.size();
            result["offline_member_ids"] = absent;
        }
        return result;
    }
    json::value read_snapshot(std::int64_t message)
    {
        auto snapshot = rpc<chat::messages_result>([&](auto done) { connected("S005").get_messages(group_, {}, done); });
        auto current = members();
        json::array readers;
        for (auto const& position : snapshot.read_positions)
        {
            if (position.user != actor("A").id && position.message >= message &&
                std::ranges::find(current, position.user, &chat::conversation_member::id) != current.end())
            { readers.push_back(position.user); }
        }
        return json::object{{"message", message}, {"count", readers.size()}, {"readers", std::move(readers)},
            {"read_positions", dto(snapshot.read_positions)}};
    }
    json::value read(json::object const& input)
    {
        auto const message = number(input, "message");
        auto const target = number(input, "target_count");
        require(message > 0 && (target == 0 || target == 1 || target == 10 || target == 50 || target == 99), "Read target must be 0/1/10/50/99");
        auto current = members();
        std::vector<account*> candidates;
        // Stable cumulative sequence: SDK identities first, real TUI identities last.
        for (bool sdk : {true, false})
        {
            for (auto const& [alias, value] : accounts_)
            {
                if (alias == "A" || alias.starts_with('S') != sdk || alias.starts_with('R')) { continue; }
                if (std::ranges::find(current, value->id, &chat::conversation_member::id) != current.end())
                { candidates.push_back(value.get()); }
            }
        }
        require(candidates.size() == 99, "Read matrix requires exactly 100 current group members");
        json::array needs_tui;
        std::int64_t applied = 0;
        for (std::int64_t i = 0; i < target; ++i)
        {
            auto* value = candidates[static_cast<std::size_t>(i)];
            if (value->authenticated)
            {
                rpc<std::int64_t>([&](auto done) { value->client->mark_read(group_, message, done); });
                ++applied;
            }
            else { needs_tui.push_back(json::string(value->alias)); }
        }
        auto result = read_snapshot(message).as_object();
        result["target_count"] = target;
        result["applied_sdk"] = applied;
        result["needs_tui"] = std::move(needs_tui);
        return result;
    }
    json::value event_snapshot(json::object const& input)
    {
        auto const sender = text(input, "sender", "A");
        auto const kind = text(input, "kind", "message");
        std::lock_guard lock(events_->mutex);
        json::object actors;
        json::array recipients, expected, missing, unexpected;
        std::uint64_t total = 0, duplicates = 0;
        std::set<std::string> actual;
        for (auto const& [alias, count] : events_->actors)
        {
            std::uint64_t message_count = 0;
            for (auto const& [id, n] : count.messages) { if (!events_->message || id == events_->message) { message_count += n; } }
            auto selected = kind == "reaction" ? count.reactions : kind == "typing_true" ? count.typing_true :
                kind == "typing_false" ? count.typing_false : kind == "read" ? count.read :
                kind == "update" ? count.updates : message_count;
            if (selected)
            {
                actual.insert(alias);
                recipients.push_back(json::string(alias));
                total += selected;
                duplicates += selected > 1 ? selected - 1 : 0;
            }
            actors[alias] = json::object{{"messages", message_count}, {"updates", count.updates}, {"reactions", count.reactions},
                {"typing_true", count.typing_true}, {"typing_false", count.typing_false}, {"read", count.read},
                {"reaction_revision", count.reaction_revision}, {"first_ns", count.first_ns}, {"last_ns", count.last_ns}};
        }
        std::set<std::string> expected_set;
        for (auto const& [alias, value] : accounts_)
        {
            // Expected identities are captured from authoritative members at reset.
            if (alias != sender && expected_members_.contains(value->id) && expected_online_.contains(alias))
            { expected.push_back(json::string(alias)); expected_set.insert(alias); }
        }
        for (auto const& alias : expected_set) { if (!actual.contains(alias)) { missing.push_back(json::string(alias)); } }
        for (auto const& alias : actual) { if (!expected_set.contains(alias)) { unexpected.push_back(json::string(alias)); } }
        return json::object{{"conversation", events_->conversation}, {"message", events_->message}, {"kind", kind},
            {"started_ns", events_->started_ns}, {"recipients", std::move(recipients)}, {"recipient_count", actual.size()},
            {"expected", std::move(expected)}, {"expected_count", expected_set.size()}, {"missing", std::move(missing)},
            {"unexpected", std::move(unexpected)}, {"total", total}, {"duplicates", duplicates}, {"actors", std::move(actors)}};
    }
    json::value sdk(json::object const& input)
    {
        auto& client = connected(text(input, "actor", "S005"));
        auto method = text(input, "method");
        auto room = number(input, "conversation", group_);
        auto message = number(input, "message");
        if (method == "get_members") { return measured<std::vector<chat::conversation_member>>([&](auto done) { client.get_members(room, done); }); }
        if (method == "get_contacts") { return measured<std::vector<chat::user>>([&](auto done) { client.get_contacts(done); }); }
        if (method == "get_presence") { return measured<std::vector<chat::presence>>([&](auto done) { client.get_presence(done); }); }
        if (method == "get_messages") { return measured<chat::messages_result>([&](auto done) { client.get_messages(room, cursor(input, "before"), done, cursor(input, "after")); }); }
        if (method == "search_messages") { return measured<chat::messages_result>([&](auto done) { client.search_messages(room, text(input, "query"), cursor(input, "before"), done); }); }
        if (method == "send_message") { return measured<chat::send_message_result>([&](auto done) { client.send_message(room, text(input, "text"), done, cursor(input, "reply_to")); }); }
        if (method == "mark_read") { return measured<std::int64_t>([&](auto done) { client.mark_read(room, message, done); }); }
        if (method == "set_typing") { return measured<bool>([&](auto done) { client.set_typing(room, flag(input, "typing"), done); }); }
        if (method == "set_message_reaction") { return measured<chat::reaction_update>([&](auto done) { client.set_message_reaction(room, message, text(input, "emoji"), done); }); }
        if (method == "edit_message") { return measured<chat::message>([&](auto done) { client.edit_message(room, message, text(input, "text"), done); }); }
        if (method == "delete_message") { return measured<chat::message>([&](auto done) { client.delete_message(room, message, done); }); }
        if (method == "get_conversations")
        {
            std::optional<chat::conversation_cursor> before;
            if (auto p = input.if_contains("before"); p && !p->is_null())
            {
                require(p->is_object(), "Conversation cursor must be an object");
                before = chat::conversation_cursor{number(p->as_object(), "activity"), number(p->as_object(), "id"), flag(p->as_object(), "pinned")};
            }
            return measured<chat::conversations_result>([&](auto done) { client.get_conversations(before, done); });
        }
        if (method == "open_direct_conversation") { return measured<chat::direct_conversation_result>([&](auto done) { client.open_direct_conversation(user(input), done); }); }
        if (method == "get_friend_requests") { return measured<chat::friend_requests_result>([&](auto done) { client.get_friend_requests(done); }); }
        if (method == "send_friend_request") { return measured<chat::friendship_result>([&](auto done) { client.send_friend_request(user(input), done); }); }
        if (method == "respond_friend_request") { return measured<chat::friendship_result>([&](auto done) { client.respond_friend_request(user(input), flag(input, "accept"), done); }); }
        if (method == "cancel_friend_request") { return measured<chat::friendship_result>([&](auto done) { client.cancel_friend_request(user(input), done); }); }
        if (method == "send_attachment" || method == "set_avatar")
        {
            auto path = text(input, "path");
            auto data = chat::tui::read_local_file(path, method == "set_avatar" ? chat::max_avatar_size : chat::max_attachment_size);
            require(data.has_value(), data ? "" : data.error());
            if (method == "set_avatar") { return measured<chat::avatar_state>([&](auto done) { client.set_avatar(std::move(*data), done); }); }
            auto filename = text(input, "filename", std::filesystem::path(path).filename().string());
            return measured<chat::message>([&](auto done) { client.send_attachment(room, filename, std::move(*data), done, cursor(input, "reply_to")); });
        }
        if (method == "clear_avatar") { return measured<chat::avatar_state>([&](auto done) { client.clear_avatar(done); }); }
        if (method == "download_attachment")
        {
            auto result = measured<std::string>([&](auto done) { client.get_attachment(room, message, done); }).as_object();
            if (result.at("ok").as_bool())
            {
                auto bytes = std::string(result.at("value").as_string());
                auto path = text(input, "path");
                auto saved = chat::tui::save_local_file(path, bytes);
                require(saved.has_value(), saved ? "" : saved.error());
                result["value"] = json::object{{"path", path}, {"size", bytes.size()}};
            }
            return result;
        }
        if (method == "remove_contact") { return measured<bool>([&](auto done) { client.remove_contact(user(input), done); }); }
        if (method == "search_users") { return measured<std::vector<chat::user>>([&](auto done) { client.search_users(text(input, "query"), done); }); }
        if (method == "create_group") { return measured<std::int64_t>([&](auto done) { client.create_group(text(input, "title"), ids(input, "members"), done); }); }
        if (method == "set_group_admin") { return measured<bool>([&](auto done) { client.set_group_admin(room, user(input), flag(input, "admin"), done); }); }
        if (method == "transfer_group_owner") { return measured<bool>([&](auto done) { client.transfer_group_owner(room, user(input), done); }); }
        if (method == "remove_group_member") { return measured<bool>([&](auto done) { client.remove_group_member(room, user(input), done); }); }
        if (method == "invite_group_members") { return measured<bool>([&](auto done) { client.invite_group_members(room, ids(input, "members"), done); }); }
        if (method == "leave_group") { return measured<bool>([&](auto done) { client.leave_group(room, done); }); }
        if (method == "rename_group") { return measured<bool>([&](auto done) { client.rename_group(room, text(input, "title"), done); }); }
        if (method == "set_group_announcement") { return measured<bool>([&](auto done) { client.set_group_announcement(room, text(input, "text"), done); }); }
        if (method == "pin_group_message") { return measured<bool>([&](auto done) { client.pin_group_message(room, message, done); }); }
        if (method == "unpin_group_message") { return measured<bool>([&](auto done) { client.unpin_group_message(room, done); }); }
        if (method == "get_group_invite") { return measured<std::optional<std::string>>([&](auto done) { client.get_group_invite(room, done); }); }
        if (method == "create_group_invite") { return measured<std::optional<std::string>>([&](auto done) { client.create_group_invite(room, done); }); }
        if (method == "revoke_group_invite") { return measured<std::optional<std::string>>([&](auto done) { client.revoke_group_invite(room, done); }); }
        if (method == "join_group") { return measured<chat::group_join_result>([&](auto done) { client.join_group(text(input, "token"), done); }); }
        if (method == "set_group_join_approval") { return measured<bool>([&](auto done) { client.set_group_join_approval(room, flag(input, "required"), done); }); }
        if (method == "get_group_join_requests") { return measured<chat::group_join_requests_result>([&](auto done) { client.get_group_join_requests(room, cursor(input, "before"), done); }); }
        if (method == "respond_group_join_request") { return measured<bool>([&](auto done) { client.respond_group_join_request(room, user(input), flag(input, "accept"), done); }); }
        if (method == "set_conversation_muted") { return measured<bool>([&](auto done) { client.set_conversation_muted(room, flag(input, "muted"), done); }); }
        if (method == "set_conversation_pinned") { return measured<bool>([&](auto done) { client.set_conversation_pinned(room, flag(input, "pinned"), done); }); }
        throw std::runtime_error("Unsupported diagnostic SDK method: " + method);
    }
    json::value command(json::object const& input)
    {
        auto name = text(input, "command");
        if (name == "configure")
        {
            require(!configured_, "Already configured");
            url_ = text(input, "url");
            prefix_ = text(input, "prefix");
            password_ = text(input, "password");
            require((url_.starts_with("ws://") || url_.starts_with("wss://")) && !prefix_.empty() && prefix_.size() <= 24 &&
                std::ranges::all_of(prefix_, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }) &&
                !password_.empty() && password_.size() <= 72, "Invalid diagnostic configuration");
            configured_ = true;
            return json::object{{"configured", true}};
        }
        if (name == "seed_navigation") { return seed_navigation(); }
        if (name == "seed") { return seed(input); }
        if (name == "manifest") { return manifest_; }
        if (name == "close") { close(); return json::object{{"closed", true}, {"business_data_deleted", false}}; }
        require(!manifest_.empty(), "Successful seed required");
        if (name == "status") { return status(flag(input, "verify")); }
        if (name == "exercise_friend_pairs") { return exercise_friend_pairs(); }
        if (name == "measure_sizes") { return measure_sizes(); }
        if (name == "connect" || name == "disconnect")
        {
            for (auto const& alias : aliases(input))
            { if (name == "connect") { connect_actor(alias); } else { disconnect_actor(alias); } }
            return status(false);
        }
        if (name == "replace-reserved")
        {
            rpc<bool>([&](auto done) { connected("S005").remove_group_member(group_, actor("S100").id, done); });
            disconnect_actor("S100");
            return status(true);
        }
        if (name == "read_snapshot") { return read_snapshot(number(input, "message")); }
        if (name == "read") { return read(input); }
        if (name == "sdk") { return sdk(input); }
        if (name == "events_reset")
        {
            expected_members_.clear();
            expected_online_.clear();
            for (auto const& [alias, value] : accounts_) { if (value->authenticated) { expected_online_.insert(alias); } }
            for (auto const& value : members()) { expected_members_.insert(value.id); }
            std::lock_guard lock(events_->mutex);
            events_->enabled = true;
            events_->conversation = number(input, "conversation", group_);
            events_->message = number(input, "message");
            events_->marker = text(input, "marker");
            events_->started_ns = now_ns();
            events_->actors.clear();
            return json::object{{"reset", true}, {"started_ns", events_->started_ns}};
        }
        if (name == "events") { return event_snapshot(input); }
        throw std::runtime_error("Unknown diagnostic command: " + name);
    }
private:
    std::set<std::int64_t> expected_members_;
    std::set<std::string> expected_online_;
};
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        std::cout << "Explicit 100-member diagnostic fixture; JSONL controls on stdin/stdout.\n"
            "Configure via stdin: {id,command:\"configure\",url,prefix,password}. Never pass credentials as CLI arguments.\n"
            "Commands: seed, seed_navigation, manifest, status, connect, disconnect, replace-reserved, exercise_friend_pairs, measure_sizes, sdk, read, read_snapshot, events_reset, events, close.\n"
            "Use a dedicated disposable database. This tool never deletes business data or executes SQL.\n";
        return 0;
    }
    if (argc != 1) { std::cerr << "Usage: chat_tui_scale_fixture [--help]\n"; return 1; }
    fixture control;
    std::string line;
    while (std::getline(std::cin, line))
    {
        json::value id = nullptr;
        auto start = clock_type::now();
        try
        {
            auto value = json::parse(line);
            require(value.is_object(), "Control must be a JSON object");
            auto const& input = value.as_object();
            if (auto field = input.if_contains("id")) { id = *field; }
            auto result = control.command(input);
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock_type::now() - start).count();
            std::cout << json::serialize(json::object{{"id", id}, {"ok", true}, {"result", std::move(result)}, {"control_elapsed_us", elapsed}}) << '\n' << std::flush;
        }
        catch (std::exception const& error)
        {
            std::cout << json::serialize(json::object{{"id", id}, {"ok", false}, {"error", error.what()}}) << '\n' << std::flush;
        }
    }
}
