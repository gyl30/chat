#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/http/config.hpp>
#include <boost/http/field.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/response.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/status.hpp>
#include <boost/http/version.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/json.hpp>

#include <chat/client.hpp>

#include "websocket.hpp"

namespace
{

using namespace std::chrono_literals;

std::string_view as_string_view(boost::core::string_view value) { return {value.data(), value.size()}; }

class client_test_worker final : public boost::corosio::tcp_server::worker_base
{
   public:
    client_test_worker(boost::corosio::io_context& io_context,
                       boost::http::shared_parser_config parser_config,
                       boost::http::shared_serializer_config serializer_config)
        : io_context_(io_context), socket_(io_context), parser_(std::move(parser_config)), serializer_(std::move(serializer_config))
    {
        serializer_.set_message(response_);
    }

    boost::corosio::tcp_socket& socket() override { return socket_; }

    void run(boost::corosio::tcp_server::launcher launch) override { launch(io_context_.get_executor(), run_session()); }

   private:
    boost::capy::io_task<> send_upgrade_response(std::string_view accept)
    {
        response_.clear();
        response_.set_start_line(boost::http::status::switching_protocols, boost::http::version::http_1_1);
        response_.set(boost::http::field::upgrade, "websocket");
        response_.set(boost::http::field::connection, "Upgrade");
        response_.set(boost::http::field::sec_websocket_accept, accept);

        serializer_.reset();
        serializer_.start();
        while (!serializer_.is_done())
        {
            auto prepared = serializer_.prepare();
            if (prepared.has_error())
            {
                co_return std::error_code(prepared.error());
            }
            if (boost::capy::buffer_empty(*prepared))
            {
                serializer_.consume(0);
                continue;
            }

            auto [ec, written] = co_await boost::capy::write(socket_, *prepared);
            serializer_.consume(written);
            if (ec)
            {
                co_return ec;
            }
        }
        co_return {};
    }

    boost::capy::io_task<> send_text(websocket_connection& connection, boost::json::object object)
    {
        auto text = boost::json::serialize(object);
        co_return co_await connection.send_text(text);
    }

