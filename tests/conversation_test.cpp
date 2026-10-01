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

template <class T, class F> std::expected<T, chat::error> call(F operation)
{
    auto promise = std::make_shared<std::promise<std::expected<T, chat::error>>>();
    auto future = promise->get_future();
    operation([promise](auto result) { promise->set_value(std::move(result)); });
    require(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "RPC timeout");
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
    std::vector<std::int64_t> conversations;
    std::vector<std::int64_t> removals;
    std::vector<chat::read_position> reads;
    std::vector<chat::typing_event> typing;
    std::vector<chat::avatar> avatars;

    template <class F> void wait(F predicate)
    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(5), predicate), "Notification timeout");
    }

    void attach(chat::client& client)
    {
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
                    empty.unread == 0,
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
        auto first_page = call<chat::messages_result>([&](auto handler) { c.get_messages(group, {}, handler); });
        require(first_page && first_page->messages.size() == 50 && first_page->has_more &&
                    first_page->messages.back().id == latest,
                "Latest history page");
        auto before = first_page->messages.front().id;
        auto older = call<chat::messages_result>([&](auto handler) { c.get_messages(group, before, handler); });
        require(older && older->messages.size() == 8 && !older->has_more &&
                    older->messages.front().id == sent->message_id,
                "Older cursor page");
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
        b_events.wait(
            [&]
            {
                return std::ranges::any_of(b_events.updates, [&](auto const& value)
                                           { return value.id == sent->message_id && value.deleted; });
            });
        auto repeated = call<chat::message>([&](auto handler) { a.delete_message(group, sent->message_id, handler); });
        require(repeated && repeated->deleted, "Repeated deletion remains deleted");
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
        a.close();
        b.close();
        c.close();
        d.close();
        a_events.wait([&] { return a_events.disconnected == 1; });
        b_events.wait([&] { return b_events.disconnected == 1; });
        c_events.wait([&] { return c_events.disconnected == 4; });
        d_events.wait([&] { return d_events.disconnected == 1; });
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
                    managed[0].send_message(managed_group, "与邀请并发的消息", handler);
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
            require(!removed_history && !removed_members && !removed_send && !removed_search && !removed_read &&
                !removed_typing && !removed_edit && !removed_delete && !removed_download && !removed_upload,
                "Removed member loses every group access path");
            require(removed_conversations && std::none_of(removed_conversations->conversations.begin(),
                removed_conversations->conversations.end(), [&](auto const& value) { return value.id == lifecycle_group; }) &&
                conversation(managed[1], lifecycle_group).member_count == 4,
                "Removed conversation disappears and current member count decreases");
            std::array<std::size_t, 4> before_removed_publish;
            {
                std::lock_guard lock(managed_events[4].mutex);
                before_removed_publish = {managed_events[4].messages.size(), managed_events[4].reads.size(),
                    managed_events[4].typing.size(), managed_events[4].updates.size()};
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
            require(!call<std::vector<chat::conversation_member>>([&](auto handler) {
                managed[4].get_members(lifecycle_group, handler);
            }), "Removed notification barrier");
            {
                std::lock_guard lock(managed_events[4].mutex);
                require(before_removed_publish == std::array<std::size_t, 4>{managed_events[4].messages.size(),
                    managed_events[4].reads.size(), managed_events[4].typing.size(), managed_events[4].updates.size()} &&
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
