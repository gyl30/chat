#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <libpq-fe.h>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/work_guard.hpp>
#include <boost/http/server/router.hpp>
#include <chat/client.hpp>
#include <chat/detail/base64.hpp>

#include "server.hpp"
#include "avatar_fixture.hpp"

namespace
{

void require(bool condition, std::string const& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

template <class T, class F> std::expected<T, chat::error> call(F operation, std::chrono::seconds timeout = std::chrono::seconds(5))
{
    auto promise = std::make_shared<std::promise<std::expected<T, chat::error>>>();
    auto future = promise->get_future();
    operation([promise](auto result) { promise->set_value(std::move(result)); });
    require(future.wait_for(timeout) == std::future_status::ready, "RPC timeout");
    return future.get();
}

struct events
{
    std::mutex mutex;
    std::condition_variable condition;
    int connected = 0;
    int disconnected = 0;
    std::vector<chat::message> messages;
    std::vector<chat::message> updates;
    std::vector<chat::reaction_update> reactions;
    std::vector<std::int64_t> conversations;
    std::vector<std::int64_t> removals;
    std::vector<chat::read_position> reads;
    std::vector<chat::typing_event> typing;
    std::vector<chat::avatar> avatars;
    std::vector<chat::group_join_request_event> join_requests;

    template <class F> void wait(F predicate)
    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(5), predicate), "Notification timeout");
    }

    void attach(chat::client& client)
    {
        client.set_group_join_request_handler([this](chat::group_join_request_event value) {
            std::lock_guard lock(mutex);
            join_requests.push_back(value);
            condition.notify_all();
        });
        client.set_reaction_handler([this](chat::reaction_update value) {
            std::lock_guard lock(mutex);
            reactions.push_back(std::move(value));
            condition.notify_all();
        });
        client.set_avatar_handler([this](std::int64_t user, chat::avatar_state state) {
            std::lock_guard lock(mutex);
            avatars.push_back({user, state, {}, {}});
            condition.notify_all();
        });
        client.set_typing_handler([this](chat::typing_event value) {
            std::lock_guard lock(mutex);
            typing.push_back(std::move(value));
            condition.notify_all();
        });
        client.set_connected_handler(
            [this]
            {
                std::lock_guard lock(mutex);
                ++connected;
                condition.notify_all();
            });
        client.set_disconnected_handler(
            [this]
            {
                std::lock_guard lock(mutex);
                ++disconnected;
                condition.notify_all();
            });
        client.set_message_handler(
            [this](chat::message message)
            {
                std::lock_guard lock(mutex);
                messages.push_back(std::move(message));
                condition.notify_all();
            });
        client.set_message_updated_handler(
            [this](chat::message value)
            {
                std::lock_guard lock(mutex);
                updates.push_back(std::move(value));
                condition.notify_all();
            });
        client.set_conversation_handler(
            [this](std::int64_t conversation, bool removed)
            {
                std::lock_guard lock(mutex);
                conversations.push_back(conversation);
                if (removed) { removals.push_back(conversation); }
                condition.notify_all();
            });
        client.set_read_handler(
            [this](std::int64_t, std::int64_t user, std::int64_t message)
            {
                std::lock_guard lock(mutex);
                reads.push_back({user, message});
                condition.notify_all();
            });
    }
};

struct fixture
{
    std::unique_ptr<PGconn, decltype(&PQfinish)> database{
        PQconnectdb(""), &PQfinish};
    std::vector<std::int64_t> users;
    std::vector<std::int64_t> groups;

    void execute(std::string const& query)
    {
        std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(database.get(), query.c_str()), &PQclear);
        require(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK, PQerrorMessage(database.get()));
    }

    void cleanup()
    {
        if (PQtransactionStatus(database.get()) != PQTRANS_IDLE)
        {
            execute("ROLLBACK");
        }
        for (auto id : groups)
        {
            execute("DELETE FROM conversations WHERE id=" + std::to_string(id));
        }
        groups.clear();
        for (auto id : users)
        {
            execute("DELETE FROM users WHERE id=" + std::to_string(id));
        }
        users.clear();
    }

    ~fixture()
    {
        try
        {
            cleanup();
        }
        catch (std::exception const& error)
        {
            std::cerr << "Fixture cleanup failed: " << error.what() << '\n';
        }
    }
};

struct runtime
{
    boost::corosio::io_context io;
    boost::capy::work_guard<boost::corosio::io_context::executor_type> work{
        boost::capy::make_work_guard(io.get_executor())};
    chat_server server{io, 8, boost::http::router<boost::http::route_params>{},
                       "", 4};
    std::thread thread;
    std::string url;

    runtime()
    {
        require(!server.bind(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), 0)), "Server bind");
        url = "ws://127.0.0.1:" + std::to_string(server.local_endpoint().port()) + "/ws";
        server.start();
        thread = std::thread([this] { io.run(); });
    }

    ~runtime()
    {
        boost::capy::run_async(io.get_executor())(
            [](chat_server& server) -> boost::capy::task<void>
            {
                server.stop();
                co_return;
            }(server));
        work.reset();
        thread.join();
        server.join();
    }
};

chat::conversation conversation(chat::client& client, std::int64_t id)
{
    auto result = call<chat::conversations_result>([&](auto handler) { client.get_conversations({}, handler); });
    require(result.has_value(), "Conversations RPC");
    for (auto const& value : result->conversations)
    {
        if (value.id == id)
        {
            return value;
        }
    }
    throw std::runtime_error("Conversation missing");
}

std::int64_t position(chat::messages_result const& result, std::int64_t user)
{
    for (auto const& value : result.read_positions)
    {
        if (value.user == user)
        {
            return value.message;
        }
    }
    throw std::runtime_error("Read position missing");
}

}