    boost::capy::io_task<bool> handle_request(websocket_connection& connection, std::string_view payload)
    {
        boost::system::error_code ec;
        auto value = boost::json::parse(payload, ec);
        if (ec || !value.is_object())
        {
            co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
        }

        auto const& request = value.as_object();
        auto const* id = request.if_contains("id");
        auto const* method = request.if_contains("method");
        auto const* params = request.if_contains("params");
        if (!id || !method || !method->is_string() || !params || !params->is_object())
        {
            co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
        }

        boost::json::object response;
        response.emplace("jsonrpc", "2.0");
        response.emplace("id", *id);

        if (method->as_string() == "get_conversations")
        {
            boost::json::array conversations;
            auto const* before = params->as_object().if_contains("before");
            if (!before)
            {
                boost::json::object last;
                last.emplace("conversation", 2);
                last.emplace("username", "bob");
                last.emplace("id", 12);
                last.emplace("from", 2);
                last.emplace("timestamp", 1700000000000LL);
                last.emplace("text", "hello");

                boost::json::object conversation;
                conversation.emplace("id", 2);
                conversation.emplace("kind", "direct");
                conversation.emplace("member_count", 2);
                conversation.emplace("user", 2);
                conversation.emplace("username", "bob");
                conversation.emplace("last", std::move(last));
                conversation.emplace("unread", 3);
                conversations.push_back(std::move(conversation));
            }
            else if (before->is_object() && before->as_object().at("id").as_int64() == 2)
            {
                boost::json::object last;
                last.emplace("conversation", 3);
                last.emplace("username", "alice");
                last.emplace("id", 6);
                last.emplace("from", 1);
                last.emplace("timestamp", 1699990000000LL);
                last.emplace("text", "older");

                boost::json::object conversation;
                conversation.emplace("id", 3);
                conversation.emplace("kind", "direct");
                conversation.emplace("member_count", 2);
                conversation.emplace("user", 3);
                conversation.emplace("username", "carol");
                conversation.emplace("last", std::move(last));
                conversation.emplace("unread", 0);
                conversations.push_back(std::move(conversation));
            }
            else
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("next", nullptr);
            result.emplace("conversations", std::move(conversations));
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "get_presence")
        {
            boost::json::object user;
            user.emplace("user", 2);
            user.emplace("online", true);
            user.emplace("last_seen", 1699999999000LL);
            boost::json::array users;
            users.push_back(std::move(user));
            boost::json::object result;
            result.emplace("users", std::move(users));
            response.emplace("result", std::move(result));

            auto [response_ec] = co_await send_text(connection, std::move(response));
            if (response_ec)
            {
                co_return boost::capy::io_result<bool>{response_ec, false};
            }

            boost::json::object notification_params;
            notification_params.emplace("user", 3);
            notification_params.emplace("online", false);
            notification_params.emplace("last_seen", 1700000001000LL);
            boost::json::object notification;
            notification.emplace("jsonrpc", "2.0");
            notification.emplace("method", "presence");
            notification.emplace("params", std::move(notification_params));
            auto [notification_ec] = co_await send_text(connection, std::move(notification));
            co_return boost::capy::io_result<bool>{notification_ec, !notification_ec};
        }

        if (method->as_string() == "get_messages")
        {
            auto const* user = params->as_object().if_contains("conversation");
            auto const* before = params->as_object().if_contains("before");
            if (!user || !user->is_int64() || user->as_int64() != 2)
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::array messages;
            if (!before)
            {
                boost::json::object first;
                first.emplace("conversation", 2);
                first.emplace("username", "bob");
                first.emplace("id", 10);
                first.emplace("from", 2);
                first.emplace("timestamp", 1700000000000);
                first.emplace("text", "first");
                messages.push_back(std::move(first));

                boost::json::object second;
                second.emplace("conversation", 2);
                second.emplace("username", "alice");
                second.emplace("id", 12);
                second.emplace("from", 1);
                second.emplace("timestamp", 1700000060000);
                second.emplace("text", "second");
                messages.push_back(std::move(second));
            }
            else if (before->is_int64() && before->as_int64() == 10)
            {
                boost::json::object older;
                older.emplace("conversation", 2);
                older.emplace("username", "bob");
                older.emplace("id", 4);
                older.emplace("from", 2);
                older.emplace("timestamp", 1699999940000);
                older.emplace("text", "older");
                messages.push_back(std::move(older));
            }
            else
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("messages", std::move(messages));
            result.emplace("read_positions", boost::json::array{boost::json::object{{"user", 2}, {"message", 12}}});
            result.emplace("has_more", false);
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "get_contacts")
        {
            boost::json::object user;
            user.emplace("id", 3);
            user.emplace("username", "carol");
            boost::json::array users;
            users.push_back(std::move(user));
            boost::json::object result;
            result.emplace("users", std::move(users));
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "search_users")
        {
            auto const* query = params->as_object().if_contains("query");
            if (!query || !query->is_string() || query->as_string() != "bo")
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object user;
            user.emplace("id", 2);
            user.emplace("username", "bob");
            boost::json::array users;
            users.push_back(std::move(user));
            boost::json::object result;
            result.emplace("users", std::move(users));
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "add_contact")
        {
            auto const* user_id = params->as_object().if_contains("user");
            if (!user_id || !user_id->is_int64() || user_id->as_int64() != 2)
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object user;
            user.emplace("id", 2);
            user.emplace("username", "bob");
            boost::json::object result;
            result.emplace("user", std::move(user));
            response.emplace("result", std::move(result));

            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() == "send_message")
        {
            auto const* user = params->as_object().if_contains("conversation");
            auto const* text = params->as_object().if_contains("text");
            if (!user || !user->is_int64() || user->as_int64() != 2 || !text || !text->is_string() || text->as_string() != "outgoing")
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("message", 20);
            result.emplace("timestamp", 1700000120000);
            result.emplace("realtime", true);
            response.emplace("result", std::move(result));
            auto [response_ec] = co_await send_text(connection, std::move(response));
            if (response_ec)
            {
                co_return boost::capy::io_result<bool>{response_ec, false};
            }

            boost::json::object notification_params;
            notification_params.emplace("conversation", 2);
            notification_params.emplace("username", "bob");
            notification_params.emplace("id", 21);
            notification_params.emplace("from", 2);
            notification_params.emplace("timestamp", 1700000180000);
            notification_params.emplace("text", "incoming");
            boost::json::object notification;
            notification.emplace("jsonrpc", "2.0");
            notification.emplace("method", "message");
            notification.emplace("params", std::move(notification_params));
            auto [notification_ec] = co_await send_text(connection, std::move(notification));
            co_return boost::capy::io_result<bool>{notification_ec, !notification_ec};
        }

        if (method->as_string() == "mark_read")
        {
            auto const* user = params->as_object().if_contains("conversation");
            auto const* message = params->as_object().if_contains("message");
            if (!user || !user->is_int64() || user->as_int64() != 2 || !message || !message->is_int64() || message->as_int64() != 21)
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("message", 21);
            response.emplace("result", std::move(result));
            auto [send_ec] = co_await send_text(connection, std::move(response));
            if (send_ec)
            {
                co_return boost::capy::io_result<bool>{send_ec, false};
            }

            boost::json::object notification_params;
            notification_params.emplace("user", 2);
            notification_params.emplace("conversation", 2);
            notification_params.emplace("message", 20);
            boost::json::object notification;
            notification.emplace("jsonrpc", "2.0");
            notification.emplace("method", "read");
            notification.emplace("params", std::move(notification_params));
            auto [notification_ec] = co_await send_text(connection, std::move(notification));
            co_return boost::capy::io_result<bool>{notification_ec, !notification_ec};
        }

        if (method->as_string() == "register")
        {
            auto const* username = params->as_object().if_contains("username");
            auto const* password = params->as_object().if_contains("password");
            if (!username || !username->is_string() || username->as_string() != "new_user" || !password ||
                !password->is_string() || password->as_string() != "new_secret")
            {
                co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
            }

            boost::json::object result;
            result.emplace("user", 4);
            response.emplace("result", std::move(result));
            auto [send_ec] = co_await send_text(connection, std::move(response));
            co_return boost::capy::io_result<bool>{send_ec, !send_ec};
        }

        if (method->as_string() != "authenticate")
        {
            co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
        }

        auto const* username = params->as_object().if_contains("username");
        auto const* password = params->as_object().if_contains("password");
        if (!username || !username->is_string() || !password || !password->is_string())
        {
            co_return boost::capy::io_result<bool>{std::make_error_code(std::errc::protocol_error), false};
        }

        auto const name = std::string_view(username->as_string().data(), username->as_string().size());
        auto const secret = std::string_view(password->as_string().data(), password->as_string().size());
        if (name == "close")
        {
            socket_.close();
            co_return boost::capy::io_result<bool>{std::error_code{}, false};
        }

        if (name == "protocol")
        {
            auto [invalid_ec] = co_await connection.send_text("invalid");
            if (invalid_ec)
            {
                co_return boost::capy::io_result<bool>{invalid_ec, false};
            }
        }

        if (name == "rpc")
        {
            boost::json::object error;
            error.emplace("code", -32003);
            error.emplace("message", "Already authenticated");
            response.emplace("error", std::move(error));
        }
        else
        {
            boost::json::object result;
            result.emplace("authenticated", name == "alice" && secret == "secret");
            result.emplace("user", name == "alice" && secret == "secret" ? 1 : 0);
            response.emplace("result", std::move(result));
        }

        auto [send_ec] = co_await send_text(connection, std::move(response));
        co_return boost::capy::io_result<bool>{send_ec, !send_ec};
    }

