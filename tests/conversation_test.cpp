#include <algorithm>
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

#include "server.hpp"

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
    std::vector<chat::read_position> reads;

    template <class F> void wait(F predicate)
    {
        std::unique_lock lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(5), predicate), "Notification timeout");
    }

    void attach(chat::client& client)
    {
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
            [this](std::int64_t conversation)
            {
                std::lock_guard lock(mutex);
                conversations.push_back(conversation);
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
        PQconnectdb("hostaddr=172.20.54.83 port=5432 dbname=chat user=chat sslmode=disable"), &PQfinish};
    std::vector<std::int64_t> users;
    std::vector<std::int64_t> groups;

    void execute(std::string const& query)
    {
        std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(database.get(), query.c_str()), &PQclear);
        require(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK, PQerrorMessage(database.get()));
    }

    void cleanup()
    {
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
                       "hostaddr=172.20.54.83 port=5432 dbname=chat user=chat sslmode=disable", 4};
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
        auto members = call<std::vector<chat::user>>([&](auto handler) { c.get_members(group, handler); });
        require(members && members->size() == 3 && (*members)[0].id == data.users[0], "Group members");
        auto forbidden = call<chat::messages_result>([&](auto handler) { d.get_messages(group, {}, handler); });
        require(!forbidden && forbidden.error().code == -32006, "Nonmember history denied");
        auto forbidden_members = call<std::vector<chat::user>>([&](auto handler) { d.get_members(group, handler); });
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
        {
            std::lock_guard lock(d_events.mutex);
            require(d_events.messages.empty() && d_events.updates.empty() && d_events.reads.empty() &&
                        d_events.conversations.empty(),
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