int run_group_tests()
{
    try
    {
        runtime server;
        fixture data;
        require(PQstatus(data.database.get()) == CONNECTION_OK, "Fixture database connection");
        events a_events, b_events, c_events, d_events;
        chat::client a, b, c, d;
        std::vector<chat::client*> clients{&a, &b, &c, &d};
        std::vector<events*> event_list{&a_events, &b_events, &c_events, &d_events};
        std::vector<std::string> names;
        for (int i = 0; i < 4; ++i)
        {
            auto& client = *clients[i];
            event_list[i]->attach(client);
            client.connect(server.url);
            event_list[i]->wait([&, i] { return event_list[i]->connected == 1; });
            names.push_back("chat_group_test_" + std::to_string(getpid()) + "_" + std::to_string(i));
            auto registered = call<std::int64_t>([&](auto handler)
                                                 { client.register_user(names.back(), "group password", handler); });
            require(registered.has_value(), "Register group fixture");
            data.users.push_back(*registered);
            auto unauthenticated_remove = call<bool>(
                [&](auto handler) { client.remove_contact(data.users.front(), handler); });
            require(!unauthenticated_remove && unauthenticated_remove.error().code == -32001,
                    "Contact removal requires authentication");
            auto unauthenticated_search = call<chat::messages_result>(
                [&](auto handler) { client.search_messages(1, "test", {}, handler); });
            require(!unauthenticated_search && unauthenticated_search.error().code == -32001,
                    "Message search requires authentication");
            auto unauthenticated_typing = call<bool>(
                [&](auto handler) { client.set_typing(1, true, handler); });
            require(!unauthenticated_typing && unauthenticated_typing.error().code == -32001,
                    "Typing requires authentication");
            auto unauthenticated_mute = call<bool>([&](auto handler) { client.set_conversation_muted(1, true, handler); });
            require(!unauthenticated_mute && unauthenticated_mute.error().code == -32001,
                    "Mute requires authentication");
            auto unauthenticated_pin = call<bool>([&](auto handler) { client.set_conversation_pinned(1, true, handler); });
            require(!unauthenticated_pin && unauthenticated_pin.error().code == -32001,
                    "Pin requires authentication");
            auto unauthenticated_reaction = call<chat::reaction_update>([&](auto handler) {
                client.set_message_reaction(1, 1, "👍", handler);
            });
            require(!unauthenticated_reaction && unauthenticated_reaction.error().code == -32001,
                    "Reaction requires authentication");
            auto unauthenticated_avatar = call<chat::avatar>([&](auto handler) {
                client.get_avatar(data.users.front(), 0, handler);
            });
            require(!unauthenticated_avatar && unauthenticated_avatar.error().code == -32001,
                    "Avatar requires authentication");
            auto authenticated = call<chat::authentication_result>(
                [&](auto handler) { client.authenticate(names.back(), "group password", handler); });
            require(authenticated && authenticated->authenticated && authenticated->user == *registered,
                    "Authenticate identity");
        }
        for (int i = 1; i < 3; ++i)
        {
            require(call<chat::user>([&](auto handler) { a.add_contact(data.users[i], handler); }).has_value(),
                    "Add group contact");
        }
        auto rejected = call<std::int64_t>([&](auto handler) { a.create_group("invalid", {data.users[3]}, handler); });
        require(!rejected && rejected.error().code == -32005, "Reject non-contact creation");
        auto duplicate = call<std::int64_t>([&](auto handler)
                                            { a.create_group("invalid", {data.users[1], data.users[1]}, handler); });
        require(!duplicate && duplicate.error().code == -32602, "Reject duplicate members");
        auto created = call<std::int64_t>([&](auto handler)
                                          { a.create_group("三人测试群", {data.users[1], data.users[2]}, handler); });
        require(created.has_value(), "Create group");
        auto group = *created;
        data.groups.push_back(group);
        b_events.wait([&] { return !b_events.conversations.empty(); });
        c_events.wait([&] { return !c_events.conversations.empty(); });
        auto empty = conversation(b, group);
        require(empty.kind == chat::conversation_kind::group && empty.last.id == 0 && empty.member_count == 3 &&
                    empty.unread == 0 && !empty.muted && !empty.pinned,
                "Empty group descriptor");
        auto members = call<std::vector<chat::conversation_member>>([&](auto handler) { c.get_members(group, handler); });
        require(members && members->size() == 3 && (*members)[0].id == data.users[0] &&
                    (*members)[0].role == chat::member_role::owner && (*members)[1].role == chat::member_role::member,
                "Group member roles");
        auto forbidden = call<chat::messages_result>([&](auto handler) { d.get_messages(group, {}, handler); });
        require(!forbidden && forbidden.error().code == -32006, "Nonmember history denied");
        auto forbidden_members = call<std::vector<chat::conversation_member>>([&](auto handler) { d.get_members(group, handler); });
        require(!forbidden_members && forbidden_members.error().code == -32006, "Nonmember member list denied");
        auto forbidden_send =
            call<chat::send_message_result>([&](auto handler) { d.send_message(group, "not allowed", handler); });
        require(!forbidden_send && forbidden_send.error().code == -32006, "Nonmember send denied");
        auto sent =
            call<chat::send_message_result>([&](auto handler) { a.send_message(group, "群聊第一条", handler); });
        require(sent && sent->realtime, "Group send response");
        b_events.wait([&] { return b_events.messages.size() == 1; });
        c_events.wait([&] { return c_events.messages.size() == 1; });
        require(b_events.messages[0].conversation == group && b_events.messages[0].id == sent->message_id &&
                    b_events.messages[0].username == names[0],
                "Member message routing");
        require(conversation(b, group).unread == 1 && conversation(c, group).unread == 1 &&
                    conversation(a, group).unread == 0,
                "Independent unread");
        auto muted = call<bool>([&](auto handler) { c.set_conversation_muted(group, true, handler); });
        auto repeated_mute = call<bool>([&](auto handler) { c.set_conversation_muted(group, true, handler); });
        auto forbidden_mute = call<bool>([&](auto handler) { d.set_conversation_muted(group, true, handler); });
        auto invalid_mute = call<bool>([&](auto handler) { a.set_conversation_muted(0, false, handler); });
        require(muted && *muted && repeated_mute && *repeated_mute && conversation(c, group).muted &&
            !conversation(b, group).muted && conversation(c, group).unread == 1 &&
            conversation(c, group).last.id == sent->message_id && !forbidden_mute && forbidden_mute.error().code == -32006 &&
            !invalid_mute && invalid_mute.error().code == -32602,
            "Mute is private, idempotent and changes neither unread nor message activity; membership is required");
        auto pinned = call<bool>([&](auto handler) { c.set_conversation_pinned(group, true, handler); });
        auto repeated_pin = call<bool>([&](auto handler) { c.set_conversation_pinned(group, true, handler); });
        auto forbidden_pin = call<bool>([&](auto handler) { d.set_conversation_pinned(group, true, handler); });
        auto invalid_pin = call<bool>([&](auto handler) { a.set_conversation_pinned(0, false, handler); });
        require(pinned && *pinned && repeated_pin && *repeated_pin && conversation(c, group).pinned &&
            !conversation(b, group).pinned && conversation(c, group).unread == 1 &&
            conversation(c, group).last.id == sent->message_id && !forbidden_pin && forbidden_pin.error().code == -32006 &&
            !invalid_pin && invalid_pin.error().code == -32602,
            "Pin is private and idempotent without changing message activity or unread");
        {
            auto denied = call<chat::reaction_update>([&](auto handler) {
                d.set_message_reaction(group, sent->message_id, "👍", handler);
            });
            auto invalid = call<chat::reaction_update>([&](auto handler) {
                a.set_message_reaction(group, sent->message_id, "not emoji", handler);
            });
            require(!denied && denied.error().code == -32007 && !invalid && invalid.error().code == -32602,
                    "Reaction membership and finite emoji validation");
            auto first = call<chat::reaction_update>([&](auto handler) {
                b.set_message_reaction(group, sent->message_id, "👍", handler);
            });
            auto repeated = call<chat::reaction_update>([&](auto handler) {
                b.set_message_reaction(group, sent->message_id, "👍", handler);
            });
            require(first && repeated && first->revision == 1 && repeated->revision == 1 &&
                    first->reactions.front().users == std::vector<std::int64_t>{data.users[1]},
                    "Explicit reaction setting is idempotent");
            a_events.wait([&] { return a_events.reactions.size() == 1; });
            c_events.wait([&] { return c_events.reactions.size() == 1; });
            auto aggregated = call<chat::reaction_update>([&](auto handler) {
                c.set_message_reaction(group, sent->message_id, "👍", handler);
            });
            require(aggregated && aggregated->revision == 2 && aggregated->reactions.size() == 1 &&
                    aggregated->reactions.front().users == std::vector<std::int64_t>{data.users[1], data.users[2]},
                    "Multiple users aggregate into one emoji count");
            auto replaced = call<chat::reaction_update>([&](auto handler) {
                b.set_message_reaction(group, sent->message_id, "😂", handler);
            });
            require(replaced && replaced->revision == 3 && replaced->reactions.size() == 2,
                    "Changing emoji replaces only the current user's reaction");
            require(conversation(b, group).last.reaction_revision == 3 && conversation(b, group).unread == 1,
                    "Conversation summary carries reaction without changing unread");
            auto searched = call<chat::messages_result>([&](auto handler) {
                a.search_messages(group, "第一条", {}, handler);
            });
            require(searched && searched->messages.size() == 1 && searched->messages.front().reaction_revision == 3 &&
                    searched->messages.front().reactions.size() == 2, "Search includes persisted reactions");
            auto clear = call<chat::reaction_update>([&](auto handler) { b.set_message_reaction(group, sent->message_id, "", handler); });
            auto clear_again = call<chat::reaction_update>([&](auto handler) { b.set_message_reaction(group, sent->message_id, "", handler); });
            require(clear && clear_again && clear->revision == 4 && clear_again->revision == 4 && clear->reactions.size() == 1,
                    "Clear retains other users and does not advance revision twice");
            require(call<chat::reaction_update>([&](auto handler) { c.set_message_reaction(group, sent->message_id, "", handler); })->revision == 5,
                    "Last clear advances the persistent empty snapshot revision");
            auto restored = call<chat::reaction_update>([&](auto handler) { b.set_message_reaction(group, sent->message_id, "❤️", handler); });
            require(restored && restored->revision == 6, "Re-add cannot reuse an old revision");
            require(call<std::vector<chat::user>>([&](auto handler) { d.get_contacts(handler); }).has_value(), "Reaction routing barrier");
            { std::lock_guard lock(d_events.mutex); require(d_events.reactions.empty(), "Nonmember receives no reaction notification"); }
        }
        {
            auto png = *chat::detail::decode_base64(avatar_png_base64);
            auto jpeg = *chat::detail::decode_base64(avatar_jpeg_base64);
            auto empty_avatar = call<chat::avatar>([&](auto handler) { b.get_avatar(data.users[0], 0, handler); });
            require(empty_avatar && empty_avatar->state == chat::avatar_state{} && empty_avatar->data.empty(),
                    "Existing user starts without an avatar");
            auto first_avatar = call<chat::avatar_state>([&](auto handler) { a.set_avatar(png, handler); });
            require(first_avatar && first_avatar->present && first_avatar->revision == 1, "Upload persistent PNG avatar");
            a_events.wait([&] { return a_events.avatars.size() == 1; });
            b_events.wait([&] { return b_events.avatars.size() == 1; });
            c_events.wait([&] { return c_events.avatars.size() == 1; });
            auto stale_avatar = call<chat::avatar>([&](auto handler) { b.get_avatar(data.users[0], 0, handler); });
            auto downloaded = call<chat::avatar>([&](auto handler) { d.get_avatar(data.users[0], 1, handler); });
            require(stale_avatar && stale_avatar->state == *first_avatar && stale_avatar->data.empty() &&
                downloaded && downloaded->data == png && downloaded->media_type == "image/png",
                "Stale revision returns current metadata; searchable users have readable avatars");
            auto search = call<std::vector<chat::user>>([&](auto handler) { b.search_users(names[0], handler); });
            auto avatar_members = call<std::vector<chat::conversation_member>>([&](auto handler) { c.get_members(group, handler); });
            require(search && search->size() == 1 && search->front().avatar == *first_avatar &&
                avatar_members && avatar_members->front().avatar == *first_avatar,
                "User search and group members expose current avatar metadata");
            auto replacement = call<chat::avatar_state>([&](auto handler) { a.set_avatar(jpeg, handler); });
            auto current_history = call<chat::messages_result>([&](auto handler) { b.get_messages(group, {}, handler); });
            auto current_file = call<chat::avatar>([&](auto handler) { b.get_avatar(data.users[0], 2, handler); });
            require(replacement && replacement->revision == 2 && replacement->present && current_file &&
                current_file->media_type == "image/jpeg" && current_file->data == jpeg && current_history &&
                current_history->messages.front().avatar == *replacement &&
                conversation(b, group).last.avatar == *replacement,
                "Replacement changes revision and old history uses the author's current avatar");
            auto cleared = call<chat::avatar_state>([&](auto handler) { a.clear_avatar(handler); });
            require(cleared && cleared->revision == 3 && !cleared->present, "Clear retains a monotonic revision");
            auto cleared_file = call<chat::avatar>([&](auto handler) { b.get_avatar(data.users[0], 2, handler); });
            auto restored_avatar = call<chat::avatar_state>([&](auto handler) { a.set_avatar(png, handler); });
            require(cleared_file && cleared_file->state == *cleared && cleared_file->data.empty() &&
                restored_avatar && restored_avatar->revision == 4 && restored_avatar->present,
                "Clear and re-upload cannot reuse an old cache key");
            auto invalid_avatar = call<chat::avatar_state>([&](auto handler) { a.set_avatar("not an image", handler); });
            auto retained_avatar = call<chat::avatar>([&](auto handler) { b.get_avatar(data.users[0], 4, handler); });
            require(!invalid_avatar && invalid_avatar.error().code == -32602 && retained_avatar && retained_avatar->data == png,
                "Invalid media does not replace the current avatar");
            b_events.wait([&] { return b_events.avatars.size() == 4; });
            c_events.wait([&] { return c_events.avatars.size() == 4; });
            require(call<std::vector<chat::user>>([&](auto handler) { d.get_contacts(handler); }).has_value(),
                "Unrelated notification barrier");
            {
                std::lock_guard lock(d_events.mutex);
                require(d_events.avatars.empty(), "Avatar changes are not globally broadcast");
            }
            auto empty_direct = call<std::int64_t>([&](auto handler) { d.open_direct_conversation(data.users[0], handler); });
            require(empty_direct.has_value(), "Empty direct conversation notification relationship");
            auto updating = std::async(std::launch::async, [&] {
                return call<chat::avatar_state>([&](auto handler) { a.set_avatar(jpeg, handler); });
            });
            auto concurrent_avatar = call<chat::avatar>([&](auto handler) { b.get_avatar(data.users[0], 4, handler); });
            auto concurrent_state = updating.get();
            require(concurrent_state && concurrent_state->revision == 5 && concurrent_avatar &&
                ((concurrent_avatar->state.revision == 4 && concurrent_avatar->data == png) ||
                 (concurrent_avatar->state == *concurrent_state && concurrent_avatar->data.empty())),
                 "Concurrent update/get observes one atomic revision and matching bytes or stale metadata");
            d_events.wait([&] { return d_events.avatars.size() == 1; });
            require(call<chat::avatar_state>([&](auto handler) { a.clear_avatar(handler); }).has_value(), "Avatar cleanup");
        }
        auto typing_start = call<bool>([&](auto handler) { a.set_typing(group, true, handler); });
        require(typing_start && *typing_start, "Typing realtime response");
        b_events.wait([&] { return b_events.typing.size() == 1; });
        c_events.wait([&] { return c_events.typing.size() == 1; });
        {
            std::lock_guard lock(b_events.mutex);
            auto const& value = b_events.typing.front();
            require(value.conversation == group && value.user == data.users[0] && value.username == names[0] &&
                        value.typing, "Typing carries authenticated author and conversation");
        }
        require(call<bool>([&](auto handler) { a.set_typing(group, false, handler); }).has_value(), "Typing stop");
        b_events.wait([&] { return b_events.typing.size() == 2 && !b_events.typing.back().typing; });
        c_events.wait([&] { return c_events.typing.size() == 2 && !c_events.typing.back().typing; });
        {
            std::lock_guard lock(a_events.mutex);
            require(a_events.typing.empty(), "Typing is not echoed to the sender");
        }
        auto denied_typing = call<bool>([&](auto handler) { d.set_typing(group, true, handler); });
        auto invalid_typing = call<bool>([&](auto handler) { a.set_typing(0, true, handler); });
        require(!denied_typing && denied_typing.error().code == -32006 && !invalid_typing &&
                    invalid_typing.error().code == -32602, "Typing membership and parameter validation");
        auto typing_history = call<chat::messages_result>([&](auto handler) { a.get_messages(group, {}, handler); });
        require(typing_history && typing_history->messages.size() == 1 &&
                    std::ranges::all_of(typing_history->read_positions, [](auto const& position) { return position.message == 0; }) &&
                    conversation(b, group).unread == 1 && conversation(c, group).unread == 1,
                "Typing does not persist a message or change unread and read position");
        auto read = call<std::int64_t>([&](auto handler) { b.mark_read(group, sent->message_id, handler); });
        require(read && *read == sent->message_id, "Group read");
        a_events.wait([&] { return !a_events.reads.empty(); });
        auto history = call<chat::messages_result>([&](auto handler) { a.get_messages(group, {}, handler); });
        require(history && position(*history, data.users[1]) == sent->message_id &&
                    position(*history, data.users[2]) == 0,
                "Member read snapshots");
        require(conversation(b, group).unread == 0 && conversation(c, group).unread == 1,
                "Reading does not clear another member");
        c.close();
        c_events.wait([&] { return c_events.disconnected == 1; });
        std::int64_t latest = sent->message_id;
        for (int i = 0; i < 57; ++i)
        {
            auto next = call<chat::send_message_result>(
                [&](auto handler) { a.send_message(group, "offline " + std::to_string(i), handler); });
            require(next.has_value(), "Offline group send");
            latest = next->message_id;
        }
        c.connect(server.url);
        c_events.wait([&] { return c_events.connected == 2; });
        auto reauthenticated = call<chat::authentication_result>(
            [&](auto handler) { c.authenticate(names[2], "group password", handler); });
        require(reauthenticated && reauthenticated->authenticated, "Reauthenticate group member");
        require(conversation(c, group).muted && conversation(c, group).pinned, "Reconnect restores persisted group preferences");
        auto first_page = call<chat::messages_result>([&](auto handler) { c.get_messages(group, {}, handler); });
        require(first_page && first_page->messages.size() == 50 && first_page->has_more &&
                    first_page->messages.back().id == latest,
                "Latest history page");
        auto before = first_page->messages.front().id;
        auto older = call<chat::messages_result>([&](auto handler) { c.get_messages(group, before, handler); });
        require(older && older->messages.size() == 8 && !older->has_more &&
                    older->messages.front().id == sent->message_id && older->messages.front().reaction_revision == 6 &&
                    older->messages.front().reactions.size() == 1 && older->messages.front().reactions.front().emoji == "❤️",
                "Older cursor page");
        require(position(*first_page, data.users[1]) == sent->message_id &&
                    position(*older, data.users[1]) == sent->message_id &&
                    position(*first_page, data.users[2]) == 0 && position(*older, data.users[2]) == 0,
                "Pagination and reconnect preserve real read positions without marking old messages read");
        auto const unread_before_search = conversation(c, group).unread;
        auto search_page = call<chat::messages_result>(
            [&](auto handler) { c.search_messages(group, "OFFLINE", {}, handler); });
        require(search_page && search_page->messages.size() == 50 && search_page->has_more &&
                    search_page->messages.back().id == latest, "Case-insensitive message search cursor page");
        auto search_older = call<chat::messages_result>([&](auto handler) {
            c.search_messages(group, "OFFLINE", search_page->messages.front().id, handler);
        });
        require(search_older && search_older->messages.size() == 7 && !search_older->has_more &&
                    search_older->messages.back().id < search_page->messages.front().id,
                "Search pages have no duplicate results");
        require(position(*search_page, data.users[2]) == position(*first_page, data.users[2]) &&
                    conversation(c, group).unread == unread_before_search, "Searching does not advance read position");
        auto denied_search = call<chat::messages_result>(
            [&](auto handler) { d.search_messages(group, "offline", {}, handler); });
        auto empty_search = call<chat::messages_result>(
            [&](auto handler) { a.search_messages(group, "", {}, handler); });
        require(!denied_search && denied_search.error().code == -32006 && !empty_search &&
                    empty_search.error().code == -32602, "Search membership and empty-query validation");
        for (auto const& query : {std::string(1025, 'x'), std::string("a\0b", 3)})
        {
            auto invalid_search = call<chat::messages_result>(
                [&](auto handler) { a.search_messages(group, query, {}, handler); });
            require(!invalid_search && invalid_search.error().code == -32602,
                    "Oversized and NUL-containing search queries rejected");
        }
        std::int64_t cursor = sent->message_id;
        std::size_t restored = 0;
        bool more = true;
        while (more)
        {
            auto page = call<chat::messages_result>([&](auto handler) { c.get_messages(group, {}, handler, cursor); });
            require(page && !page->messages.empty(), "Forward recovery page");
            for (auto const& message : page->messages)
            {
                require(message.id > cursor, "Recovery order");
                cursor = message.id;
                ++restored;
            }
            more = page->has_more;
        }
        require(restored == 57 && cursor == latest && conversation(c, group).unread == 58,
                "Recover more than one page");
        require(call<std::int64_t>([&](auto handler) { c.mark_read(group, latest, handler); }).value() == latest,
                "Read recovered messages");
        require(call<std::int64_t>([&](auto handler) { c.mark_read(group, sent->message_id, handler); }).value() ==
                    latest,
                "Monotonic read position");
        auto direct = call<std::int64_t>([&](auto handler) { a.open_direct_conversation(data.users[1], handler); });
        auto same = call<std::int64_t>([&](auto handler) { b.open_direct_conversation(data.users[0], handler); });
        require(direct && same && *direct == *same && *direct != group, "Canonical direct pair");
        auto direct_sent = call<chat::send_message_result>([&](auto handler)
                                                           { a.send_message(*direct, "direct after group", handler); });
        require(direct_sent.has_value(), "Direct send after group");
        auto direct_before_mute = conversation(b, *direct);
        auto direct_mute = call<bool>([&](auto handler) { b.set_conversation_muted(*direct, true, handler); });
        require(direct_mute && *direct_mute && conversation(b, *direct).muted && !conversation(a, *direct).muted &&
            conversation(b, *direct).unread == direct_before_mute.unread &&
            conversation(b, *direct).last.id == direct_before_mute.last.id, "Direct mute is also a personal preference");
        require(call<bool>([&](auto handler) { b.set_conversation_pinned(*direct, true, handler); }).value() &&
            !conversation(a, *direct).pinned, "Direct pin is private");
        b.close();
        b_events.wait([&] { return b_events.disconnected == 1; });
        b.connect(server.url);
        b_events.wait([&] { return b_events.connected == 2; });
        auto direct_relogin = call<chat::authentication_result>([&](auto handler) {
            b.authenticate(names[1], "group password", handler);
        });
        require(direct_relogin && direct_relogin->authenticated && conversation(b, *direct).muted && conversation(b, *direct).pinned &&
            conversation(b, *direct).unread == direct_before_mute.unread,
            "Disconnect and login restore persisted direct mute and unread");
        auto direct_unmute = call<bool>([&](auto handler) { b.set_conversation_muted(*direct, false, handler); });
        require(direct_unmute && !*direct_unmute && !conversation(b, *direct).muted, "Explicit unmute restores direct preference");
        auto direct_reaction = call<chat::reaction_update>([&](auto handler) {
            b.set_message_reaction(*direct, direct_sent->message_id, "🎉", handler);
        });
        auto cross_reaction = call<chat::reaction_update>([&](auto handler) {
            b.set_message_reaction(group, direct_sent->message_id, "🎉", handler);
        });
        require(direct_reaction && direct_reaction->revision == 1 && !cross_reaction && cross_reaction.error().code == -32007,
                "Direct reaction and cross-conversation rejection");
        auto wrong = call<std::int64_t>([&](auto handler) { b.mark_read(group, direct_sent->message_id, handler); });
        require(!wrong && wrong.error().code == -32602, "Reject cross-conversation read");
        require(conversation(b, *direct).kind == chat::conversation_kind::direct &&
                    conversation(b, *direct).user == data.users[0],
                "Direct descriptor remains user-scoped");
        auto one = std::async(std::launch::async,
                              [&]
                              {
                                  return call<chat::send_message_result>(
                                      [&](auto handler) { a.send_message(group, "concurrent a", handler); });
                              });
        auto two = std::async(std::launch::async,
                              [&]
                              {
                                  return call<chat::send_message_result>(
                                      [&](auto handler) { b.send_message(group, "concurrent b", handler); });
                              });
        require(one.get().has_value() && two.get().has_value(), "Concurrent members send");
        auto concurrent =
            call<chat::messages_result>([&](auto handler) { c.get_messages(group, {}, handler, latest); });
        require(concurrent && concurrent->messages.size() == 2 &&
                    concurrent->messages[0].id < concurrent->messages[1].id,
                "Concurrent history ordering");
        auto replied = call<chat::send_message_result>(
            [&](auto handler) { b.send_message(group, "引用回复", handler, sent->message_id); });
        require(replied && replied->reply && replied->reply->id == sent->message_id &&
                    replied->reply->text == "群聊第一条",
                "Reply send result");
        c_events.wait(
            [&]
            {
                return std::ranges::any_of(
                    c_events.messages, [&](auto const& value)
                    { return value.id == replied->message_id && value.reply && value.reply->id == sent->message_id; });
            });
        auto reply_history = call<chat::messages_result>([&](auto handler) { c.get_messages(group, {}, handler); });
        require(reply_history && reply_history->messages.back().reply &&
                    reply_history->messages.back().reply->from == data.users[0],
                "Persisted reply history");
        auto cross_reply = call<chat::send_message_result>(
            [&](auto handler) { b.send_message(group, "wrong", handler, direct_sent->message_id); });
        require(!cross_reply && cross_reply.error().code == -32602, "Reject cross-conversation reply");
        auto missing_reply = call<chat::send_message_result>(
            [&](auto handler) { b.send_message(group, "wrong", handler, 9223372036854775807LL); });
        require(!missing_reply && missing_reply.error().code == -32602, "Reject nonexistent reply");
        auto direct_reply = call<chat::send_message_result>(
            [&](auto handler) { b.send_message(*direct, "direct reply", handler, direct_sent->message_id); });
        require(direct_reply && direct_reply->reply && direct_reply->reply->id == direct_sent->message_id,
                "Direct reply");
        auto forbidden_edit = call<chat::message>(
            [&](auto handler) { b.edit_message(group, sent->message_id, "other author", handler); });
        require(!forbidden_edit && forbidden_edit.error().code == -32007, "Cannot edit another author");
        auto cross_edit = call<chat::message>(
            [&](auto handler) { a.edit_message(*direct, sent->message_id, "other conversation", handler); });
        require(!cross_edit && cross_edit.error().code == -32007, "Cannot edit across conversations");
        auto unread_before_edit = conversation(b, group).unread;
        c.close();
        c_events.wait([&] { return c_events.disconnected == 2; });
        auto edited = call<chat::message>([&](auto handler)
                                          { a.edit_message(group, sent->message_id, "编辑后的第一条", handler); });
        require(edited && edited->edited_at && edited->text == "编辑后的第一条", "Author edit result");
        b_events.wait([&] { return !b_events.updates.empty() && b_events.updates.back().id == sent->message_id; });
        auto edited_again =
            call<chat::message>([&](auto handler) { a.edit_message(group, sent->message_id, "再次编辑", handler); });
        require(edited_again && edited_again->edited_at > edited->edited_at, "Monotonic edit timestamp");
        require(conversation(b, group).unread == unread_before_edit, "Editing does not add unread");
        auto quoted_history = call<chat::messages_result>([&](auto handler) { a.get_messages(group, {}, handler); });
        require(quoted_history && quoted_history->messages.back().reply &&
                    quoted_history->messages.back().reply->text == "再次编辑" &&
                    quoted_history->messages.back().reply->edited_at == edited_again->edited_at,
                "Edited quote history");
        c.connect(server.url);
        c_events.wait([&] { return c_events.connected == 3; });
        auto authenticated_again = call<chat::authentication_result>(
            [&](auto handler) { c.authenticate(names[2], "group password", handler); });
        require(authenticated_again && authenticated_again->authenticated, "Reconnect after edit");
        auto recovered_edit = call<chat::messages_result>([&](auto handler) { c.get_messages(group, {}, handler, 0); });
        require(recovered_edit && recovered_edit->messages.front().text == "再次编辑" &&
                    recovered_edit->messages.front().edited_at == edited_again->edited_at,
                "Recover old edited message");
        auto edited_direct = call<chat::message>(
            [&](auto handler) { b.edit_message(*direct, direct_reply->message_id, "edited direct reply", handler); });
        require(edited_direct && edited_direct->reply && conversation(a, *direct).last.text == "edited direct reply",
                "Direct edit and summary");
        auto unauthorized_delete =
            call<chat::message>([&](auto handler) { b.delete_message(group, sent->message_id, handler); });
        require(!unauthorized_delete && unauthorized_delete.error().code == -32007, "Cannot delete another author");
        auto outside_delete =
            call<chat::message>([&](auto handler) { d.delete_message(group, sent->message_id, handler); });
        require(!outside_delete && outside_delete.error().code == -32007, "Nonmember cannot delete");
        auto quoted_again = call<chat::send_message_result>(
            [&](auto handler) { c.send_message(group, "保留的引用", handler, sent->message_id); });
        require(quoted_again.has_value(), "Reply before deletion");
        auto unread_before_delete = conversation(c, group).unread;
        c.close();
        c_events.wait([&] { return c_events.disconnected == 3; });
        auto deleted = call<chat::message>([&](auto handler) { a.delete_message(group, sent->message_id, handler); });
        require(deleted && deleted->deleted && deleted->text.empty() && deleted->id == sent->message_id,
                "Author deletes body and preserves ID");
        auto deleted_reaction = call<chat::reaction_update>([&](auto handler) {
            b.set_message_reaction(group, sent->message_id, "👍", handler);
        });
        require(deleted->reaction_revision == 7 && deleted->reactions.empty() && !deleted_reaction &&
                deleted_reaction.error().code == -32007, "Deletion clears reactions and prevents further interaction");
        b_events.wait(
            [&]
            {
                return std::ranges::any_of(b_events.updates, [&](auto const& value)
                                           { return value.id == sent->message_id && value.deleted; });
            });
        auto repeated = call<chat::message>([&](auto handler) { a.delete_message(group, sent->message_id, handler); });
        require(repeated && repeated->deleted && repeated->reaction_revision == deleted->reaction_revision,
                "Repeated deletion remains deleted without advancing reaction revision");
        auto edit_deleted =
            call<chat::message>([&](auto handler) { a.edit_message(group, sent->message_id, "resurrect", handler); });
        require(!edit_deleted && edit_deleted.error().code == -32007, "Deleted message cannot be edited");
        auto delete_unread =
            call<chat::message>([&](auto handler) { b.delete_message(group, replied->message_id, handler); });
        require(delete_unread && delete_unread->deleted, "Deleted messages do not count as unread");
        auto deleted_quote_history =
            call<chat::messages_result>([&](auto handler) { a.get_messages(group, {}, handler); });
        require(deleted_quote_history && deleted_quote_history->messages.back().reply &&
                    deleted_quote_history->messages.back().reply->deleted &&
                    deleted_quote_history->messages.back().reply->text.empty(),
                "Deleted quote history hides original body");
        c.connect(server.url);
        c_events.wait([&] { return c_events.connected == 4; });
        auto auth_after_delete = call<chat::authentication_result>(
            [&](auto handler) { c.authenticate(names[2], "group password", handler); });
        require(auth_after_delete && auth_after_delete->authenticated, "Reconnect after deletion");
        require(conversation(c, group).unread == unread_before_delete - 1, "Deleted unread restored count");
        auto recovered_delete =
            call<chat::messages_result>([&](auto handler) { c.get_messages(group, {}, handler, 0); });
        require(recovered_delete && recovered_delete->messages.front().id == sent->message_id &&
                    recovered_delete->messages.front().deleted && recovered_delete->messages.front().text.empty(),
                "Recover old deleted message");
        auto deleted_direct =
            call<chat::message>([&](auto handler) { b.delete_message(*direct, direct_reply->message_id, handler); });
        require(deleted_direct && deleted_direct->deleted && conversation(a, *direct).last.deleted,
                "Direct deletion summary");
        require(call<std::int64_t>([&](auto handler) { a.mark_read(*direct, direct_reply->message_id, handler); })
                    .has_value(),
                "Read position can retain a deleted ID");
        for (int i = 0; i < 2; ++i)
        {
            auto large = call<chat::send_message_result>([&](auto handler)
                                                         { a.send_message(group, std::string(40000, 'x'), handler); });
            require(large.has_value(), "Legal long message send");
        }
        auto large_history = call<chat::messages_result>([&](auto handler) { c.get_messages(group, {}, handler); });
        require(large_history && large_history->messages.back().text.size() == 40000,
                "History response larger than a single message");
        auto large_recovery = call<chat::messages_result>(
            [&](auto handler) { c.get_messages(group, {}, handler, quoted_again->message_id); });
        require(large_recovery && large_recovery->messages.size() == 2 &&
                    large_recovery->messages.front().text.size() == 40000 && !large_recovery->has_more,
                "Large forward recovery page");
        auto second_group = call<std::int64_t>(
            [&](auto handler) { a.create_group("大摘要群", {data.users[1], data.users[2]}, handler); });
        require(second_group.has_value(), "Create another group for summaries");
        data.groups.push_back(*second_group);
        require(call<chat::send_message_result>([&](auto handler)
                                                { a.send_message(*second_group, std::string(40000, 'y'), handler); })
                    .has_value(),
                "Another legal long message");
        require(conversation(c, group).last.text.size() == 40000 &&
                    conversation(c, *second_group).last.text.size() == 40000,
                "Conversation summaries exceed a single message size");
        require(call<chat::user>([&](auto handler) { b.add_contact(data.users[0], handler); }).has_value(),
                "Create reverse contact");
        auto before_remove = call<chat::messages_result>([&](auto handler) { a.get_messages(*direct, {}, handler); });
        auto removed = call<bool>([&](auto handler) { a.remove_contact(data.users[1], handler); });
        require(removed && *removed, "Remove owned contact");
        auto removed_again = call<bool>([&](auto handler) { a.remove_contact(data.users[1], handler); });
        require(removed_again && !*removed_again, "Contact removal is idempotent");
        auto contacts_after_remove = call<std::vector<chat::user>>([&](auto handler) { a.get_contacts(handler); });
        auto reverse_contacts = call<std::vector<chat::user>>([&](auto handler) { b.get_contacts(handler); });
        require(contacts_after_remove && contacts_after_remove->size() == 1 &&
                    contacts_after_remove->front().id == data.users[2] && reverse_contacts &&
                    reverse_contacts->size() == 1 && reverse_contacts->front().id == data.users[0],
                "Only the caller's contact relation is removed");
        auto after_remove = call<chat::messages_result>([&](auto handler) { a.get_messages(*direct, {}, handler); });
        require(before_remove && after_remove && before_remove->messages.size() == after_remove->messages.size() &&
                    position(*before_remove, data.users[0]) == position(*after_remove, data.users[0]) &&
                    conversation(a, group).member_count == 3,
                "Removing contact preserves history, read position and group membership");
        auto searchable_again = call<std::vector<chat::user>>(
            [&](auto handler) { a.search_users(names[1], handler); });
        require(searchable_again && searchable_again->size() == 1 && searchable_again->front().id == data.users[1],
                "Removed contact can be added again");
        auto invalid_remove = call<bool>([&](auto handler) { a.remove_contact(0, handler); });
        auto self_remove = call<bool>([&](auto handler) { a.remove_contact(data.users[0], handler); });
        require(!invalid_remove && invalid_remove.error().code == -32602 && !self_remove &&
                    self_remove.error().code == -32602, "Invalid and self contact removal rejected");
        auto presence_after_remove = call<std::vector<chat::presence>>([&](auto handler) { a.get_presence(handler); });
        require(presence_after_remove && std::ranges::any_of(*presence_after_remove, [&](auto const& value)
                    { return value.user == data.users[1]; }),
                "Direct history retains presence after contact removal");
        auto removed_search = call<chat::messages_result>(
            [&](auto handler) { a.search_messages(*direct, "direct reply", {}, handler); });
        require(removed_search && removed_search->messages.empty(), "Deleted messages are excluded from search");
        auto literal = call<chat::send_message_result>([&](auto handler) {
            a.send_message(group, "100%_literal中文O'Reilly", handler);
        });
        require(literal.has_value(), "Literal search fixture");
        auto literal_search = call<chat::messages_result>(
            [&](auto handler) { b.search_messages(group, "%_literal中文O'Reilly", {}, handler); });
        auto other_conversation_search = call<chat::messages_result>(
            [&](auto handler) { a.search_messages(*direct, "中文", {}, handler); });
        require(literal_search && literal_search->messages.size() == 1 &&
                    literal_search->messages.front().id == literal->message_id && other_conversation_search &&
                    other_conversation_search->messages.empty(), "Unicode and punctuation are literal and scoped");
        auto direct_typing = call<bool>([&](auto handler) { a.set_typing(*direct, true, handler); });
        require(direct_typing && *direct_typing, "Direct conversation typing");
        b_events.wait([&] { return b_events.typing.size() == 3 && b_events.typing.back().conversation == *direct; });
        require(call<bool>([&](auto handler) { a.set_typing(*direct, false, handler); }).has_value(), "Direct typing stop");
        b_events.wait([&] { return b_events.typing.size() == 4 && !b_events.typing.back().typing; });
        {
            std::lock_guard lock(c_events.mutex);
            require(c_events.typing.size() == 2, "Typing is scoped to its conversation");
        }
        std::string file_bytes(3 * chat::attachment_chunk_size + 17, '\0');
        for (std::size_t i = 0; i < file_bytes.size(); ++i)
        {
            file_bytes[i] = static_cast<char>(i % 256);
        }
        auto file = call<chat::message>([&](auto handler) {
            a.send_attachment(group, "资料.bin", file_bytes, handler, literal->message_id);
        });
        require(file.has_value(), file ? "" : "Attachment send: " + file.error().message +
                    " (" + std::to_string(file.error().code) + ")");
        require(file && file->attachment && file->attachment->filename == "资料.bin" &&
                    file->attachment->size == static_cast<std::int64_t>(file_bytes.size()) &&
                    file->attachment->media_type == "application/octet-stream" && file->reply &&
                    file->reply->id == literal->message_id, "Atomic attachment message and reply");
        c_events.wait([&] { return std::ranges::any_of(c_events.messages, [&](auto const& value) {
            return value.id == file->id && value.attachment && value.attachment->filename == "资料.bin";
        }); });
        require(conversation(b, group).last.attachment && conversation(b, group).last.id == file->id,
                "Attachment metadata in conversation summary");
        auto const unread_before_download = conversation(c, group).unread;
        auto downloaded = call<std::string>([&](auto handler) { c.get_attachment(group, file->id, handler); });
        require(downloaded && *downloaded == file_bytes && conversation(c, group).unread == unread_before_download,
                "Chunked binary download preserves every byte and read position");
        auto file_history = call<chat::messages_result>(
            [&](auto handler) { b.get_messages(group, {}, handler, literal->message_id); });
        require(file_history && file_history->messages.size() == 1 && file_history->messages.front().attachment,
                "Attachment restored through forward history");
        auto file_search = call<chat::messages_result>(
            [&](auto handler) { b.search_messages(group, "资料.bin", {}, handler); });
        require(file_search && file_search->messages.size() == 1 && file_search->messages.front().attachment &&
                    file_search->messages.front().id == file->id, "Attachment filename search");
        auto forbidden_download = call<std::string>(
            [&](auto handler) { d.get_attachment(group, file->id, handler); });
        auto wrong_conversation_download = call<std::string>(
            [&](auto handler) { a.get_attachment(*direct, file->id, handler); });
        auto forbidden_upload = call<chat::message>(
            [&](auto handler) { d.send_attachment(group, "forbidden.bin", "data", handler); });
        auto edit_attachment = call<chat::message>(
            [&](auto handler) { a.edit_message(group, file->id, "rename", handler); });
        require(!forbidden_download && forbidden_download.error().code == -32007 && !wrong_conversation_download &&
                    wrong_conversation_download.error().code == -32007 && !forbidden_upload &&
                    forbidden_upload.error().code == -32006 && !edit_attachment && edit_attachment.error().code == -32007,
                "Attachment membership, conversation and immutability checks");
        auto failed_file_reply = call<chat::message>([&](auto handler) {
            a.send_attachment(group, "aborted.bin", "abc", handler, direct_sent->message_id);
        });
        require(!failed_file_reply && failed_file_reply.error().code == -32602, "Attachment cross-conversation reply rejected");
        auto empty_file = call<chat::message>(
            [&](auto handler) { a.send_attachment(*direct, "empty.bin", "", handler); });
        require(empty_file && empty_file->attachment && empty_file->attachment->size == 0,
                "Empty file and next upload after cancelled failure");
        auto downloaded_empty = call<std::string>(
            [&](auto handler) { b.get_attachment(*direct, empty_file->id, handler); });
        require(downloaded_empty && downloaded_empty->empty(), "Empty attachment download");
        auto bad_filename = call<chat::message>(
            [&](auto handler) { a.send_attachment(group, "../bad.bin", "data", handler); });
        auto too_large = call<chat::message>([&](auto handler) {
            a.send_attachment(group, "large.bin", std::string(chat::max_attachment_size + 1, 'x'), handler);
        });
        require(!bad_filename && bad_filename.error().code == -32602 && !too_large,
                "Unsafe filename and oversized attachment rejected");
        auto file_reply = call<chat::send_message_result>(
            [&](auto handler) { b.send_message(group, "引用文件", handler, file->id); });
        require(file_reply && file_reply->reply && file_reply->reply->text == "资料.bin", "Reply to attachment");
        auto file_deleted = call<chat::message>(
            [&](auto handler) { a.delete_message(group, file->id, handler); });
        require(file_deleted && file_deleted->deleted && !file_deleted->attachment, "Attachment deletion placeholder");
        auto unavailable_file = call<std::string>([&](auto handler) { b.get_attachment(group, file->id, handler); });
        require(!unavailable_file && unavailable_file.error().code == -32007, "Deleted attachment cannot be downloaded");
        auto cleared_query = "SELECT count(*) FROM message_attachments WHERE message_id=" + std::to_string(file->id);
        std::unique_ptr<PGresult, decltype(&PQclear)> cleared(PQexec(data.database.get(), cleared_query.c_str()), &PQclear);
        require(cleared && PQresultStatus(cleared.get()) == PGRES_TUPLES_OK &&
                    std::string_view(PQgetvalue(cleared.get(), 0, 0)) == "0", "Deleted attachment bytes are removed");
        auto file_reply_history = call<chat::messages_result>([&](auto handler) { c.get_messages(group, {}, handler); });
        require(file_reply_history && file_reply_history->messages.back().reply &&
                    file_reply_history->messages.back().reply->deleted, "Deleted attachment reply placeholder");
        auto image_file = call<chat::message>([&](auto handler) {
            a.send_attachment(*direct, "detected.bin", std::string("\x89PNG\r\n\x1a\n", 8), handler);
        });
        require(image_file && image_file->attachment && image_file->attachment->media_type == "image/png",
                "Image type detected from bytes rather than filename");
        auto const boundary_start = std::chrono::steady_clock::now();
        auto const png_bytes = *chat::detail::decode_base64(avatar_png_base64);
        auto boundary_bytes = png_bytes + std::string(chat::max_attachment_size - png_bytes.size(), '\0');
        auto boundary_image = call<chat::message>([&](auto handler) {
            a.send_attachment(*direct, "boundary.png", boundary_bytes, handler);
        }, std::chrono::seconds(30));
        require(boundary_image && boundary_image->attachment &&
                boundary_image->attachment->size == static_cast<std::int64_t>(chat::max_attachment_size),
                "Exactly 10 MiB PNG attachment is accepted");
        auto const download_start = std::chrono::steady_clock::now();
        auto first_download = std::async(std::launch::async, [&] {
            return call<std::string>([&](auto handler) { b.get_attachment(*direct, boundary_image->id, handler); }, std::chrono::seconds(30));
        });
        auto second_download = call<std::string>([&](auto handler) { b.get_attachment(*direct, boundary_image->id, handler); }, std::chrono::seconds(30));
        auto completed_download = first_download.get();
        require(completed_download && second_download && *completed_download == boundary_bytes && *second_download == boundary_bytes,
                "Concurrent downloads from one client preserve both complete 10 MiB results");
        std::cout << "ATTACHMENT_BOUNDARY bytes=" << boundary_bytes.size() << " upload_ms="
            << std::chrono::duration<double, std::milli>(download_start - boundary_start).count() << " two_downloads_ms="
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - download_start).count() << '\n';
        auto nul_message = call<chat::send_message_result>(
            [&](auto handler) { a.send_message(group, std::string("a\0b", 3), handler); });
        auto nul_edit = call<chat::message>(
            [&](auto handler) { a.edit_message(group, literal->message_id, std::string("a\0b", 3), handler); });
        require(!nul_message && nul_message.error().code == -32602 && !nul_edit && nul_edit.error().code == -32602,
                "Binary data cannot be silently truncated through text messages");
        {
            std::lock_guard lock(d_events.mutex);
            require(d_events.messages.empty() && d_events.updates.empty() && d_events.reads.empty() &&
                        d_events.conversations.empty() && d_events.typing.empty(),
                    "Nonmember receives no notifications");
        }
        {
            auto const base = "chat_mention_" + std::to_string(getpid());
            std::vector<std::string> special_names{base, base + ".plus", "名 字_" + base,
                "r(.*)[z]\\_'" + base, base + std::string(2000, 'x')};
            std::vector<std::int64_t> special_ids;
            std::vector<std::int64_t> mention_members{data.users[1], data.users[2]};
            require(call<chat::user>([&](auto handler) { a.add_contact(data.users[1], handler); }).has_value(),
                "Restore contact removed by the earlier contact lifecycle test");
            for (auto const& name : special_names)
            {
                auto const* value = name.c_str();
                std::unique_ptr<PGresult, decltype(&PQclear)> inserted(PQexecParams(data.database.get(),
                    "INSERT INTO users(username,password_hash) VALUES($1,repeat('x',60)) RETURNING id", 1,
                    nullptr, &value, nullptr, nullptr, 0), &PQclear);
                require(inserted && PQresultStatus(inserted.get()) == PGRES_TUPLES_OK && PQntuples(inserted.get()) == 1,
                    "Create literal Unicode, whitespace and regex username fixtures");
                auto const id = std::stoll(PQgetvalue(inserted.get(), 0, 0));
                data.users.push_back(id);
                special_ids.push_back(id);
                mention_members.push_back(id);
                require(call<chat::user>([&](auto handler) { a.add_contact(id, handler); }).has_value(), "Mention fixture contact");
            }
            auto created_mentions = call<std::int64_t>([&](auto handler) { a.create_group("mention", mention_members, handler); });
            require(created_mentions.has_value(), "Create mention group");
            auto const mention_group = *created_mentions;
            data.groups.push_back(mention_group);
            auto targets = [](std::vector<chat::mention> const& mentions) {
                std::vector<std::int64_t> ids;
                for (auto const& mention : mentions) { ids.push_back(mention.user); }
                return ids;
            };
            auto const text = "@" + names[1] + " @" + names[2] + " @" + names[1] + " @" + names[3];
            auto mentioned = call<chat::send_message_result>([&](auto handler) { a.send_message(mention_group, text, handler); });
            require(mentioned && targets(mentioned->mentions) == std::vector<std::int64_t>{data.users[1], data.users[2]},
                "Server persists exact unique current group targets and ignores outsiders");
            b_events.wait([&] { return std::any_of(b_events.messages.begin(), b_events.messages.end(),
                [&](auto const& message) { return message.id == mentioned->message_id; }); });
            {
                std::lock_guard lock(b_events.mutex);
                auto value = std::find_if(b_events.messages.begin(), b_events.messages.end(),
                    [&](auto const& message) { return message.id == mentioned->message_id; });
                require(targets(value->mentions) == targets(mentioned->mentions), "Realtime message carries persisted targets");
            }
            auto searched = call<chat::messages_result>([&](auto handler) { a.search_messages(mention_group, names[1], {}, handler); });
            require(searched && searched->messages.size() == 1 && targets(searched->messages.front().mentions) == targets(mentioned->mentions) &&
                targets(conversation(a, mention_group).last.mentions) == targets(mentioned->mentions), "Search and latest message share persisted mention data");
            auto literal_mentions = call<chat::send_message_result>([&](auto handler) {
                a.send_message(mention_group, "@" + special_names[1] + " @" + special_names[2] + " @" + special_names[3], handler);
            });
            require(literal_mentions && targets(literal_mentions->mentions) ==
                std::vector<std::int64_t>{special_ids[1], special_ids[2], special_ids[3]},
                "Full longest usernames match literally, including Unicode, spaces and regex metacharacters");
            auto boundaries = call<chat::send_message_result>([&](auto handler) {
                a.send_message(mention_group, "mail@" + base + " @" + base + "_extra @@" + base + " @CHAT_MENTION_" + std::to_string(getpid()), handler);
            });
            require(boundaries && boundaries->mentions.empty(), "Email, word substring, double at-sign and case mismatch are not mentions");
            auto edited_mention = call<chat::message>([&](auto handler) {
                a.edit_message(mention_group, literal_mentions->message_id, "edited @" + base, handler);
            });
            require(edited_mention && targets(edited_mention->mentions) == std::vector<std::int64_t>{special_ids[0]},
                "Editing atomically replaces original mention targets");
            auto deleted_mention = call<chat::message>([&](auto handler) { a.delete_message(mention_group, literal_mentions->message_id, handler); });
            require(deleted_mention && deleted_mention->mentions.empty(), "Deleted message loses interactive mention metadata");
            auto before_large = conversation(a, mention_group);
            auto oversized = call<chat::send_message_result>([&](auto handler) {
                a.send_message(mention_group, std::string(63000, 'x') + " @" + special_names.back(), handler);
            });
            require(!oversized && oversized.error().code == -32602 && conversation(a, mention_group).last.id == before_large.last.id,
                "Mention-expanded 64 KiB payload is rejected without committing message or targets");
            require(call<bool>([&](auto handler) { b.leave_group(mention_group, handler); }).has_value(), "Mention target leaves group");
            auto left_target = call<chat::send_message_result>([&](auto handler) { a.send_message(mention_group, "left @" + names[1], handler); });
            auto old_history = call<chat::messages_result>([&](auto handler) { a.get_messages(mention_group, {}, handler); });
            require(left_target && left_target->mentions.empty() && old_history &&
                targets(old_history->messages.front().mentions) == targets(mentioned->mentions),
                "New messages ignore exited members while prior persisted targets remain historical facts");
            require(call<bool>([&](auto handler) { a.invite_group_members(mention_group, {data.users[1]}, handler); }).has_value(), "Reinvite mention target");
            auto rejoined_mention = call<chat::send_message_result>([&](auto handler) { a.send_message(mention_group, "rejoined @" + names[1], handler); });
            require(rejoined_mention && targets(rejoined_mention->mentions) == std::vector<std::int64_t>{data.users[1]} &&
                conversation(b, mention_group).unread == 1, "Rejoined member is mentionable without counting old history as unread");
            require(call<bool>([&](auto h) { a.set_group_admin(mention_group, data.users[1], true, h); }).has_value(),
                "Promote group-pin administrator");
            auto member_pin = call<bool>([&](auto h) { c.pin_group_message(mention_group, mentioned->message_id, h); });
            auto member_unpin = call<bool>([&](auto h) { c.unpin_group_message(mention_group, h); });
            auto outsider_pin = call<bool>([&](auto h) { d.pin_group_message(mention_group, mentioned->message_id, h); });
            auto direct_pin_history = call<chat::messages_result>([&](auto h) { a.get_messages(*direct, {}, h); });
            require(direct_pin_history && !direct_pin_history->messages.empty(), "Cross-conversation pin fixture");
            auto cross_pin = call<bool>([&](auto h) { a.pin_group_message(mention_group, direct_pin_history->messages.front().id, h); });
            auto deleted_pin = call<bool>([&](auto h) { a.pin_group_message(mention_group, literal_mentions->message_id, h); });
            auto direct_pin = call<bool>([&](auto h) { a.pin_group_message(*direct, mentioned->message_id, h); });
            require(!member_pin && member_pin.error().code == -32009 && !member_unpin && member_unpin.error().code == -32009 &&
                !outsider_pin && outsider_pin.error().code == -32006 && !deleted_pin && deleted_pin.error().code == -32007 &&
                !cross_pin && cross_pin.error().code == -32007 &&
                !direct_pin && direct_pin.error().code == -32006, "Group pin requires current owner/admin and a live same-group message");
            auto before_pin = conversation(c, mention_group);
            require(before_pin.announcement.empty(), "Groups default to no announcement");
            auto member_announcement = call<bool>([&](auto h) { c.set_group_announcement(mention_group, "denied", h); });
            auto outsider_announcement = call<bool>([&](auto h) { d.set_group_announcement(mention_group, "denied", h); });
            auto direct_announcement = call<bool>([&](auto h) { a.set_group_announcement(*direct, "denied", h); });
            require(!member_announcement && member_announcement.error().code == -32009 &&
                !outsider_announcement && outsider_announcement.error().code == -32006 &&
                !direct_announcement && direct_announcement.error().code == -32006,
                "Only current group owner/admin may change announcements");
            std::array<std::size_t, 2> before_announcement_events;
            {
                std::lock_guard lock(c_events.mutex);
                before_announcement_events = {c_events.conversations.size(), c_events.messages.size()};
            }
            auto announcement = call<bool>([&](auto h) { a.set_group_announcement(mention_group, "群公告\n<纯文本>", h); });
            require(announcement && *announcement, "Owner saves a current plaintext announcement");
            c_events.wait([&] { return c_events.conversations.size() > before_announcement_events[0]; });
            auto with_announcement = conversation(c, mention_group);
            require(with_announcement.announcement == "群公告\n<纯文本>" && with_announcement.last.id == before_pin.last.id &&
                with_announcement.unread == before_pin.unread && with_announcement.member_count == before_pin.member_count,
                "Announcement refresh does not change messages, unread or membership");
            auto same_announcement = call<bool>([&](auto h) { b.set_group_announcement(mention_group, "群公告\n<纯文本>", h); });
            require(same_announcement && !*same_announcement && conversation(c, mention_group).announcement == "群公告\n<纯文本>",
                "Announcement no-op does not fall through to leave");
            {
                std::lock_guard lock(c_events.mutex);
                require(c_events.conversations.size() == before_announcement_events[0] + 1 &&
                    c_events.messages.size() == before_announcement_events[1], "Announcement no-op sends no event and updates never send a message");
            }
            auto max_announcement = call<bool>([&](auto h) { b.set_group_announcement(mention_group, std::string(4096, 'x'), h); });
            auto oversized_announcement = call<bool>([&](auto h) { a.set_group_announcement(mention_group, std::string(4097, 'x'), h); });
            auto nul_announcement = call<bool>([&](auto h) { a.set_group_announcement(mention_group, std::string("a\0b", 3), h); });
            require(max_announcement && *max_announcement && !oversized_announcement && oversized_announcement.error().code == -32602 &&
                !nul_announcement && nul_announcement.error().code == -32602 && conversation(c, mention_group).announcement.size() == 4096,
                "UTF-8 byte limit and NUL validation preserve the last valid announcement");
            std::size_t before_pin_events;
            {
                std::lock_guard lock(b_events.mutex);
                before_pin_events = b_events.conversations.size();
            }
            auto pinned = call<bool>([&](auto h) { a.pin_group_message(mention_group, mentioned->message_id, h); });
            require(pinned && *pinned, "Owner pins a group message");
            b_events.wait([&] { return b_events.conversations.size() > before_pin_events; });
            auto snapshot_pin = conversation(c, mention_group);
            require(snapshot_pin.pinned_message && snapshot_pin.pinned_message->id == mentioned->message_id &&
                snapshot_pin.last.id == before_pin.last.id && snapshot_pin.unread == before_pin.unread,
                "Members see pinned summary independently of latest message and unread");
            auto unchanged_pin = call<bool>([&](auto h) { b.pin_group_message(mention_group, mentioned->message_id, h); });
            auto replaced_pin = call<bool>([&](auto h) { b.pin_group_message(mention_group, rejoined_mention->message_id, h); });
            auto pin_edit = call<chat::message>([&](auto h) {
                a.edit_message(mention_group, rejoined_mention->message_id, "rejoined edited @" + names[1], h);
            });
            auto edited_pin = conversation(c, mention_group);
            require(unchanged_pin && !*unchanged_pin && replaced_pin && *replaced_pin && pin_edit && edited_pin.pinned_message &&
                edited_pin.pinned_message->id == rejoined_mention->message_id && edited_pin.pinned_message->text == pin_edit->text,
                "Admin replaces single pinned message; edits refresh its summary");
            b.close();
            b_events.wait([&] { return b_events.disconnected == 2; });
            require(call<bool>([&](auto h) { a.set_group_announcement(mention_group, "离线期间更新的公告", h); }).value(),
                "Change announcement while administrator is offline");
            b.connect(server.url);
            b_events.wait([&] { return b_events.connected == 3; });
            require(call<chat::authentication_result>([&](auto handler) { b.authenticate(names[1], "group password", handler); })->authenticated,
                "Reconnect mention recipient");
            auto recovered_mentions = call<chat::messages_result>([&](auto handler) { b.get_messages(mention_group, {}, handler); });
            auto paged_mentions = call<chat::messages_result>([&](auto handler) { b.get_messages(mention_group, rejoined_mention->message_id, handler); });
            require(conversation(b, mention_group).pinned_message &&
                conversation(b, mention_group).pinned_message->id == rejoined_mention->message_id &&
                conversation(b, mention_group).announcement == "离线期间更新的公告",
                "Reconnect restores pinned message from authoritative conversation snapshot");
            require(recovered_mentions && targets(recovered_mentions->messages.back().mentions) == targets(rejoined_mention->mentions) &&
                paged_mentions && targets(paged_mentions->messages.front().mentions) == targets(mentioned->mentions),
                "Reconnect and cursor history retain persisted targets");
            auto direct_mention = call<chat::send_message_result>([&](auto handler) { a.send_message(*direct, "direct @" + names[1], handler); });
            auto filename_mention = call<chat::message>([&](auto handler) { a.send_attachment(mention_group, "@" + names[1], "file", handler); });
            require(direct_mention && direct_mention->mentions.empty() && filename_mention && filename_mention->mentions.empty(),
                "Direct text and attachment filenames do not create group mentions");
            auto unpin = call<bool>([&](auto h) { b.unpin_group_message(mention_group, h); });
            auto repeated_unpin = call<bool>([&](auto h) { a.unpin_group_message(mention_group, h); });
            require(unpin && *unpin && repeated_unpin && !*repeated_unpin && !conversation(c, mention_group).pinned_message,
                "Admin unpins; repeated unpin is idempotent");
            require(call<bool>([&](auto h) { a.pin_group_message(mention_group, rejoined_mention->message_id, h); }).has_value() &&
                call<chat::message>([&](auto h) { a.delete_message(mention_group, rejoined_mention->message_id, h); }).has_value() &&
                !conversation(c, mention_group).pinned_message, "Deleting current pinned message atomically clears the reference");
            auto clear_announcement = call<bool>([&](auto h) { b.set_group_announcement(mention_group, "", h); });
            auto repeated_clear = call<bool>([&](auto h) { a.set_group_announcement(mention_group, "", h); });
            require(clear_announcement && *clear_announcement && repeated_clear && !*repeated_clear &&
                conversation(c, mention_group).announcement.empty() && conversation(b, mention_group).member_count == before_pin.member_count,
                "Admin clears announcement and repeat clear preserves current membership");
            std::cout << "PASS group announcement permission, persistence, notification, limits, no-op and reconnect\n";
            auto no_link = call<std::optional<std::string>>([&](auto h) { a.get_group_invite(mention_group, h); });
            auto member_link = call<std::optional<std::string>>([&](auto h) { c.get_group_invite(mention_group, h); });
            auto outsider_link = call<std::optional<std::string>>([&](auto h) { d.create_group_invite(mention_group, h); });
            auto direct_link = call<std::optional<std::string>>([&](auto h) { a.create_group_invite(*direct, h); });
            require(no_link && !*no_link && !member_link && member_link.error().code == -32009 &&
                !outsider_link && outsider_link.error().code == -32006 && !direct_link && direct_link.error().code == -32006,
                "Only current group managers may view or create invite secrets; existing groups default to no link");
            auto created_link = call<std::optional<std::string>>([&](auto h) { a.create_group_invite(mention_group, h); });
            require(created_link && *created_link && (**created_link).size() == 64, "Owner creates high-entropy invite token");
            auto const token = **created_link;
            auto owner_link = call<std::optional<std::string>>([&](auto h) { a.get_group_invite(mention_group, h); });
            require(owner_link && *owner_link == std::optional<std::string>(token), "Owner reads the persistent current token");
            auto admin_link = call<std::optional<std::string>>([&](auto h) { b.create_group_invite(mention_group, h); });
            require(admin_link && *admin_link == std::optional<std::string>(token), "Create is stable and administrator can recover current link");
            auto invalid_link = call<chat::group_join_result>([&](auto h) { d.join_group(std::string(64, 'a'), h); });
            require(!invalid_link && invalid_link.error().code == -32014, "Unknown opaque token cannot join a group");
            auto const before_join = conversation(a, mention_group);
            std::size_t before_join_events;
            { std::lock_guard lock(c_events.mutex); before_join_events = c_events.conversations.size(); }
            auto joined = call<chat::group_join_result>([&](auto h) { d.join_group(token, h); });
            require(joined && joined->conversation == mention_group && joined->title == before_join.username &&
                joined->member_count == before_join.member_count + 1 && joined->state == chat::group_join_state::joined,
                "Authenticated non-contact joins with authoritative title/count");
            c_events.wait([&] { return c_events.conversations.size() > before_join_events; });
            auto joined_history = call<chat::messages_result>([&](auto h) { d.get_messages(mention_group, {}, h); });
            require(joined_history && !joined_history->messages.empty() && position(*joined_history, data.users[3]) == 0 &&
                conversation(d, mention_group).unread == 0 && conversation(a, mention_group).last.id == before_join.last.id,
                "Link join exposes old history without pretending it was read or changing activity");
            auto repeated_join = call<chat::group_join_result>([&](auto h) { d.join_group(token, h); });
            require(repeated_join && repeated_join->state == chat::group_join_state::member && repeated_join->member_count == joined->member_count,
                "Existing membership joins idempotently without duplicate members");
            auto after_link = call<chat::send_message_result>([&](auto h) { a.send_message(mention_group, "after link join", h); });
            require(after_link && conversation(d, mention_group).unread == 1, "Only new messages count unread after link join");
            d_events.wait([&] { return std::any_of(d_events.messages.begin(), d_events.messages.end(), [&](auto const& value) { return value.id == after_link->message_id; }); });
            require(call<bool>([&](auto h) { d.mark_read(mention_group, after_link->message_id, h); }).has_value() &&
                call<bool>([&](auto h) { d.set_conversation_muted(mention_group, true, h); }).has_value() &&
                call<bool>([&](auto h) { d.set_conversation_pinned(mention_group, true, h); }).has_value() &&
                call<bool>([&](auto h) { a.set_group_admin(mention_group, data.users[3], true, h); }).has_value(),
                "Set real read position, preferences and administrator role before repeat join");
            auto repeat_admin = call<chat::group_join_result>([&](auto h) { d.join_group(token, h); });
            auto preserved_history = call<chat::messages_result>([&](auto h) { d.get_messages(mention_group, {}, h); });
            auto preserved_members = call<std::vector<chat::conversation_member>>([&](auto h) { d.get_members(mention_group, h); });
            auto preserved = conversation(d, mention_group);
            require(repeat_admin && repeat_admin->state == chat::group_join_state::member && preserved_history &&
                position(*preserved_history, data.users[3]) == after_link->message_id && preserved.muted && preserved.pinned && preserved_members &&
                std::any_of(preserved_members->begin(), preserved_members->end(), [&](auto const& value) { return value.id == data.users[3] && value.role == chat::member_role::admin; }),
                "Repeat link join preserves role, read position and personal preferences");
            require(call<bool>([&](auto h) { a.remove_group_member(mention_group, data.users[3], h); }).value(), "Remove link-joined administrator");
            auto rejoined = call<chat::group_join_result>([&](auto h) { d.join_group(token, h); });
            auto rejoined_members = call<std::vector<chat::conversation_member>>([&](auto h) { d.get_members(mention_group, h); });
            auto rejoined_history = call<chat::messages_result>([&](auto h) { d.get_messages(mention_group, {}, h); });
            auto fresh = conversation(d, mention_group);
            require(rejoined && rejoined->state == chat::group_join_state::joined && !fresh.muted && !fresh.pinned && fresh.unread == 0 &&
                rejoined_history && position(*rejoined_history, data.users[3]) == 0 && rejoined_members &&
                std::any_of(rejoined_members->begin(), rejoined_members->end(), [&](auto const& value) { return value.id == data.users[3] && value.role == chat::member_role::member; }),
                "Link rejoin creates a fresh ordinary membership with current watermark, zero real read and default preferences");
            auto revoked = call<std::optional<std::string>>([&](auto h) { b.revoke_group_invite(mention_group, h); });
            auto repeated_revoke = call<std::optional<std::string>>([&](auto h) { a.revoke_group_invite(mention_group, h); });
            auto stale_link = call<chat::group_join_result>([&](auto h) { d.join_group(token, h); });
            require(revoked && !*revoked && repeated_revoke && !*repeated_revoke && !stale_link && stale_link.error().code == -32014,
                "Administrator revokes link idempotently and old secret no longer joins");
            auto replacement = call<std::optional<std::string>>([&](auto h) { b.create_group_invite(mention_group, h); });
            require(replacement && *replacement && **replacement != token, "Recreating revoked link generates a different random secret");
            int d_connects, d_disconnects;
            { std::lock_guard lock(d_events.mutex); d_connects = d_events.connected; d_disconnects = d_events.disconnected; }
            d.close();
            d_events.wait([&] { return d_events.disconnected == d_disconnects + 1; });
            d.connect(server.url);
            d_events.wait([&] { return d_events.connected == d_connects + 1; });
            auto d_auth = call<chat::authentication_result>([&](auto h) { d.authenticate(names[3], "group password", h); });
            auto restored_join = call<chat::group_join_result>([&](auto h) { d.join_group(**replacement, h); });
            require(d_auth && d_auth->authenticated && conversation(d, mention_group).unread == 0 &&
                restored_join && restored_join->state == chat::group_join_state::member,
                "Reconnect restores link-created membership and persistent invite token");
            std::cout << "PASS group invite secret permissions, stable creation/revoke, direct join, fresh rejoin, notifications and reconnect\n";
            require(!conversation(a, mention_group).join_approval, "Existing group defaults to direct invitation join");
            auto member_approval = call<bool>([&](auto h) { c.set_group_join_approval(mention_group, true, h); });
            auto direct_approval = call<bool>([&](auto h) { a.set_group_join_approval(*direct, true, h); });
            require(!member_approval && member_approval.error().code == -32009 && !direct_approval && direct_approval.error().code == -32006,
                "Only group managers may configure link approval");
            require(call<bool>([&](auto h) { a.set_group_join_approval(mention_group, true, h); }).value() &&
                !call<bool>([&](auto h) { b.set_group_join_approval(mention_group, true, h); }).value() &&
                conversation(c, mention_group).join_approval, "Owner enables approval; administrator repeat is idempotent");
            require(call<chat::group_join_result>([&](auto h) { d.join_group(**replacement, h); })->state == chat::group_join_state::member,
                "Approval does not turn existing membership into an application");
            require(call<bool>([&](auto h) { a.remove_group_member(mention_group, data.users[3], h); }).value(), "Remove applicant fixture");
            std::size_t ordinary_requests, requester_requests, manager_requests;
            { std::lock_guard lock(c_events.mutex); ordinary_requests = c_events.join_requests.size(); }
            { std::lock_guard lock(d_events.mutex); requester_requests = d_events.join_requests.size(); }
            { std::lock_guard lock(b_events.mutex); manager_requests = b_events.join_requests.size(); }
            auto pending_join = call<chat::group_join_result>([&](auto h) { d.join_group(**replacement, h); });
            require(pending_join && pending_join->state == chat::group_join_state::pending &&
                pending_join->member_count == before_join.member_count, "Pending join does not create a membership");
            b_events.wait([&] { return b_events.join_requests.size() > manager_requests; });
            d_events.wait([&] { return d_events.join_requests.size() > requester_requests; });
            auto requests = call<chat::group_join_requests_result>([&](auto h) { a.get_group_join_requests(mention_group, {}, h); });
            require(requests && requests->requests.size() == 1 && !requests->next &&
                requests->requests.front().applicant.id == data.users[3] && requests->requests.front().applicant.username == names[3],
                "Manager sees actual applicant metadata");
            auto const requested_at = requests->requests.front().created_at;
            auto duplicate_pending = call<chat::group_join_result>([&](auto h) { d.join_group(**replacement, h); });
            auto duplicate_list = call<chat::group_join_requests_result>([&](auto h) { b.get_group_join_requests(mention_group, {}, h); });
            require(duplicate_pending && duplicate_pending->state == chat::group_join_state::pending && duplicate_list &&
                duplicate_list->requests.size() == 1 && duplicate_list->requests.front().created_at == requested_at,
                "Repeated pending join preserves the single application and creation time");
            auto private_requests = call<chat::group_join_requests_result>([&](auto h) { c.get_group_join_requests(mention_group, {}, h); });
            auto private_response = call<bool>([&](auto h) { c.respond_group_join_request(mention_group, data.users[3], true, h); });
            auto pending_requests = call<chat::group_join_requests_result>([&](auto h) { d.get_group_join_requests(mention_group, {}, h); });
            auto denied_send = call<chat::send_message_result>([&](auto h) { d.send_message(mention_group, "pending send", h); });
            auto denied_history = call<chat::messages_result>([&](auto h) { d.get_messages(mention_group, {}, h); });
            auto denied_members = call<std::vector<chat::conversation_member>>([&](auto h) { d.get_members(mention_group, h); });
            auto denied_search = call<chat::messages_result>([&](auto h) { d.search_messages(mention_group, "link", {}, h); });
            auto denied_read = call<std::int64_t>([&](auto h) { d.mark_read(mention_group, after_link->message_id, h); });
            auto denied_file = call<chat::message>([&](auto h) { d.send_attachment(mention_group, "pending.bin", "pending", h); });
            require(!private_requests && private_requests.error().code == -32009 && !private_response && private_response.error().code == -32009 &&
                !pending_requests && pending_requests.error().code == -32006 && !denied_send && !denied_history && !denied_members &&
                !denied_search && !denied_read && !denied_file, "Pending applicant has no group data or message permissions");
            int pending_connects, pending_disconnects;
            { std::lock_guard lock(d_events.mutex); pending_connects = d_events.connected; pending_disconnects = d_events.disconnected; }
            d.close();
            d_events.wait([&] { return d_events.disconnected == pending_disconnects + 1; });
            d.connect(server.url);
            d_events.wait([&] { return d_events.connected == pending_connects + 1; });
            auto pending_auth = call<chat::authentication_result>([&](auto h) { d.authenticate(names[3], "group password", h); });
            auto pending_snapshot = call<chat::conversations_result>([&](auto h) { d.get_conversations({}, h); });
            auto recovered_request = call<chat::group_join_requests_result>([&](auto h) { a.get_group_join_requests(mention_group, {}, h); });
            require(pending_auth && pending_auth->authenticated && pending_snapshot &&
                std::ranges::none_of(pending_snapshot->conversations, [&](auto const& value) { return value.id == mention_group; }) &&
                recovered_request && recovered_request->requests.size() == 1 && recovered_request->requests.front().created_at == requested_at,
                "Reconnect preserves pending relation without granting a conversation");
            auto rejected = call<bool>([&](auto h) { b.respond_group_join_request(mention_group, data.users[3], false, h); });
            d_events.wait([&] { return std::ranges::any_of(d_events.join_requests, [&](auto const& value) {
                return value.conversation == mention_group && value.state == chat::group_join_request_state::rejected;
            }); });
            require(rejected && *rejected && !call<bool>([&](auto h) { a.respond_group_join_request(mention_group, data.users[3], false, h); }).value() &&
                call<chat::group_join_requests_result>([&](auto h) { a.get_group_join_requests(mention_group, {}, h); })->requests.empty(),
                "Administrator rejects, applicant receives result, repeated decision is idempotent");
            require(call<chat::group_join_result>([&](auto h) { d.join_group(**replacement, h); })->state == chat::group_join_state::pending,
                "Rejected user may submit a new application");
            std::size_t before_pending_messages;
            { std::lock_guard lock(d_events.mutex); before_pending_messages = d_events.messages.size(); }
            auto before_accept = call<chat::send_message_result>([&](auto h) { a.send_message(mention_group, "before approval acceptance", h); });
            require(before_accept && call<bool>([&](auto h) { a.respond_group_join_request(mention_group, data.users[3], true, h); }).value(),
                "Owner accepts a pending application");
            d_events.wait([&] { return std::ranges::any_of(d_events.join_requests, [&](auto const& value) {
                return value.conversation == mention_group && value.state == chat::group_join_request_state::accepted;
            }); });
            auto accepted_history = call<chat::messages_result>([&](auto h) { d.get_messages(mention_group, {}, h); });
            auto accepted_members = call<std::vector<chat::conversation_member>>([&](auto h) { d.get_members(mention_group, h); });
            auto accepted_snapshot = conversation(d, mention_group);
            require(accepted_history && position(*accepted_history, data.users[3]) == 0 && accepted_members &&
                std::ranges::any_of(*accepted_members, [&](auto const& value) { return value.id == data.users[3] && value.role == chat::member_role::member; }) &&
                accepted_snapshot.unread == 0 && !accepted_snapshot.muted && !accepted_snapshot.pinned && accepted_snapshot.join_approval &&
                call<chat::group_join_requests_result>([&](auto h) { b.get_group_join_requests(mention_group, {}, h); })->requests.empty(),
                "Acceptance atomically removes request and creates fresh ordinary membership with zero real read and latest unread watermark");
            { std::lock_guard lock(d_events.mutex); require(d_events.messages.size() == before_pending_messages, "Pending applicant is isolated from group realtime messages"); }
            { std::lock_guard lock(c_events.mutex); require(c_events.join_requests.size() == ordinary_requests, "Ordinary group members never receive private join applications or results"); }
            auto after_accept = call<chat::send_message_result>([&](auto h) { a.send_message(mention_group, "after approval acceptance", h); });
            require(after_accept && conversation(d, mention_group).unread == 1, "Only post-acceptance messages count unread");
            d_events.wait([&] { return std::ranges::any_of(d_events.messages, [&](auto const& value) { return value.id == after_accept->message_id; }); });
            require(call<bool>([&](auto h) { a.remove_group_member(mention_group, data.users[3], h); }).value() &&
                call<chat::group_join_result>([&](auto h) { d.join_group(**replacement, h); })->state == chat::group_join_state::pending,
                "Rejoin after removal again requires approval");
            require(call<chat::user>([&](auto h) { a.add_contact(data.users[3], h); }).has_value() &&
                call<bool>([&](auto h) { a.invite_group_members(mention_group, {data.users[3]}, h); }).value() &&
                call<chat::group_join_requests_result>([&](auto h) { a.get_group_join_requests(mention_group, {}, h); })->requests.empty(),
                "Explicit contact invitation bypasses link approval and atomically clears an existing application");
            require(call<bool>([&](auto h) { a.remove_group_member(mention_group, data.users[3], h); }).value() &&
                call<chat::group_join_result>([&](auto h) { d.join_group(**replacement, h); })->state == chat::group_join_state::pending,
                "Create pending request before changing mode");
            require(call<std::optional<std::string>>([&](auto h) { a.revoke_group_invite(mention_group, h); }).has_value() &&
                call<chat::group_join_requests_result>([&](auto h) { a.get_group_join_requests(mention_group, {}, h); })->requests.size() == 1,
                "Revocation blocks new use without deleting a previously submitted application");
            auto approval_link = call<std::optional<std::string>>([&](auto h) { a.create_group_invite(mention_group, h); });
            require(approval_link && *approval_link && call<bool>([&](auto h) { b.set_group_join_approval(mention_group, false, h); }).value() &&
                call<chat::group_join_requests_result>([&](auto h) { a.get_group_join_requests(mention_group, {}, h); })->requests.size() == 1,
                "Disabling approval does not automatically accept pending users");
            auto mode_join = call<chat::group_join_result>([&](auto h) { d.join_group(**approval_link, h); });
            require(mode_join && mode_join->state == chat::group_join_state::joined &&
                call<chat::group_join_requests_result>([&](auto h) { a.get_group_join_requests(mention_group, {}, h); })->requests.empty(),
                "Using the current direct link joins normally and removes the pending application atomically");
            auto const request_sql = "WITH applicants AS (INSERT INTO users(username,password_hash) SELECT 'chat_request_page_" +
                std::to_string(getpid()) + "_'||n,repeat('x',60) FROM generate_series(1,58) n RETURNING id), "
                "requests AS (INSERT INTO group_join_requests(conversation_id,user_id) SELECT " + std::to_string(mention_group) +
                ",id FROM applicants RETURNING user_id) SELECT user_id FROM requests ORDER BY user_id DESC";
            std::unique_ptr<PGresult, decltype(&PQclear)> request_fixture(PQexec(data.database.get(), request_sql.c_str()), &PQclear);
            require(request_fixture && PQresultStatus(request_fixture.get()) == PGRES_TUPLES_OK && PQntuples(request_fixture.get()) == 58,
                "Create real multi-page pending application fixtures");
            for (int row = 0; row < 58; ++row) { data.users.push_back(std::stoll(PQgetvalue(request_fixture.get(), row, 0))); }
            auto first_requests = call<chat::group_join_requests_result>([&](auto h) { a.get_group_join_requests(mention_group, {}, h); });
            require(first_requests && first_requests->requests.size() == 50 && first_requests->next == first_requests->requests.back().applicant.id,
                "Pending applications use a bounded first page and exact last-user cursor");
            auto second_requests = call<chat::group_join_requests_result>([&](auto h) { b.get_group_join_requests(mention_group, first_requests->next, h); });
            require(second_requests && second_requests->requests.size() == 8 && !second_requests->next,
                "Pending application cursor terminates without truncation");
            for (int row = 0; row < 58; ++row)
            {
                auto const& item = row < 50 ? first_requests->requests[row] : second_requests->requests[row - 50];
                require(item.applicant.id == std::stoll(PQgetvalue(request_fixture.get(), row, 0)), "Application pages contain each user exactly once in cursor order");
            }
            data.execute("DELETE FROM group_join_requests WHERE conversation_id=" + std::to_string(mention_group));
            std::cout << "PASS group join approval permission, private notification, pending isolation, accept/reject, reconnect, mode changes and cursor pagination\n";
            std::cout << "PASS persistent group mentions, literal names, boundaries, edits, leave/rejoin, reconnect and payload limit\n";
        }
        auto const page_owner = std::to_string(data.users[0]);
        auto const page_peer = std::to_string(data.users[1]);
        auto const page_sql =
            "WITH created AS (INSERT INTO conversations(kind,title,owner_id,activity) "
            "SELECT 'group','pagination'," + page_owner + ",1000+n/2 FROM generate_series(1,123) n RETURNING id,activity), "
            "members AS (INSERT INTO conversation_members(conversation_id,user_id,pinned) "
            "SELECT id," + page_owner + ",activity%2=0 FROM created UNION ALL SELECT id," + page_peer +
            ",false FROM created RETURNING conversation_id) SELECT id FROM created";
        std::unique_ptr<PGresult, decltype(&PQclear)> page_fixture(PQexec(data.database.get(), page_sql.c_str()), &PQclear);
        require(page_fixture && PQresultStatus(page_fixture.get()) == PGRES_TUPLES_OK && PQntuples(page_fixture.get()) == 123,
            "Create multi-page pin fixtures with tied activity across both tiers");
        for (int row = 0; row < 123; ++row) { data.groups.push_back(std::stoll(PQgetvalue(page_fixture.get(), row, 0))); }
        auto verify_pages = [&](chat::client& client, std::int64_t user) {
            auto const sql = "SELECT c.id,own.pinned FROM conversations c JOIN conversation_members own ON own.conversation_id=c.id "
                "WHERE own.user_id=" + std::to_string(user) +
                " AND (c.kind='group' OR EXISTS(SELECT 1 FROM messages WHERE conversation_id=c.id)) "
                "ORDER BY own.pinned DESC,c.activity DESC,c.id DESC";
            std::unique_ptr<PGresult, decltype(&PQclear)> expected(PQexec(data.database.get(), sql.c_str()), &PQclear);
            require(expected && PQresultStatus(expected.get()) == PGRES_TUPLES_OK, "Expected conversation order");
            std::optional<chat::conversation_cursor> cursor;
            int count = 0, pages = 0;
            bool pinned_cursor = false, ordinary_cursor = false;
            do
            {
                auto result = call<chat::conversations_result>([&](auto handler) { client.get_conversations(cursor, handler); });
                require(result && result->conversations.size() <= 50, "Conversation cursor page limit");
                for (auto const& item : result->conversations)
                {
                    require(count < PQntuples(expected.get()) && item.id == std::stoll(PQgetvalue(expected.get(), count, 0)) &&
                        item.pinned == (std::string_view(PQgetvalue(expected.get(), count, 1)) == "t"),
                        "All pages preserve pinned/activity/id order without omission or duplication");
                    ++count;
                }
                cursor = result->next;
                if (cursor)
                {
                    require(!result->conversations.empty() && cursor->id == result->conversations.back().id &&
                        cursor->pinned == result->conversations.back().pinned, "Cursor carries the last visible tier");
                    pinned_cursor |= cursor->pinned;
                    ordinary_cursor |= !cursor->pinned;
                }
                require(++pages <= 4, "Cursor traversal terminates");
            } while (cursor);
            require(count == PQntuples(expected.get()) && pages >= 3 && ordinary_cursor &&
                (user != data.users[0] || pinned_cursor), "Full traversal crosses both tiers and multiple pinned pages");
        };
        verify_pages(a, data.users[0]);
        verify_pages(b, data.users[1]);
        auto const changed_id = data.groups.back();
        require(call<bool>([&](auto handler) { a.set_conversation_pinned(changed_id, true, handler); }).value(), "Pin paged conversation");
        verify_pages(a, data.users[0]);
        verify_pages(b, data.users[1]);
        auto unpinned = call<bool>([&](auto handler) { a.set_conversation_pinned(changed_id, false, handler); });
        require(unpinned && !*unpinned, "Unpin paged conversation");
        verify_pages(a, data.users[0]);
        std::cout << "PASS personal pin, tied activity, multi-page cursor and unpin ordering\n";
        a.close();
        b.close();
        c.close();
        d.close();
        a_events.wait([&] { return a_events.disconnected == 1; });
        b_events.wait([&] { return b_events.disconnected == 3; });
        c_events.wait([&] { return c_events.disconnected == 4; });
        d_events.wait([&] { return d_events.disconnected == 3; });
        {
            std::array<events, 5> managed_events;
            std::array<chat::client, 5> managed;
            std::vector<std::int64_t> managed_ids;
            for (int i = 0; i < 5; ++i)
            {
                auto& client = managed[i];
                managed_events[i].attach(client);
                client.connect(server.url);
                managed_events[i].wait([&, i] { return managed_events[i].connected == 1; });
                auto name = "chat_roles_test_" + std::to_string(getpid()) + "_" + std::to_string(i);
                auto id = call<std::int64_t>([&](auto handler) { client.register_user(name, "roles password", handler); });
                require(id.has_value(), "Register roles fixture");
                data.users.push_back(*id);
                managed_ids.push_back(*id);
                auto unauthenticated = call<bool>([&](auto handler) { client.set_group_admin(1, *id, true, handler); });
                require(!unauthenticated && unauthenticated.error().code == -32001, "Admin change requires authentication");
                auto authenticated = call<chat::authentication_result>(
                    [&](auto handler) { client.authenticate(name, "roles password", handler); });
                require(authenticated && authenticated->authenticated, "Authenticate roles fixture");
                if (i > 0)
                {
                    require(call<chat::user>([&](auto handler) { managed[0].add_contact(*id, handler); }).has_value(),
                            "Owner role contacts");
                }
            }
            auto created = call<std::int64_t>([&](auto handler) {
                managed[0].create_group("管理员群", {managed_ids[1], managed_ids[2], managed_ids[3], managed_ids[4]}, handler);
            });
            require(created.has_value(), "Create role group");
            auto const managed_group = *created;
            data.groups.push_back(managed_group);
            for (int i = 1; i <= 2; ++i)
            {
                auto promoted = call<bool>([&](auto handler) {
                    managed[0].set_group_admin(managed_group, managed_ids[i], true, handler);
                });
                require(promoted && *promoted, "Promote administrator");
            }
            auto promote = [&](int i) {
                return call<bool>([&](auto handler) { managed[0].set_group_admin(managed_group, managed_ids[i], true, handler); });
            };
            auto third = std::async(std::launch::async, promote, 3);
            auto fourth = std::async(std::launch::async, promote, 4);
            auto third_result = third.get();
            auto fourth_result = fourth.get();
            require(third_result.has_value() != fourth_result.has_value() &&
                        (third_result ? fourth_result.error().code : third_result.error().code) == -32010,
                    "Concurrent requests cannot create a fourth administrator");
            auto roles = call<std::vector<chat::conversation_member>>(
                [&](auto handler) { managed[1].get_members(managed_group, handler); });
            require(roles && roles->size() == 5 && roles->front().role == chat::member_role::owner &&
                        std::count_if(roles->begin(), roles->end(), [](auto const& member) {
                            return member.role == chat::member_role::admin;
                        }) == 3, "Three administrators plus one owner");
            auto denied = call<bool>([&](auto handler) {
                managed[1].set_group_admin(managed_group, managed_ids[2], false, handler);
            });
            auto owner_target = call<bool>([&](auto handler) {
                managed[0].set_group_admin(managed_group, managed_ids[0], true, handler);
            });
            auto absent_target = call<bool>([&](auto handler) {
                managed[0].set_group_admin(managed_group, data.users[0], true, handler);
            });
            require(!denied && denied.error().code == -32009 && !owner_target && owner_target.error().code == -32602 &&
                        !absent_target && absent_target.error().code == -32005, "Only owner manages existing ordinary members");
            auto unchanged = call<bool>([&](auto handler) {
                managed[0].set_group_admin(managed_group, managed_ids[1], true, handler);
            });
            auto demoted = call<bool>([&](auto handler) {
                managed[0].set_group_admin(managed_group, managed_ids[1], false, handler);
            });
            auto replacement = call<bool>([&](auto handler) {
                managed[0].set_group_admin(managed_group, managed_ids[third_result ? 4 : 3], true, handler);
            });
            require(unchanged && !*unchanged && demoted && *demoted && replacement && *replacement,
                    "Idempotent assignment and freed administrator slot");
            managed[2].close();
            managed_events[2].wait([&] { return managed_events[2].disconnected == 1; });
            managed[2].connect(server.url);
            managed_events[2].wait([&] { return managed_events[2].connected == 2; });
            auto authenticated = call<chat::authentication_result>([&](auto handler) {
                managed[2].authenticate("chat_roles_test_" + std::to_string(getpid()) + "_2", "roles password", handler);
            });
            auto recovered = call<std::vector<chat::conversation_member>>(
                [&](auto handler) { managed[2].get_members(managed_group, handler); });
            require(authenticated && authenticated->authenticated && recovered && (*recovered)[1].role == chat::member_role::member &&
                        (*recovered)[2].role == chat::member_role::admin, "Roles persist across reconnect");
            auto direct = call<std::int64_t>([&](auto handler) { managed[0].open_direct_conversation(managed_ids[1], handler); });
            require(direct.has_value(), "Direct conversation remains available");
            auto direct_admin = call<bool>([&](auto handler) {
                managed[0].set_group_admin(*direct, managed_ids[1], true, handler);
            });
            auto invalid_group = call<std::int64_t>([&](auto handler) {
                managed[0].create_group(std::string("a\0b", 3), {managed_ids[1]}, handler);
            });
            require(!direct_admin && direct_admin.error().code == -32006 && !invalid_group && invalid_group.error().code == -32602,
                    "Group-only management and NUL title validation");
            auto owner_leave = call<bool>([&](auto handler) { managed[0].leave_group(managed_group, handler); });
            auto ordinary_rename = call<bool>([&](auto handler) { managed[1].rename_group(managed_group, "拒绝", handler); });
            auto ordinary_invite = call<bool>([&](auto handler) {
                managed[1].invite_group_members(managed_group, {managed_ids[4]}, handler);
            });
            require(!owner_leave && owner_leave.error().code == -32009 && !ordinary_rename &&
                        ordinary_rename.error().code == -32009 && !ordinary_invite && ordinary_invite.error().code == -32009,
                    "Owner stays and ordinary members cannot manage group");
            auto renamed = call<bool>([&](auto handler) { managed[2].rename_group(managed_group, "管理员改名", handler); });
            auto same_name = call<bool>([&](auto handler) { managed[0].rename_group(managed_group, "管理员改名", handler); });
            auto nul_name = call<bool>([&](auto handler) { managed[0].rename_group(managed_group, std::string("a\0b", 3), handler); });
            auto duplicate_invite = call<bool>([&](auto handler) {
                managed[0].invite_group_members(managed_group, {managed_ids[4], managed_ids[4]}, handler);
            });
            require(renamed && *renamed && same_name && !*same_name && !nul_name && nul_name.error().code == -32602 &&
                        !duplicate_invite && duplicate_invite.error().code == -32602 &&
                        conversation(managed[1], managed_group).username == "管理员改名", "Rename persistence and parameter checks");
            auto own_message = call<chat::send_message_result>([&](auto handler) {
                managed[4].send_message(managed_group, "退出前自己的消息", handler);
            });
            auto old_file = call<chat::message>([&](auto handler) {
                managed[0].send_attachment(managed_group, "history.bin", "old file", handler);
            });
            require(own_message && old_file, "Former member message and attachment fixtures");
            std::size_t changes_before_leave;
            {
                std::lock_guard lock(managed_events[4].mutex);
                changes_before_leave = managed_events[4].conversations.size();
            }
            auto left = call<bool>([&](auto handler) { managed[4].leave_group(managed_group, handler); });
            require(left && *left, "Administrator can leave");
            managed_events[4].wait([&] { return managed_events[4].conversations.size() > changes_before_leave; });
            auto left_conversations = call<chat::conversations_result>([&](auto handler) { managed[4].get_conversations({}, handler); });
            require(left_conversations && std::none_of(left_conversations->conversations.begin(), left_conversations->conversations.end(),
                        [&](auto const& value) { return value.id == managed_group; }), "Left group disappears from conversations");
            auto lost_history = call<chat::messages_result>([&](auto handler) { managed[4].get_messages(managed_group, {}, handler); });
            auto lost_members = call<std::vector<chat::conversation_member>>([&](auto handler) { managed[4].get_members(managed_group, handler); });
            auto lost_send = call<chat::send_message_result>([&](auto handler) { managed[4].send_message(managed_group, "denied", handler); });
            auto lost_search = call<chat::messages_result>([&](auto handler) { managed[4].search_messages(managed_group, "退出", {}, handler); });
            auto lost_read = call<std::int64_t>([&](auto handler) { managed[4].mark_read(managed_group, own_message->message_id, handler); });
            auto lost_typing = call<bool>([&](auto handler) { managed[4].set_typing(managed_group, true, handler); });
            auto lost_edit = call<chat::message>([&](auto handler) { managed[4].edit_message(managed_group, own_message->message_id, "denied", handler); });
            auto lost_delete = call<chat::message>([&](auto handler) { managed[4].delete_message(managed_group, own_message->message_id, handler); });
            auto lost_file = call<std::string>([&](auto handler) { managed[4].get_attachment(managed_group, old_file->id, handler); });
            require(!lost_history && !lost_members && !lost_send && !lost_search && !lost_read && !lost_typing &&
                        !lost_edit && !lost_delete && !lost_file, "Left member loses all group access, including own messages");
            std::size_t old_message_count, old_read_count, old_typing_count;
            {
                std::lock_guard lock(managed_events[4].mutex);
                old_message_count = managed_events[4].messages.size();
                old_read_count = managed_events[4].reads.size();
                old_typing_count = managed_events[4].typing.size();
            }
            auto after_leave = call<chat::send_message_result>([&](auto handler) {
                managed[0].send_message(managed_group, "退出之后的消息", handler);
            });
            require(after_leave.has_value(), "Send after another member leaves");
            require(call<std::int64_t>([&](auto handler) { managed[1].mark_read(managed_group, after_leave->message_id, handler); }).has_value(),
                    "Read after another member leaves");
            require(call<bool>([&](auto handler) { managed[0].set_typing(managed_group, true, handler); }).has_value(),
                    "Typing after another member leaves");
            auto barrier = call<std::vector<chat::conversation_member>>([&](auto handler) { managed[4].get_members(managed_group, handler); });
            require(!barrier && barrier.error().code == -32006, "Left member event barrier");
            {
                std::lock_guard lock(managed_events[4].mutex);
                require(managed_events[4].messages.size() == old_message_count && managed_events[4].reads.size() == old_read_count &&
                            managed_events[4].typing.size() == old_typing_count, "Left member receives no message/read/typing notifications");
            }
            auto missing_contact = call<bool>([&](auto handler) {
                managed[2].invite_group_members(managed_group, {managed_ids[4]}, handler);
            });
            require(!missing_contact && missing_contact.error().code == -32005, "Administrators must invite their own contacts");
            require(call<chat::user>([&](auto handler) { managed[2].add_contact(managed_ids[4], handler); }).has_value(),
                    "Administrator contact");
            data.execute("BEGIN");
            data.execute("UPDATE conversations SET title=title WHERE id=" + std::to_string(managed_group));
            auto racing_send = std::async(std::launch::async, [&] {
                return call<chat::send_message_result>([&](auto handler) {
                    managed[0].send_message(managed_group, "与邀请并发的消息 @chat_roles_test_" + std::to_string(getpid()) + "_4", handler);
                });
            });
            auto invite = std::async(std::launch::async, [&] {
                return call<bool>([&](auto handler) { managed[2].invite_group_members(managed_group, {managed_ids[4]}, handler); });
            });
            bool blocked = false;
            for (int i = 0; i < 100 && !blocked; ++i)
            {
                std::string query = "SELECT count(*)>=2 FROM pg_stat_activity WHERE datname=current_database() "
                                    "AND wait_event_type='Lock' AND cardinality(pg_blocking_pids(pid))>0";
                std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(data.database.get(), query.c_str()), &PQclear);
                blocked = result && PQresultStatus(result.get()) == PGRES_TUPLES_OK && std::string_view(PQgetvalue(result.get(), 0, 0)) == "t";
                if (!blocked)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            data.execute("COMMIT");
            auto invited = invite.get();
            auto raced_message = racing_send.get();
            require(blocked && invited && *invited && raced_message, "Concurrent send and invitation wait for conversation lock");
            std::unique_ptr<PGresult, decltype(&PQclear)> joined(PQexec(data.database.get(),
                ("SELECT joined_message_id::text FROM conversation_members WHERE conversation_id=" + std::to_string(managed_group) +
                 " AND user_id=" + std::to_string(managed_ids[4])).c_str()), &PQclear);
            require(joined && PQresultStatus(joined.get()) == PGRES_TUPLES_OK && PQntuples(joined.get()) == 1,
                    "Joined watermark query");
            auto joined_watermark = std::stoll(PQgetvalue(joined.get(), 0, 0));
            std::uint64_t racing_unread = raced_message->message_id > joined_watermark ? 1 : 0;
            require(raced_message->mentions.size() == racing_unread &&
                (!racing_unread || raced_message->mentions.front().user == managed_ids[4]),
                "Mention resolution shares the send/invite lock boundary with joined watermark");

            require(joined_watermark == after_leave->message_id || joined_watermark == raced_message->message_id,
                    "Joined watermark follows serialized send/invite order");
            auto rejoined = call<chat::messages_result>([&](auto handler) { managed[4].get_messages(managed_group, {}, handler); });
            auto rejoined_roles = call<std::vector<chat::conversation_member>>([&](auto handler) { managed[4].get_members(managed_group, handler); });
            auto rejoined_file = call<std::string>([&](auto handler) { managed[4].get_attachment(managed_group, old_file->id, handler); });
            require(rejoined && rejoined->messages.size() == 4 && position(*rejoined, managed_ids[4]) == 0 &&
                        conversation(managed[4], managed_group).unread == racing_unread && rejoined_roles &&
                        rejoined_roles->back().role == chat::member_role::member && rejoined_file && *rejoined_file == "old file",
                    "Rejoin exposes full history, resets admin role, excludes prejoin unread and does not fake reading");
            auto again = call<bool>([&](auto handler) { managed[0].invite_group_members(managed_group, {managed_ids[4]}, handler); });
            require(again && !*again, "Repeated invitation preserves existing membership");
            auto next_message = call<chat::send_message_result>([&](auto handler) { managed[0].send_message(managed_group, "入群之后", handler); });
            require(next_message && conversation(managed[4], managed_group).unread == racing_unread + 1,
                    "Messages after joining count as unread");
            managed_events[4].wait([&] { return std::any_of(managed_events[4].messages.begin(), managed_events[4].messages.end(),
                [&](auto const& value) { return value.id == next_message->message_id; }); });
            auto read_after_join = call<std::int64_t>([&](auto handler) { managed[4].mark_read(managed_group, next_message->message_id, handler); });
            require(read_after_join && *read_after_join == next_message->message_id && conversation(managed[4], managed_group).unread == 0,
                    "Only mark_read advances actual reading");
            auto slot = call<bool>([&](auto handler) { managed[0].set_group_admin(managed_group, managed_ids[1], true, handler); });
            require(slot && *slot, "Leaving administrator frees a slot");
            managed[4].close();
            managed_events[4].wait([&] { return managed_events[4].disconnected == 1; });
            managed[4].connect(server.url);
            managed_events[4].wait([&] { return managed_events[4].connected == 2; });
            auto reauth = call<chat::authentication_result>([&](auto handler) {
                managed[4].authenticate("chat_roles_test_" + std::to_string(getpid()) + "_4", "roles password", handler);
            });
            auto snapshot = call<chat::messages_result>([&](auto handler) { managed[4].get_messages(managed_group, {}, handler); });
            require(reauth && reauth->authenticated && snapshot && position(*snapshot, managed_ids[4]) == next_message->message_id &&
                        conversation(managed[4], managed_group).username == "管理员改名", "Rejoin state and title survive reconnect");
            auto lifecycle = call<std::int64_t>([&](auto handler) {
                managed[0].create_group("管理生命周期", {managed_ids[1], managed_ids[2], managed_ids[3], managed_ids[4]}, handler);
            });
            require(lifecycle.has_value(), "Lifecycle group");
            auto const lifecycle_group = *lifecycle;
            data.groups.push_back(lifecycle_group);
            for (int i : {1, 2})
            {
                require(call<bool>([&](auto handler) {
                    managed[0].set_group_admin(lifecycle_group, managed_ids[i], true, handler);
                }).has_value(), "Lifecycle administrator");
            }
            auto nonowner_transfer = call<bool>([&](auto handler) {
                managed[2].transfer_group_owner(lifecycle_group, managed_ids[1], handler);
            });
            auto ordinary_target = call<bool>([&](auto handler) {
                managed[0].transfer_group_owner(lifecycle_group, managed_ids[3], handler);
            });
            auto ordinary_remove = call<bool>([&](auto handler) {
                managed[3].remove_group_member(lifecycle_group, managed_ids[4], handler);
            });
            require(!nonowner_transfer && nonowner_transfer.error().code == -32009 && !ordinary_target &&
                ordinary_target.error().code == -32009 && !ordinary_remove && ordinary_remove.error().code == -32009,
                "Only owner transfers to a current administrator; ordinary members cannot remove");
            auto transferred = call<bool>([&](auto handler) {
                managed[0].transfer_group_owner(lifecycle_group, managed_ids[1], handler);
            });
            auto transferred_roles = call<std::vector<chat::conversation_member>>([&](auto handler) {
                managed[0].get_members(lifecycle_group, handler);
            });
            require(transferred && *transferred && transferred_roles && conversation(managed[0], lifecycle_group).member_count == 5 &&
                (*transferred_roles)[0].role == chat::member_role::admin &&
                (*transferred_roles)[1].role == chat::member_role::owner &&
                std::count_if(transferred_roles->begin(), transferred_roles->end(), [](auto const& member) {
                    return member.role == chat::member_role::admin;
                }) == 2, "Transfer swaps roles without consuming an administrator slot");
            require(call<bool>([&](auto handler) { managed[0].leave_group(lifecycle_group, handler); }).has_value(),
                "Former owner can leave after transfer");
            require(call<chat::user>([&](auto handler) { managed[1].add_contact(managed_ids[0], handler); }).has_value(),
                "New owner contact");
            require(call<bool>([&](auto handler) {
                managed[1].invite_group_members(lifecycle_group, {managed_ids[0]}, handler);
            }).has_value(), "Former owner rejoins as member");
            auto admin_remove_admin = call<bool>([&](auto handler) {
                managed[2].remove_group_member(lifecycle_group, managed_ids[1], handler);
            });
            require(!admin_remove_admin && admin_remove_admin.error().code == -32009, "Administrator cannot remove owner");
            require(call<bool>([&](auto handler) {
                managed[1].set_group_admin(lifecycle_group, managed_ids[0], true, handler);
            }).has_value(), "Promote former owner again");
            admin_remove_admin = call<bool>([&](auto handler) {
                managed[2].remove_group_member(lifecycle_group, managed_ids[0], handler);
            });
            auto self_remove = call<bool>([&](auto handler) {
                managed[1].remove_group_member(lifecycle_group, managed_ids[1], handler);
            });
            require(!admin_remove_admin && admin_remove_admin.error().code == -32009 && !self_remove &&
                self_remove.error().code == -32602, "Administrator cannot remove administrator; owner cannot remove self");
            require(call<bool>([&](auto handler) {
                managed[1].remove_group_member(lifecycle_group, managed_ids[0], handler);
            }).has_value(), "Owner removes administrator");
            require(call<bool>([&](auto handler) {
                managed[1].invite_group_members(lifecycle_group, {managed_ids[0]}, handler);
            }).has_value(), "Owner reinvites removed administrator");
            auto removed_admin_roles = call<std::vector<chat::conversation_member>>([&](auto handler) {
                managed[0].get_members(lifecycle_group, handler);
            });
            require(removed_admin_roles && removed_admin_roles->front().role == chat::member_role::member,
                "Removed administrator returns as ordinary member");
            auto removed_own = call<chat::send_message_result>([&](auto handler) {
                managed[4].send_message(lifecycle_group, "将被移除者自己的消息", handler);
            });
            auto removed_file = call<chat::message>([&](auto handler) {
                managed[1].send_attachment(lifecycle_group, "removed.bin", "file", handler);
            });
            require(removed_own && removed_file, "Removed member fixtures");
            auto before_removal_read = call<std::int64_t>([&](auto handler) {
                managed[4].mark_read(lifecycle_group, removed_file->id, handler);
            });
            require(before_removal_read && *before_removal_read == removed_file->id,
                "Removed member has a real reading watermark to discard");
            require(call<bool>([&](auto handler) { managed[4].set_conversation_muted(lifecycle_group, true, handler); }).value() &&
                call<bool>([&](auto handler) { managed[4].set_conversation_pinned(lifecycle_group, true, handler); }).value(),
                "Member preferences before removal");
            require(call<bool>([&](auto handler) {
                managed[2].remove_group_member(lifecycle_group, managed_ids[4], handler);
            }).has_value(), "Administrator removes ordinary member");
            managed_events[4].wait([&] {
                return std::count(managed_events[4].removals.begin(), managed_events[4].removals.end(), lifecycle_group) == 1;
            });
            auto removed_history = call<chat::messages_result>([&](auto handler) { managed[4].get_messages(lifecycle_group, {}, handler); });
            auto removed_members = call<std::vector<chat::conversation_member>>([&](auto handler) { managed[4].get_members(lifecycle_group, handler); });
            auto removed_send = call<chat::send_message_result>([&](auto handler) { managed[4].send_message(lifecycle_group, "denied", handler); });
            auto removed_search = call<chat::messages_result>([&](auto handler) { managed[4].search_messages(lifecycle_group, "消息", {}, handler); });
            auto removed_read = call<std::int64_t>([&](auto handler) { managed[4].mark_read(lifecycle_group, removed_own->message_id, handler); });
            auto removed_typing = call<bool>([&](auto handler) { managed[4].set_typing(lifecycle_group, true, handler); });
            auto removed_edit = call<chat::message>([&](auto handler) { managed[4].edit_message(lifecycle_group, removed_own->message_id, "denied", handler); });
            auto removed_delete = call<chat::message>([&](auto handler) { managed[4].delete_message(lifecycle_group, removed_own->message_id, handler); });
            auto removed_download = call<std::string>([&](auto handler) { managed[4].get_attachment(lifecycle_group, removed_file->id, handler); });
            auto removed_upload = call<chat::message>([&](auto handler) { managed[4].send_attachment(lifecycle_group, "denied.bin", "file", handler); });
            auto removed_conversations = call<chat::conversations_result>([&](auto handler) {
                managed[4].get_conversations({}, handler);
            });
            auto removed_mute = call<bool>([&](auto handler) { managed[4].set_conversation_muted(lifecycle_group, true, handler); });
            auto removed_pin = call<bool>([&](auto handler) { managed[4].set_conversation_pinned(lifecycle_group, true, handler); });
            auto removed_announcement = call<bool>([&](auto h) { managed[4].set_group_announcement(lifecycle_group, "denied", h); });
            require(!removed_history && !removed_members && !removed_send && !removed_search && !removed_read &&
                !removed_typing && !removed_edit && !removed_delete && !removed_download && !removed_upload &&
                !removed_mute && removed_mute.error().code == -32006 && !removed_pin && removed_pin.error().code == -32006 &&
                !removed_announcement && removed_announcement.error().code == -32006,
                "Removed member loses every group access path");
            require(removed_conversations && std::none_of(removed_conversations->conversations.begin(),
                removed_conversations->conversations.end(), [&](auto const& value) { return value.id == lifecycle_group; }) &&
                conversation(managed[1], lifecycle_group).member_count == 4,
                "Removed conversation disappears and current member count decreases");
            std::array<std::size_t, 5> before_removed_publish;
            {
                std::lock_guard lock(managed_events[4].mutex);
                before_removed_publish = {managed_events[4].messages.size(), managed_events[4].reads.size(),
                    managed_events[4].typing.size(), managed_events[4].updates.size(), managed_events[4].conversations.size()};
            }
            auto while_removed = call<chat::send_message_result>([&](auto handler) {
                managed[1].send_message(lifecycle_group, "移除期间的消息", handler);
            });
            require(while_removed && call<std::int64_t>([&](auto handler) {
                managed[2].mark_read(lifecycle_group, while_removed->message_id, handler);
            }).has_value() && call<bool>([&](auto handler) {
                managed[1].set_typing(lifecycle_group, true, handler);
            }).has_value() && call<chat::message>([&](auto handler) {
                managed[1].edit_message(lifecycle_group, while_removed->message_id, "移除期间的编辑", handler);
            }).has_value(), "Publish after removal");
            require(call<bool>([&](auto h) { managed[1].set_group_announcement(lifecycle_group, "移除后的公告", h); }).value(),
                "Current owner changes announcement after removal");
            require(!call<std::vector<chat::conversation_member>>([&](auto handler) {
                managed[4].get_members(lifecycle_group, handler);
            }), "Removed notification barrier");
            {
                std::lock_guard lock(managed_events[4].mutex);
                require(before_removed_publish == std::array<std::size_t, 5>{managed_events[4].messages.size(),
                    managed_events[4].reads.size(), managed_events[4].typing.size(), managed_events[4].updates.size(),
                    managed_events[4].conversations.size()} &&
                    std::count(managed_events[4].removals.begin(), managed_events[4].removals.end(), lifecycle_group) == 1,
                    "Removed member receives exactly one removal and no later realtime events");
            }
            require(call<chat::user>([&](auto handler) { managed[1].add_contact(managed_ids[4], handler); }).has_value() &&
                call<bool>([&](auto handler) {
                    managed[1].invite_group_members(lifecycle_group, {managed_ids[4]}, handler);
                }).has_value(), "Reinvite removed member");
            auto restored = call<chat::messages_result>([&](auto handler) { managed[4].get_messages(lifecycle_group, {}, handler); });
            auto restored_members = call<std::vector<chat::conversation_member>>([&](auto handler) { managed[4].get_members(lifecycle_group, handler); });
            require(restored && restored->messages.size() == 3 && position(*restored, managed_ids[4]) == 0 &&
                conversation(managed[4], lifecycle_group).unread == 0 &&
                !conversation(managed[4], lifecycle_group).muted && !conversation(managed[4], lifecycle_group).pinned &&
                conversation(managed[1], lifecycle_group).member_count == 5 && restored_members &&
                restored_members->back().role == chat::member_role::member,
                "Reinvite resets role and actual read position while exposing history without old unread");
            auto after_reinvite = call<chat::send_message_result>([&](auto handler) {
                managed[1].send_message(lifecycle_group, "重新邀请后的消息", handler);
            });
            require(after_reinvite && conversation(managed[4], lifecycle_group).unread == 1,
                "New messages after reinvite count unread");
            auto locked_race = [&](auto first_operation, auto second_operation) {
                data.execute("BEGIN");
                data.execute("UPDATE conversations SET title=title WHERE id=" + std::to_string(lifecycle_group));
                auto first = std::async(std::launch::async, first_operation);
                auto second = std::async(std::launch::async, second_operation);
                bool waiting = false;
                for (int i = 0; i < 100 && !waiting; ++i)
                {
                    std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(data.database.get(),
                        "SELECT pg_stat_clear_snapshot(); "
                        "SELECT count(*)>=2 FROM pg_stat_activity WHERE datname=current_database() "
                        "AND wait_event_type='Lock' AND cardinality(pg_blocking_pids(pid))>0"), &PQclear);
                    waiting = result && PQresultStatus(result.get()) == PGRES_TUPLES_OK &&
                        std::string_view(PQgetvalue(result.get(), 0, 0)) == "t";
                    if (!waiting) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
                }
                data.execute("COMMIT");
                auto results = std::pair{first.get(), second.get()};
                require(waiting, "Both management race requests wait for conversation lock");
                return results;
            };
            auto [mute_remove, racing_mute] = locked_race([&] {
                return call<bool>([&](auto handler) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], handler); });
            }, [&] {
                return call<bool>([&](auto handler) { managed[4].set_conversation_muted(lifecycle_group, true, handler); });
            });
            require(mute_remove && *mute_remove && (racing_mute || racing_mute.error().code == -32006),
                "Mute/remove race rechecks membership after conversation lock");
            require(call<bool>([&](auto handler) {
                managed[1].invite_group_members(lifecycle_group, {managed_ids[4]}, handler);
            }).has_value() && !conversation(managed[4], lifecycle_group).muted,
                "Reinvite after mute/remove race resets the personal preference");
            auto [pin_remove, racing_pin] = locked_race([&] {
                return call<bool>([&](auto handler) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], handler); });
            }, [&] {
                return call<bool>([&](auto handler) { managed[4].set_conversation_pinned(lifecycle_group, true, handler); });
            });
            require(pin_remove && *pin_remove && (racing_pin || racing_pin.error().code == -32006),
                "Pin/remove race rechecks membership after conversation lock");
            require(call<bool>([&](auto handler) {
                managed[1].invite_group_members(lifecycle_group, {managed_ids[4]}, handler);
            }).has_value() && !conversation(managed[4], lifecycle_group).pinned,
                "Reinvite after pin/remove race resets personal pin");
            auto [racing_remove, member_send] = locked_race([&] {
                return call<bool>([&](auto handler) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], handler); });
            }, [&] {
                return call<chat::send_message_result>([&](auto handler) { managed[4].send_message(lifecycle_group, "与移除竞争", handler); });
            });
            require(racing_remove && *racing_remove && (member_send || member_send.error().code == -32006),
                "Remove/send race follows lock order without a post-removal write");
            auto current_history = call<chat::messages_result>([&](auto handler) { managed[1].get_messages(lifecycle_group, {}, handler); });
            require(current_history && current_history->messages.size() == static_cast<std::size_t>(member_send ? 5 : 4),
                "Send before removal is persisted; send after removal is denied");
            require(call<bool>([&](auto handler) {
                managed[1].invite_group_members(lifecycle_group, {managed_ids[4]}, handler);
            }).has_value(), "Restore member for reaction/remove race");
            auto [reaction_remove, racing_reaction] = locked_race([&] {
                return call<bool>([&](auto handler) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], handler); });
            }, [&] {
                return call<chat::reaction_update>([&](auto handler) {
                    managed[4].set_message_reaction(lifecycle_group, after_reinvite->message_id, "👍", handler);
                });
            });
            require(reaction_remove && *reaction_remove && (racing_reaction || racing_reaction.error().code == -32007),
                    "Reaction/remove race checks membership after conversation lock");
            auto denied_after_remove = call<chat::reaction_update>([&](auto handler) {
                managed[4].set_message_reaction(lifecycle_group, after_reinvite->message_id, "❤️", handler);
            });
            require(!denied_after_remove && denied_after_remove.error().code == -32007,
                    "Removed member cannot replace a reaction");
            auto [racing_delete, reaction_during_delete] = locked_race([&] {
                return call<chat::message>([&](auto handler) {
                    managed[1].delete_message(lifecycle_group, after_reinvite->message_id, handler);
                });
            }, [&] {
                return call<chat::reaction_update>([&](auto handler) {
                    managed[2].set_message_reaction(lifecycle_group, after_reinvite->message_id, "🎉", handler);
                });
            });
            auto after_delete_race = call<chat::messages_result>([&](auto handler) {
                managed[1].get_messages(lifecycle_group, {}, handler);
            });
            require(racing_delete && racing_delete->deleted && racing_delete->reactions.empty() &&
                (reaction_during_delete || reaction_during_delete.error().code == -32007) && after_delete_race &&
                std::ranges::any_of(after_delete_race->messages, [&](auto const& message) {
                    return message.id == after_reinvite->message_id && message.deleted && message.reactions.empty();
                }), "Reaction/delete race always leaves an empty deleted snapshot");
            auto [pin_delete, pin_during_delete] = locked_race([&] {
                return call<chat::message>([&](auto h) { managed[1].delete_message(lifecycle_group, while_removed->message_id, h); });
            }, [&] {
                return call<bool>([&](auto h) { managed[2].pin_group_message(lifecycle_group, while_removed->message_id, h); });
            });
            require(pin_delete && pin_delete->deleted && (pin_during_delete || pin_during_delete.error().code == -32007) &&
                !conversation(managed[2], lifecycle_group).pinned_message,
                "Pin/delete race shares conversation lock and cannot retain a deleted pin");
            auto [demoted_announcer, racing_announcement] = locked_race([&] {
                return call<bool>([&](auto h) { managed[1].set_group_admin(lifecycle_group, managed_ids[2], false, h); });
            }, [&] {
                return call<bool>([&](auto h) { managed[2].set_group_announcement(lifecycle_group, "权限变化竞争的公告", h); });
            });
            require(demoted_announcer && *demoted_announcer &&
                (racing_announcement || racing_announcement.error().code == -32009) &&
                conversation(managed[1], lifecycle_group).announcement == (racing_announcement ? "权限变化竞争的公告" : "移除后的公告"),
                "Announcement/demotion race rechecks current role after acquiring conversation lock");
            auto denied_announcement = call<bool>([&](auto h) { managed[2].set_group_announcement(lifecycle_group, "denied", h); });
            require(!denied_announcement && denied_announcement.error().code == -32009 &&
                call<bool>([&](auto h) { managed[1].set_group_admin(lifecycle_group, managed_ids[2], true, h); }).value(),
                "Demoted member cannot write; restore administrator for existing lifecycle tests");
            auto lifecycle_link = call<std::optional<std::string>>([&](auto h) { managed[1].create_group_invite(lifecycle_group, h); });
            require(lifecycle_link && *lifecycle_link, "Create link for lock races");
            auto [demoted_inviter, racing_link] = locked_race([&] {
                return call<bool>([&](auto h) { managed[1].set_group_admin(lifecycle_group, managed_ids[2], false, h); });
            }, [&] {
                return call<std::optional<std::string>>([&](auto h) { managed[2].create_group_invite(lifecycle_group, h); });
            });
            auto denied_link = call<std::optional<std::string>>([&](auto h) { managed[2].get_group_invite(lifecycle_group, h); });
            require(demoted_inviter && *demoted_inviter && (racing_link || racing_link.error().code == -32009) &&
                !denied_link && denied_link.error().code == -32009 &&
                call<bool>([&](auto h) { managed[1].set_group_admin(lifecycle_group, managed_ids[2], true, h); }).value(),
                "Link/demotion race checks current role under conversation lock");
            auto [revoke_race, join_race] = locked_race([&] {
                return call<std::optional<std::string>>([&](auto h) { managed[2].revoke_group_invite(lifecycle_group, h); });
            }, [&] {
                return call<chat::group_join_result>([&](auto h) { managed[4].join_group(**lifecycle_link, h); });
            });
            auto post_revoke_join = call<chat::group_join_result>([&](auto h) { managed[4].join_group(**lifecycle_link, h); });
            require(revoke_race && !*revoke_race && (join_race || join_race.error().code == -32014) &&
                !post_revoke_join && post_revoke_join.error().code == -32014,
                "Join/revoke race rechecks token after acquiring lock; stale secret always fails afterwards");
            if (join_race) { require(call<bool>([&](auto h) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], h); }).value(), "Restore removed race fixture"); }
            auto send_link = call<std::optional<std::string>>([&](auto h) { managed[1].create_group_invite(lifecycle_group, h); });
            require(send_link && *send_link, "Fresh link for concurrent join/send");
            auto const pre_join_latest = conversation(managed[1], lifecycle_group).last.id;
            auto [join_send, send_join] = locked_race([&] {
                return call<chat::group_join_result>([&](auto h) { managed[4].join_group(**send_link, h); });
            }, [&] {
                return call<chat::send_message_result>([&](auto h) { managed[1].send_message(lifecycle_group, "concurrent link join", h); });
            });
            require(join_send && join_send->state == chat::group_join_state::joined && send_join, "Concurrent join and send both complete");
            auto watermark_sql = "SELECT joined_message_id::text,last_read_message_id::text,is_admin::text FROM conversation_members WHERE conversation_id=" +
                std::to_string(lifecycle_group) + " AND user_id=" + std::to_string(managed_ids[4]);
            std::unique_ptr<PGresult, decltype(&PQclear)> join_watermark(PQexec(data.database.get(), watermark_sql.c_str()), &PQclear);
            require(join_watermark && PQresultStatus(join_watermark.get()) == PGRES_TUPLES_OK && PQntuples(join_watermark.get()) == 1,
                "Read serialized link join watermark");
            auto const actual_joined = std::stoll(PQgetvalue(join_watermark.get(), 0, 0));
            require((actual_joined == pre_join_latest || actual_joined == send_join->message_id) &&
                std::string_view(PQgetvalue(join_watermark.get(), 0, 1)) == "0" && std::string_view(PQgetvalue(join_watermark.get(), 0, 2)) == "false" &&
                conversation(managed[4], lifecycle_group).unread == (actual_joined < send_join->message_id ? 1 : 0),
                "Join/send race separates current membership watermark from real read and counts only later messages");
            require(call<bool>([&](auto h) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], h); }).value(), "Remove link race member before existing recovery tests");
            std::cout << "PASS invite role/revocation/send races under conversation lock\n";
            require(call<bool>([&](auto h) { managed[1].set_group_join_approval(lifecycle_group, true, h); }).value() &&
                call<chat::group_join_result>([&](auto h) { managed[4].join_group(**send_link, h); })->state == chat::group_join_state::pending,
                "Create pending applicant for serialized approval races");
            auto [approval_demote, demoted_accept] = locked_race([&] {
                return call<bool>([&](auto h) { managed[1].set_group_admin(lifecycle_group, managed_ids[2], false, h); });
            }, [&] {
                return call<bool>([&](auto h) { managed[2].respond_group_join_request(lifecycle_group, managed_ids[4], true, h); });
            });
            require(approval_demote && *approval_demote && (demoted_accept ? *demoted_accept : demoted_accept.error().code == -32009),
                "Approval/demotion race validates manager role after conversation lock");
            auto demoted_response = call<bool>([&](auto h) { managed[2].respond_group_join_request(lifecycle_group, managed_ids[4], false, h); });
            require(!demoted_response && demoted_response.error().code == -32009 &&
                call<bool>([&](auto h) { managed[1].set_group_admin(lifecycle_group, managed_ids[2], true, h); }).value(),
                "Demoted administrator cannot respond to pending applications");
            if (demoted_accept)
            {
                require(call<bool>([&](auto h) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], h); }).value() &&
                    call<chat::group_join_result>([&](auto h) { managed[4].join_group(**send_link, h); })->state == chat::group_join_state::pending,
                    "Restore pending applicant after acceptance won demotion race");
            }
            auto const before_accept_race = conversation(managed[1], lifecycle_group).last.id;
            auto [accept_send, send_accept] = locked_race([&] {
                return call<bool>([&](auto h) { managed[1].respond_group_join_request(lifecycle_group, managed_ids[4], true, h); });
            }, [&] {
                return call<chat::send_message_result>([&](auto h) { managed[2].send_message(lifecycle_group, "concurrent approval acceptance", h); });
            });
            require(accept_send && *accept_send && send_accept, "Concurrent acceptance and send both complete");
            std::unique_ptr<PGresult, decltype(&PQclear)> accepted_watermark(PQexec(data.database.get(), watermark_sql.c_str()), &PQclear);
            require(accepted_watermark && PQresultStatus(accepted_watermark.get()) == PGRES_TUPLES_OK && PQntuples(accepted_watermark.get()) == 1,
                "Read serialized approval membership watermark");
            auto const accepted_joined = std::stoll(PQgetvalue(accepted_watermark.get(), 0, 0));
            require((accepted_joined == before_accept_race || accepted_joined == send_accept->message_id) &&
                std::string_view(PQgetvalue(accepted_watermark.get(), 0, 1)) == "0" &&
                std::string_view(PQgetvalue(accepted_watermark.get(), 0, 2)) == "false" &&
                conversation(managed[4], lifecycle_group).unread == (accepted_joined < send_accept->message_id ? 1 : 0),
                "Acceptance/send lock order sets current join watermark without treating older history as read");
            require(call<bool>([&](auto h) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], h); }).value() &&
                call<chat::group_join_result>([&](auto h) { managed[4].join_group(**send_link, h); })->state == chat::group_join_state::pending,
                "Create pending request for duplicate acceptance race");
            auto [owner_accept, admin_accept] = locked_race([&] {
                return call<bool>([&](auto h) { managed[1].respond_group_join_request(lifecycle_group, managed_ids[4], true, h); });
            }, [&] {
                return call<bool>([&](auto h) { managed[2].respond_group_join_request(lifecycle_group, managed_ids[4], true, h); });
            });
            require(owner_accept && admin_accept && *owner_accept != *admin_accept &&
                call<chat::group_join_requests_result>([&](auto h) { managed[1].get_group_join_requests(lifecycle_group, {}, h); })->requests.empty(),
                "Duplicate accept race consumes one request and creates one membership");
            require(call<bool>([&](auto h) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], h); }).value() &&
                call<chat::group_join_result>([&](auto h) { managed[4].join_group(**send_link, h); })->state == chat::group_join_state::pending,
                "Create pending request for accept/reject race");
            auto [racing_accept, racing_reject] = locked_race([&] {
                return call<bool>([&](auto h) { managed[1].respond_group_join_request(lifecycle_group, managed_ids[4], true, h); });
            }, [&] {
                return call<bool>([&](auto h) { managed[2].respond_group_join_request(lifecycle_group, managed_ids[4], false, h); });
            });
            auto decided_members = call<std::vector<chat::conversation_member>>([&](auto h) { managed[1].get_members(lifecycle_group, h); });
            require(racing_accept && racing_reject && *racing_accept != *racing_reject && decided_members &&
                std::ranges::any_of(*decided_members, [&](auto const& value) { return value.id == managed_ids[4]; }) == *racing_accept &&
                call<chat::group_join_requests_result>([&](auto h) { managed[1].get_group_join_requests(lifecycle_group, {}, h); })->requests.empty(),
                "Accept/reject race produces exactly one decision with consistent membership");
            if (*racing_accept) { require(call<bool>([&](auto h) { managed[1].remove_group_member(lifecycle_group, managed_ids[4], h); }).value(), "Remove accepted race fixture"); }
            require(call<bool>([&](auto h) { managed[1].set_group_join_approval(lifecycle_group, false, h); }).value(), "Restore direct-join mode for remaining lifecycle tests");
            std::cout << "PASS approval/demotion, approval/send, duplicate acceptance and accept/reject races under conversation lock\n";
            require(call<chat::user>([&](auto handler) { managed[2].add_contact(managed_ids[3], handler); }).has_value(),
                "Racing invitation contact");
            std::size_t typing_before;
            {
                std::lock_guard lock(managed_events[1].mutex);
                typing_before = managed_events[1].typing.size();
            }
            auto [typing_remove, racing_typing] = locked_race([&] {
                return call<bool>([&](auto handler) { managed[1].remove_group_member(lifecycle_group, managed_ids[3], handler); });
            }, [&] {
                return call<bool>([&](auto handler) { managed[3].set_typing(lifecycle_group, true, handler); });
            });
            require(typing_remove && racing_typing && call<std::vector<chat::conversation_member>>([&](auto handler) {
                managed[1].get_members(lifecycle_group, handler);
            }).has_value(), "Remove/typing race completes");
            {
                std::lock_guard lock(managed_events[1].mutex);
                require(managed_events[1].typing.size() == typing_before + (*racing_typing ? 1 : 0),
                    "Typing before removal is published; typing after removal is suppressed");
            }
            require(call<bool>([&](auto handler) {
                managed[2].invite_group_members(lifecycle_group, {managed_ids[3]}, handler);
            }).has_value(), "Restore member for remove/invite race");
            auto [owner_remove, admin_invite] = locked_race([&] {
                return call<bool>([&](auto handler) { managed[1].remove_group_member(lifecycle_group, managed_ids[3], handler); });
            }, [&] {
                return call<bool>([&](auto handler) { managed[2].invite_group_members(lifecycle_group, {managed_ids[3]}, handler); });
            });
            auto after_membership_race = call<std::vector<chat::conversation_member>>([&](auto handler) {
                managed[1].get_members(lifecycle_group, handler);
            });
            require(owner_remove && *owner_remove && admin_invite && after_membership_race &&
                std::any_of(after_membership_race->begin(), after_membership_race->end(), [&](auto const& member) {
                    return member.id == managed_ids[3];
                }) == *admin_invite, "Owner removes ordinary member; remove/invite race preserves serialized membership");
            auto [transfer_race, leave_race] = locked_race([&] {
                return call<bool>([&](auto handler) { managed[1].transfer_group_owner(lifecycle_group, managed_ids[2], handler); });
            }, [&] {
                return call<bool>([&](auto handler) { managed[2].leave_group(lifecycle_group, handler); });
            });
            require(transfer_race.has_value() != leave_race.has_value() &&
                (transfer_race ? leave_race.error().code == -32009 : transfer_race.error().code == -32005),
                "Transfer/leave race cannot leave an ownerless group");
            auto const recovered_owner = transfer_race ? 2 : 1;
            int old_connects, old_disconnects;
            {
                std::lock_guard lock(managed_events[1].mutex);
                old_connects = managed_events[1].connected;
                old_disconnects = managed_events[1].disconnected;
            }
            managed[1].close();
            managed_events[1].wait([&] { return managed_events[1].disconnected == old_disconnects + 1; });
            managed[1].connect(server.url);
            managed_events[1].wait([&] { return managed_events[1].connected == old_connects + 1; });
            auto lifecycle_auth = call<chat::authentication_result>([&](auto handler) {
                managed[1].authenticate("chat_roles_test_" + std::to_string(getpid()) + "_1", "roles password", handler);
            });
            auto lifecycle_snapshot = call<std::vector<chat::conversation_member>>([&](auto handler) {
                managed[1].get_members(lifecycle_group, handler);
            });
            require(lifecycle_auth && lifecycle_auth->authenticated && lifecycle_snapshot &&
                std::count_if(lifecycle_snapshot->begin(), lifecycle_snapshot->end(), [](auto const& member) {
                    return member.role == chat::member_role::owner;
                }) == 1 && std::any_of(lifecycle_snapshot->begin(), lifecycle_snapshot->end(), [&](auto const& member) {
                    return member.id == managed_ids[recovered_owner] && member.role == chat::member_role::owner;
                }) && std::none_of(lifecycle_snapshot->begin(), lifecycle_snapshot->end(), [&](auto const& member) {
                    return member.id == managed_ids[4];
                }), "Reconnect restores transferred owner and removed membership");
            auto scale_query = "INSERT INTO users(username,password_hash) SELECT 'chat_scale_test_" + std::to_string(getpid()) +
                "_'||n,repeat('x',60) FROM generate_series(1,199) n RETURNING id";
            std::unique_ptr<PGresult, decltype(&PQclear)> scale_users(PQexec(data.database.get(), scale_query.c_str()), &PQclear);
            require(scale_users && PQresultStatus(scale_users.get()) == PGRES_TUPLES_OK && PQntuples(scale_users.get()) == 199,
                    "Small group measurement users");
            std::vector<std::int64_t> scale_ids;
            for (int i = 0; i < 199; ++i)
            {
                auto id = std::stoll(PQgetvalue(scale_users.get(), i, 0));
                scale_ids.push_back(id);
                data.users.push_back(id);
            }
            data.execute("INSERT INTO contacts(owner_id,contact_id) SELECT " + std::to_string(managed_ids[0]) +
                ",id FROM users WHERE username LIKE 'chat_scale_test_" + std::to_string(getpid()) + "_%'");
            for (int size : {3, 10, 50, 200})
            {
                std::vector<std::int64_t> members(scale_ids.begin(), scale_ids.begin() + size - 1);
                auto scale_group = call<std::int64_t>([&](auto handler) {
                    managed[0].create_group("规模测量", members, handler);
                });
                require(scale_group.has_value(), "Create measured small group");
                data.groups.push_back(*scale_group);
                std::array<double, 3> milliseconds{};
                for (int sample = 0; sample < 3; ++sample)
                {
                    auto start = std::chrono::steady_clock::now();
                    auto listed = call<std::vector<chat::conversation_member>>([&](auto handler) {
                        managed[0].get_members(*scale_group, handler);
                    });
                    auto members_done = std::chrono::steady_clock::now();
                    auto history = call<chat::messages_result>([&](auto handler) {
                        managed[0].get_messages(*scale_group, {}, handler);
                    });
                    auto history_done = std::chrono::steady_clock::now();
                    auto sent = call<chat::send_message_result>([&](auto handler) {
                        managed[0].send_message(*scale_group, "规模样本", handler);
                    });
                    auto send_done = std::chrono::steady_clock::now();
                    require(listed && listed->size() == static_cast<std::size_t>(size) && history &&
                        history->read_positions.size() == static_cast<std::size_t>(size) && sent,
                        "Member/read/publish paths remain complete at measured size");
                    milliseconds[0] += std::chrono::duration<double, std::milli>(members_done - start).count() / 3;
                    milliseconds[1] += std::chrono::duration<double, std::milli>(history_done - members_done).count() / 3;
                    milliseconds[2] += std::chrono::duration<double, std::milli>(send_done - history_done).count() / 3;
                }
                std::cout << "GROUP_SCALE members=" << size << " samples=3 get_members_ms=" << milliseconds[0]
                    << " history_ms=" << milliseconds[1] << " send_ms=" << milliseconds[2] << '\n';
            }
            for (int i = 0; i < 5; ++i)
            {
                managed[i].close();
                managed_events[i].wait([&, i] { return managed_events[i].disconnected == (i == 1 || i == 2 || i == 4 ? 2 : 1); });
            }
        }
        data.cleanup();
        std::cout
            << "PASS real four-client direct/group, membership, read, pagination, reconnect and concurrent sends\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL groups: " << error.what() << '\n';
        return 1;
    }
}