    boost::capy::task<> run_session()
    {
        parser_.reset();
        parser_.start();

        auto [read_ec] = co_await parser_.read_header(socket_);
        if (read_ec || !parser_.is_complete() || parser_.has_buffered_data())
        {
            socket_.close();
            co_return;
        }

        std::string accept;
        if (as_string_view(parser_.get().target()) != "/ws" || !websocket_upgrade_accept(parser_.get(), accept))
        {
            socket_.close();
            co_return;
        }

        auto [write_ec] = co_await send_upgrade_response(accept);
        if (write_ec)
        {
            socket_.close();
            co_return;
        }

        websocket_connection connection(socket_);
        for (;;)
        {
            auto receive_result = co_await connection.receive();
            auto& [ec, message] = receive_result;
            if (ec || message.message_type == websocket_message::type::close)
            {
                break;
            }

            auto [handle_ec, keep_open] = co_await handle_request(connection, message.payload);
            if (handle_ec || !keep_open)
            {
                break;
            }
        }

        socket_.close();
    }

    boost::corosio::io_context& io_context_;
    boost::corosio::tcp_socket socket_;
    boost::http::request_parser parser_;
    boost::http::response response_;
    boost::http::serializer serializer_;
};

std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> make_workers(
    boost::corosio::io_context& io_context,
    boost::http::shared_parser_config const& parser_config,
    boost::http::shared_serializer_config const& serializer_config)
{
    std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> workers;
    workers.push_back(std::make_unique<client_test_worker>(io_context, parser_config, serializer_config));
    return workers;
}

struct test_state
{
    template<class Predicate>
    bool wait(Predicate predicate)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 5s, predicate);
    }

    std::mutex mutex;
    std::condition_variable condition;
    int connected = 0;
    int disconnected = 0;
    std::vector<chat::error> errors;
    std::vector<chat::message> messages;
    std::vector<std::pair<std::int64_t, std::int64_t>> reads;
    std::vector<chat::presence> presences;
};

boost::capy::task<> stop_server(boost::corosio::tcp_server& server)
{
    server.stop();
    co_return;
}

struct server_guard
{
    boost::corosio::io_context& io_context;
    boost::corosio::tcp_server& server;
    std::thread& thread;

    ~server_guard()
    {
        boost::capy::run_async(io_context.get_executor())(stop_server(server));
        thread.join();
        server.join();
    }
};

}    // namespace

int main()
{
    boost::corosio::io_context server_io_context;
    auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
    auto serializer_config = boost::http::make_serializer_config(boost::http::serializer_config{});

    boost::corosio::tcp_server server(server_io_context, server_io_context.get_executor());
    server.set_workers(make_workers(server_io_context, parser_config, serializer_config));
    if (auto ec = server.bind(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "FAIL client test server bind: " << ec.message() << '\n';
        return 1;
    }
    auto const port = server.local_endpoint().port();
    server.start();

    std::thread server_thread([&server_io_context] { server_io_context.run(); });
    server_guard guard{server_io_context, server, server_thread};

    test_state state;
    chat::client client;
    client.set_connected_handler([&state] {
        std::lock_guard lock(state.mutex);
        ++state.connected;
        state.condition.notify_all();
    });
    client.set_disconnected_handler([&state] {
        std::lock_guard lock(state.mutex);
        ++state.disconnected;
        state.condition.notify_all();
    });
    client.set_error_handler([&state](chat::error const& error) {
        std::lock_guard lock(state.mutex);
        state.errors.push_back(error);
        state.condition.notify_all();
    });
    client.set_message_handler([&state](chat::message message) {
        std::lock_guard lock(state.mutex);
        state.messages.push_back(std::move(message));
        state.condition.notify_all();
    });
    client.set_read_handler(
        [&state](std::int64_t conversation, std::int64_t user, std::int64_t message)
        {
        std::lock_guard lock(state.mutex);
            if (conversation == 2)
            {
        state.reads.emplace_back(user, message);
            }
        state.condition.notify_all();
    });
    client.set_presence_handler([&state](chat::presence value) {
        std::lock_guard lock(state.mutex);
        state.presences.push_back(std::move(value));
        state.condition.notify_all();
    });

    auto url = std::string("ws://127.0.0.1:") + std::to_string(port) + "/ws";
    client.connect(url);
    if (!state.wait([&state] { return state.connected == 1; }))
    {
        std::cerr << "FAIL client connection\n";
        return 1;
    }
    std::cout << "PASS client connection\n";

    bool registered_called = false;
    std::int64_t registered_user = 0;
    client.register_user("new_user", "new_secret", [&](std::expected<std::int64_t, chat::error> result) {
        std::lock_guard lock(state.mutex);
        registered_called = true;
        if (result)
        {
            registered_user = *result;
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return registered_called; }) || registered_user != 4)
    {
        std::cerr << "FAIL client register response\n";
        return 1;
    }
    std::cout << "PASS client register response\n";

    bool authenticated_called = false;
    bool authenticated = false;
    client.authenticate("alice", "secret",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
        authenticated_called = true;
                            authenticated = result && result->authenticated;
        state.condition.notify_all();
    });
    if (!state.wait([&] { return authenticated_called; }) || !authenticated)
    {
        std::cerr << "FAIL client authenticate response\n";
        return 1;
    }
    std::cout << "PASS client authenticate response\n";

    bool presence_called = false;
    std::vector<chat::presence> presence;
    client.get_presence([&](std::expected<std::vector<chat::presence>, chat::error> result) {
        std::lock_guard lock(state.mutex);
        presence_called = true;
        if (result)
        {
            presence = std::move(*result);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return presence_called && !state.presences.empty(); }) || presence.size() != 1 ||
        presence[0].user != 2 || !presence[0].online || presence[0].last_seen != 1699999999000LL ||
        state.presences.back().user != 3 || state.presences.back().online ||
        state.presences.back().last_seen != 1700000001000LL)
    {
        std::cerr << "FAIL client presence\n";
        return 1;
    }
    std::cout << "PASS client presence\n";

    bool contacts_called = false;
    std::vector<chat::user> contacts;
    client.get_contacts([&](std::expected<std::vector<chat::user>, chat::error> result) {
        std::lock_guard lock(state.mutex);
        contacts_called = true;
        if (result)
        {
            contacts = std::move(*result);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return contacts_called; }) || contacts.size() != 1 || contacts[0].id != 3 || contacts[0].username != "carol")
    {
        std::cerr << "FAIL client contacts\n";
        return 1;
    }
    std::cout << "PASS client contacts\n";

    bool users_called = false;
    std::vector<chat::user> users;
    client.search_users("bo", [&](std::expected<std::vector<chat::user>, chat::error> result) {
        std::lock_guard lock(state.mutex);
        users_called = true;
        if (result)
        {
            users = std::move(*result);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return users_called; }) || users.size() != 1 || users[0].id != 2 || users[0].username != "bob")
    {
        std::cerr << "FAIL client user search\n";
        return 1;
    }
    std::cout << "PASS client user search\n";

    bool contact_added_called = false;
    chat::user added_contact;
    client.add_contact(2, [&](std::expected<chat::user, chat::error> result) {
        std::lock_guard lock(state.mutex);
        contact_added_called = true;
        if (result)
        {
            added_contact = std::move(*result);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return contact_added_called; }) || added_contact.id != 2 || added_contact.username != "bob")
    {
        std::cerr << "FAIL client add contact\n";
        return 1;
    }
    std::cout << "PASS client add contact\n";

    bool conversations_called = false;
    std::vector<chat::conversation> conversations;
    client.get_conversations({},
                             [&](std::expected<chat::conversations_result, chat::error> result)
                             {
        std::lock_guard lock(state.mutex);
        conversations_called = true;
        if (result)
        {
                                     conversations = std::move(result->conversations);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return conversations_called; }) || conversations.size() != 1 || conversations[0].user != 2 ||
        conversations[0].username != "bob" || conversations[0].last.id != 12 || conversations[0].last.from != 2 ||
        conversations[0].last.timestamp != 1700000000000LL || conversations[0].last.text != "hello" ||
        conversations[0].unread != 3)
    {
        std::cerr << "FAIL client conversations\n";
        return 1;
    }
    std::cout << "PASS client conversations\n";

    bool conversation_cursor_called = false;
    std::vector<chat::conversation> older_conversations;
    client.get_conversations(chat::conversation_cursor{1700000000000LL, 2},
                             [&](std::expected<chat::conversations_result, chat::error> result)
                             {
        std::lock_guard lock(state.mutex);
        conversation_cursor_called = true;
        if (result)
        {
                                     older_conversations = std::move(result->conversations);
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return conversation_cursor_called; }) || older_conversations.size() != 1 ||
        older_conversations[0].user != 3 || older_conversations[0].last.id != 6)
    {
        std::cerr << "FAIL client conversation cursor\n";
        return 1;
    }
    std::cout << "PASS client conversation cursor\n";

    bool messages_called = false;
    chat::messages_result messages_result;
    client.get_messages(2, {}, [&](std::expected<chat::messages_result, chat::error> result) {
        std::lock_guard lock(state.mutex);
        messages_called = true;
        if (result)
        {
            messages_result = std::move(*result);
        }
        state.condition.notify_all();
    });
    auto const& messages = messages_result.messages;
    if (!state.wait([&] { return messages_called; }) ||
        (messages_result.read_positions.size() != 1 || messages_result.read_positions[0].message != 12) ||
        messages.size() != 2 || messages[0].id != 10 || messages[0].from != 2 ||
        messages[0].timestamp != 1700000000000 || messages[0].text != "first" || messages[1].id != 12 ||
        messages[1].from != 1 || messages[1].timestamp != 1700000060000 || messages[1].text != "second")
    {
        std::cerr << "FAIL client messages\n";
        return 1;
    }
    std::cout << "PASS client messages\n";

    bool older_messages_called = false;
    chat::messages_result older_messages_result;
    client.get_messages(2, 10, [&](std::expected<chat::messages_result, chat::error> result) {
        std::lock_guard lock(state.mutex);
        older_messages_called = true;
        if (result)
        {
            older_messages_result = std::move(*result);
        }
        state.condition.notify_all();
    });
    auto const& older_messages = older_messages_result.messages;
    if (!state.wait([&] { return older_messages_called; }) ||
        (older_messages_result.read_positions.size() != 1 || older_messages_result.read_positions[0].message != 12) ||
        older_messages.size() != 1 || older_messages[0].id != 4 || older_messages[0].from != 2 ||
        older_messages[0].timestamp != 1699999940000 || older_messages[0].text != "older")
    {
        std::cerr << "FAIL client message cursor\n";
        return 1;
    }
    std::cout << "PASS client message cursor\n";

    bool send_called = false;
    chat::send_message_result send_result;
    client.send_message(2, "outgoing", [&](std::expected<chat::send_message_result, chat::error> result) {
        std::lock_guard lock(state.mutex);
        send_called = true;
        if (result)
        {
            send_result = *result;
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return send_called && !state.messages.empty(); }) || send_result.message_id != 20 ||
        send_result.timestamp != 1700000120000 || !send_result.realtime || state.messages.back().id != 21 ||
        state.messages.back().from != 2 || state.messages.back().timestamp != 1700000180000 ||
        state.messages.back().text != "incoming")
    {
        std::cerr << "FAIL client send and notification\n";
        return 1;
    }
    std::cout << "PASS client send and notification\n";

    bool mark_read_called = false;
    std::int64_t read_message = 0;
    client.mark_read(2, 21, [&](std::expected<std::int64_t, chat::error> result) {
        std::lock_guard lock(state.mutex);
        mark_read_called = true;
        if (result)
        {
            read_message = *result;
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return mark_read_called && !state.reads.empty(); }) || read_message != 21 ||
        state.reads.back().first != 2 || state.reads.back().second != 20)
    {
        std::cerr << "FAIL client mark read and read notification\n";
        return 1;
    }
    std::cout << "PASS client mark read and read notification\n";

    bool rejected_called = false;
    bool rejected = false;
    client.authenticate("alice", "wrong",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
        rejected_called = true;
                            rejected = result && !result->authenticated;
        state.condition.notify_all();
    });
    if (!state.wait([&] { return rejected_called; }) || !rejected)
    {
        std::cerr << "FAIL client authenticate rejection\n";
        return 1;
    }
    std::cout << "PASS client authenticate rejection\n";

    bool rpc_error_called = false;
    chat::error rpc_error;
    client.authenticate("rpc", "secret",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
        if (!result)
        {
            rpc_error_called = true;
            rpc_error = result.error();
        }
        state.condition.notify_all();
    });
    if (!state.wait([&] { return rpc_error_called; }) || rpc_error.kind != chat::error_kind::rpc || rpc_error.code != -32003 ||
        rpc_error.message != "Already authenticated")
    {
        std::cerr << "FAIL client rpc error\n";
        return 1;
    }
    std::cout << "PASS client rpc error\n";

    bool protocol_response_called = false;
    client.authenticate("protocol", "secret",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
                            protocol_response_called = result && !result->authenticated;
        state.condition.notify_all();
    });
    if (!state.wait([&] { return !state.errors.empty() && protocol_response_called; }) || state.errors.back().kind != chat::error_kind::protocol)
    {
        std::cerr << "FAIL client protocol error\n";
        return 1;
    }
    std::cout << "PASS client protocol error\n";

    client.close();
    if (!state.wait([&state] { return state.disconnected == 1; }))
    {
        std::cerr << "FAIL client close\n";
        return 1;
    }
    std::cout << "PASS client close\n";

    client.connect(url);
    if (!state.wait([&state] { return state.connected == 2; }))
    {
        std::cerr << "FAIL client reconnect\n";
        return 1;
    }
    std::cout << "PASS client reconnect\n";

    bool close_error_called = false;
    client.authenticate("close", "secret",
                        [&](std::expected<chat::authentication_result, chat::error> result)
                        {
        std::lock_guard lock(state.mutex);
        close_error_called = !result && result.error().kind == chat::error_kind::transport;
        state.condition.notify_all();
    });
    if (!state.wait([&] { return close_error_called && state.disconnected == 2; }))
    {
        std::cerr << "FAIL client pending request close\n";
        return 1;
    }
    std::cout << "PASS client pending request close\n";

    return 0;
}
