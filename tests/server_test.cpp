#include <array>
#include <tuple>
#include <string>
#include <vector>
#include <cstdint>
#include <utility>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <system_error>

#include <boost/json.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/http/field.hpp>
#include <boost/http/config.hpp>
#include <boost/http/method.hpp>
#include <boost/http/status.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/http/server/router.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/http/response_parser.hpp>

#include "pg_connection.hpp"
#include "server.hpp"

namespace
{

bool json_contains(boost::json::value const& actual, boost::json::value const& expected)
{
    if (expected.is_object())
    {
        if (!actual.is_object())
        {
            return false;
        }
        for (auto const& field : expected.as_object())
        {
            auto const* found = actual.as_object().if_contains(field.key());
            if (!found || !json_contains(*found, field.value()))
            {
                return false;
            }
        }
        return true;
    }
    if (expected.is_array())
    {
        if (!actual.is_array() || actual.as_array().size() != expected.as_array().size())
        {
            return false;
        }
        for (std::size_t i = 0; i < expected.as_array().size(); ++i)
        {
            if (!json_contains(actual.as_array()[i], expected.as_array()[i]))
            {
                return false;
            }
        }
        return true;
    }
    return actual == expected;
}

bool json_matches(std::string_view actual, std::string_view expected)
{
    boost::system::error_code actual_ec, expected_ec;
    auto a = boost::json::parse(actual, actual_ec);
    auto e = boost::json::parse(expected, expected_ec);
    return !actual_ec && !expected_ec && json_contains(a, e);
}

constexpr std::string_view kHealthBody = R"({"status":"ok"})";
constexpr std::string_view kNotFoundBody = "not found";
constexpr std::string_view kWebSocketKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr std::string_view kWebSocketAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
constexpr std::string_view kDatabaseConnectionString = "";
constexpr char kTestUsername[] = "chat_server_test";
constexpr char kTestPassword[] = "test password";
constexpr char kPeerUsername[] = "chat_server_peer";

boost::http::route_task health_handler(boost::http::route_params& params)
{
    params.status(boost::http::status::ok);
    params.res.set(boost::http::field::content_type, "application/json");

    auto [ec] = co_await params.send(kHealthBody);
    if (ec)
    {
        co_return boost::http::route_error(ec);
    }

    co_return boost::http::route_done;
}

boost::http::route_task not_found_handler(boost::http::route_params& params)
{
    params.status(boost::http::status::not_found);

    auto [ec] = co_await params.send(kNotFoundBody);
    if (ec)
    {
        co_return boost::http::route_error(ec);
    }

    co_return boost::http::route_done;
}

struct server_stop_guard
{
    chat_server& server;

    ~server_stop_guard() { server.stop(); }
};

boost::capy::io_task<> connect(boost::corosio::tcp_socket& socket, unsigned short port)
{
    auto [ec] = co_await socket.connect(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), port));
    co_return ec;
}

boost::capy::io_task<> send_request(boost::corosio::tcp_socket& socket, std::string_view request)
{
    auto [ec, written] = co_await boost::capy::write(socket, boost::capy::const_buffer(request.data(), request.size()));
    if (ec)
    {
        co_return ec;
    }
    if (written != request.size())
    {
        co_return std::make_error_code(std::errc::io_error);
    }
    co_return {};
}

std::vector<std::uint8_t> make_masked_frame(std::uint8_t opcode, bool final, std::string_view payload)
{
    constexpr std::array<std::uint8_t, 4> mask = {1, 2, 3, 4};

    std::size_t mask_offset = 2;
    if (payload.size() <= 125)
    {
        mask_offset = 2;
    }
    else if (payload.size() <= 0xffff)
    {
        mask_offset = 4;
    }
    else
    {
        mask_offset = 10;
    }

    std::vector<std::uint8_t> frame(mask_offset + mask.size() + payload.size());
    frame[0] = static_cast<std::uint8_t>((final ? 0x80U : 0U) | opcode);
    if (payload.size() <= 125)
    {
        frame[1] = static_cast<std::uint8_t>(0x80U | payload.size());
    }
    else if (payload.size() <= 0xffff)
    {
        frame[1] = 0xfe;
        frame[2] = static_cast<std::uint8_t>(payload.size() >> 8);
        frame[3] = static_cast<std::uint8_t>(payload.size());
    }
    else
    {
        frame[1] = 0xff;
        auto const size = static_cast<std::uint64_t>(payload.size());
        for (std::size_t i = 0; i < 8; ++i)
        {
            frame[2 + i] = static_cast<std::uint8_t>(size >> ((7 - i) * 8));
        }
    }

    for (std::size_t i = 0; i < mask.size(); ++i)
    {
        frame[mask_offset + i] = mask[i];
    }

    auto const payload_offset = mask_offset + mask.size();
    for (std::size_t i = 0; i < payload.size(); ++i)
    {
        frame[payload_offset + i] = static_cast<std::uint8_t>(payload[i]) ^ mask[i % mask.size()];
    }
    return frame;
}

boost::capy::io_task<> send_websocket_text(boost::corosio::tcp_socket& socket, std::string_view payload)
{
    auto frame = make_masked_frame(0x01, true, payload);
    if (frame.empty())
    {
        co_return std::make_error_code(std::errc::message_size);
    }

    auto [ec, written] = co_await boost::capy::write(socket, boost::capy::const_buffer(frame.data(), frame.size()));
    if (ec)
    {
        co_return ec;
    }
    if (written != frame.size())
    {
        co_return std::make_error_code(std::errc::io_error);
    }
    co_return {};
}

boost::capy::io_task<std::string> receive_websocket_text(boost::corosio::tcp_socket& socket)
{
    std::array<std::uint8_t, 2> header{};
    auto [header_ec, header_read] = co_await boost::capy::read(socket, boost::capy::mutable_buffer(header.data(), header.size()));
    if (header_ec)
    {
        co_return boost::capy::io_result<std::string>{header_ec, {}};
    }
    if (header_read != header.size() || header[0] != 0x81 || (header[1] & 0x80U) != 0)
    {
        co_return boost::capy::io_result<std::string>{std::make_error_code(std::errc::protocol_error), {}};
    }

    std::uint64_t payload_size = header[1] & 0x7fU;
    if (payload_size == 126)
    {
        std::array<std::uint8_t, 2> length{};
        auto [length_ec, length_read] = co_await boost::capy::read(socket, boost::capy::mutable_buffer(length.data(), length.size()));
        if (length_ec)
        {
            co_return boost::capy::io_result<std::string>{length_ec, {}};
        }
        if (length_read != length.size())
        {
            co_return boost::capy::io_result<std::string>{std::make_error_code(std::errc::io_error), {}};
        }
        payload_size = (static_cast<std::uint64_t>(length[0]) << 8) | length[1];
    }
    else if (payload_size == 127)
    {
        std::array<std::uint8_t, 8> length{};
        auto [length_ec, length_read] = co_await boost::capy::read(socket, boost::capy::mutable_buffer(length.data(), length.size()));
        if (length_ec)
        {
            co_return boost::capy::io_result<std::string>{length_ec, {}};
        }
        if (length_read != length.size() || (length[0] & 0x80U) != 0)
        {
            co_return boost::capy::io_result<std::string>{std::make_error_code(std::errc::protocol_error), {}};
        }
        payload_size = 0;
        for (auto byte : length)
        {
            payload_size = (payload_size << 8) | byte;
        }
    }

    if (payload_size > std::string{}.max_size())
    {
        co_return boost::capy::io_result<std::string>{std::make_error_code(std::errc::message_size), {}};
    }

    std::string payload(static_cast<std::size_t>(payload_size), '\0');
    if (!payload.empty())
    {
        auto [payload_ec, payload_read] = co_await boost::capy::read(socket, boost::capy::mutable_buffer(payload.data(), payload.size()));
        if (payload_ec)
        {
            co_return boost::capy::io_result<std::string>{payload_ec, {}};
        }
        if (payload_read != payload.size())
        {
            co_return boost::capy::io_result<std::string>{std::make_error_code(std::errc::io_error), {}};
        }
    }

    co_return boost::capy::io_result<std::string>{std::error_code{}, std::move(payload)};
}

boost::capy::io_task<> authenticate_websocket(boost::corosio::tcp_socket& socket,
                                                    std::string_view username,
                                                    std::string_view request_id)
{
    std::string request = R"({"jsonrpc":"2.0","method":"authenticate","params":{"username":")";
    request.append(username);
    request.append(R"(","password":")");
    request.append(kTestPassword);
    request.append(R"("},"id":")");
    request.append(request_id);
    request.append(R"("})");

    auto write_result = co_await send_websocket_text(socket, request);
    auto& [write_ec] = write_result;
    if (write_ec)
    {
        co_return write_ec;
    }

    auto reply_result = co_await receive_websocket_text(socket);
    auto& [read_ec, reply] = reply_result;
    if (read_ec)
    {
        co_return read_ec;
    }

    std::string expected = R"({"jsonrpc":"2.0","result":{"authenticated":true},"id":")";
    expected.append(request_id);
    expected.append(R"("})");
    if (!json_matches(reply, expected))
    {
        co_return std::make_error_code(std::errc::protocol_error);
    }

    co_return {};
}

bool valid_health_response(boost::http::response_parser const& parser)
{
    return parser.get().status() == boost::http::status::ok && parser.body() == kHealthBody;
}

boost::capy::io_task<> upgrade_websocket(boost::corosio::tcp_socket& socket, boost::http::response_parser& parser)
{
    std::string request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: ";
    request.append(kWebSocketKey);
    request.append("\r\nSec-WebSocket-Version: 13\r\n\r\n");

    auto [write_ec] = co_await send_request(socket, request);
    if (write_ec)
    {
        co_return write_ec;
    }

    parser.reset();
    parser.start();
    auto [read_ec] = co_await parser.read(socket);
    if (read_ec)
    {
        co_return read_ec;
    }

    auto const accept = parser.get().value_or(boost::http::field::sec_websocket_accept, "");
    if (parser.get().status() != boost::http::status::switching_protocols || std::string_view(accept.data(), accept.size()) != kWebSocketAccept ||
        parser.has_buffered_data())
    {
        co_return std::make_error_code(std::errc::protocol_error);
    }

    co_return {};
}

boost::capy::task<std::string> open_direct(boost::corosio::tcp_socket& socket, std::string const& user)
{
    auto [write_ec] = co_await send_websocket_text(
        socket, "{\"jsonrpc\":\"2.0\",\"method\":\"open_direct_conversation\",\"params\":{\"user\":" + user +
                    "},\"id\":\"open-direct\"}");
    if (write_ec)
    {
        co_return std::string{};
    }
    auto reply_result = co_await receive_websocket_text(socket);
    auto& [ec, reply] = reply_result;
    if (ec)
    {
        co_return std::string{};
    }
    boost::system::error_code parse_ec;
    auto value = boost::json::parse(reply, parse_ec);
    if (parse_ec || !value.is_object() || !value.as_object().contains("result"))
    {
        co_return std::string{};
    }
    co_return std::to_string(value.as_object().at("result").as_object().at("conversation").as_int64());
}

boost::capy::task<int> run_client(boost::corosio::io_context& io_context, chat_server& server, unsigned short port)
{
    server_stop_guard stop_guard{server};

    pg_connection fixture_connection(io_context);
    auto [fixture_connect_ec] = co_await fixture_connection.connect(std::string(kDatabaseConnectionString));
    if (fixture_connect_ec)
    {
        std::cerr << "FAIL authentication fixture connect: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    std::vector<std::string> cleanup_parameters;
    cleanup_parameters.emplace_back(kTestUsername);
    auto cleanup_result = co_await fixture_connection.execute_row(
        "DELETE FROM users WHERE username = $1 RETURNING id::text", std::move(cleanup_parameters));
    auto& [cleanup_ec, cleanup_row] = cleanup_result;
    (void)cleanup_row;
    if (cleanup_ec)
    {
        std::cerr << "FAIL registration fixture cleanup: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    {
        boost::corosio::tcp_socket socket(io_context);
        auto [connect_ec] = co_await connect(socket, port);
        if (connect_ec)
        {
            std::cerr << "FAIL server HTTP connect: " << connect_ec.message() << '\n';
            co_return 1;
        }

        auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
        boost::http::response_parser parser(parser_config);

        constexpr std::string_view health_request =
            "GET /health HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";
        auto [first_write_ec] = co_await send_request(socket, health_request);
        if (first_write_ec)
        {
            std::cerr << "FAIL first HTTP request: " << first_write_ec.message() << '\n';
            co_return 1;
        }

        parser.reset();
        parser.start();
        auto [first_read_ec] = co_await parser.read(socket);
        if (first_read_ec || !valid_health_response(parser))
        {
            std::cerr << "FAIL first HTTP response\n";
            co_return 1;
        }

        constexpr std::string_view missing_request =
            "GET /missing HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        auto [second_write_ec] = co_await send_request(socket, missing_request);
        if (second_write_ec)
        {
            std::cerr << "FAIL second HTTP request: " << second_write_ec.message() << '\n';
            co_return 1;
        }

        parser.start();
        auto [second_read_ec] = co_await parser.read(socket);
        if (second_read_ec || parser.get().status() != boost::http::status::not_found || parser.body() != kNotFoundBody)
        {
            std::cerr << "FAIL second HTTP response\n";
            co_return 1;
        }

        socket.close();
        std::cout << "PASS server HTTP keep-alive\n";
    }

    {
        boost::corosio::tcp_socket socket(io_context);
        auto [connect_ec] = co_await connect(socket, port);
        if (connect_ec)
        {
            std::cerr << "FAIL WebSocket connect: " << connect_ec.message() << '\n';
            co_return 1;
        }

        auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
        boost::http::response_parser parser(parser_config);
        auto [upgrade_ec] = co_await upgrade_websocket(socket, parser);
        if (upgrade_ec)
        {
            std::cerr << "FAIL server WebSocket upgrade: " << upgrade_ec.message() << '\n';
            co_return 1;
        }
        std::cout << "PASS server WebSocket upgrade\n";

        constexpr std::array<std::uint8_t, 8> binary = {0x82, 0x82, 0x05, 0x06, 0x07, 0x08, 0x04, 0x04};
        auto [binary_ec, binary_written] = co_await boost::capy::write(socket, boost::capy::const_buffer(binary.data(), binary.size()));
        if (binary_ec || binary_written != binary.size())
        {
            std::cerr << "FAIL server binary message write\n";
            co_return 1;
        }
        std::cout << "PASS server binary message ignored\n";

        constexpr std::string_view invalid_registration =
            R"({"jsonrpc":"2.0","method":"register","params":{"username":"chat_server_test","password":""},"id":"register-invalid"})";
        auto [invalid_registration_write_ec] = co_await send_websocket_text(socket, invalid_registration);
        if (invalid_registration_write_ec)
        {
            std::cerr << "FAIL registration invalid params write\n";
            co_return 1;
        }

        auto invalid_registration_reply_result = co_await receive_websocket_text(socket);
        auto& [invalid_registration_read_ec, invalid_registration_reply] = invalid_registration_reply_result;
        constexpr std::string_view expected_invalid_registration_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32602,"message":"Invalid params"},"id":"register-invalid"})";
        if (invalid_registration_read_ec ||
            !json_matches(invalid_registration_reply, expected_invalid_registration_reply))
        {
            std::cerr << "FAIL registration invalid params\n";
            co_return 1;
        }
        std::cout << "PASS registration invalid params\n";

        std::string long_password_registration =
            R"({"jsonrpc":"2.0","method":"register","params":{"username":"chat_server_test","password":")";
        long_password_registration.append(73, 'x');
        long_password_registration.append(R"("},"id":"register-long"})");
        auto [long_password_registration_write_ec] = co_await send_websocket_text(socket, long_password_registration);
        if (long_password_registration_write_ec)
        {
            std::cerr << "FAIL registration password limit write\n";
            co_return 1;
        }

        auto long_password_registration_reply_result = co_await receive_websocket_text(socket);
        auto& [long_password_registration_read_ec, long_password_registration_reply] = long_password_registration_reply_result;
        constexpr std::string_view expected_long_password_registration_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32602,"message":"Invalid params"},"id":"register-long"})";
        if (long_password_registration_read_ec ||
            !json_matches(long_password_registration_reply, expected_long_password_registration_reply))
        {
            std::cerr << "FAIL registration password limit\n";
            co_return 1;
        }
        std::cout << "PASS registration password limit\n";

        constexpr std::string_view registration_request =
            R"({"jsonrpc":"2.0","method":"register","params":{"username":"chat_server_test","password":"test password"},"id":"register-ok"})";
        auto [registration_write_ec] = co_await send_websocket_text(socket, registration_request);
        if (registration_write_ec)
        {
            std::cerr << "FAIL registration success write\n";
            co_return 1;
        }

        auto registration_reply_result = co_await receive_websocket_text(socket);
        auto& [registration_read_ec, registration_reply] = registration_reply_result;
        if (registration_read_ec)
        {
            std::cerr << "FAIL registration success read\n";
            co_return 1;
        }

        std::vector<std::string> registered_user_parameters;
        registered_user_parameters.emplace_back(kTestUsername);
        auto registered_user_result = co_await fixture_connection.execute_row(
            "SELECT id::text, password_hash FROM users WHERE username = $1", std::move(registered_user_parameters));
        auto& [registered_user_ec, registered_user] = registered_user_result;
        if (registered_user_ec || !registered_user || registered_user->size() != 2 || registered_user->at(1).size() != 60 ||
            !registered_user->at(1).starts_with("$2b$12$"))
        {
            std::cerr << "FAIL registration verification: " << fixture_connection.error_message() << '\n';
            co_return 1;
        }

        std::string expected_registration_reply = R"({"jsonrpc":"2.0","result":{"user":)";
        expected_registration_reply.append(registered_user->front());
        expected_registration_reply.append(R"(},"id":"register-ok"})");
        if (!json_matches(registration_reply, expected_registration_reply))
        {
            std::cerr << "FAIL registration success\n";
            co_return 1;
        }
        std::cout << "PASS registration success\n";

        constexpr std::string_view duplicate_registration =
            R"({"jsonrpc":"2.0","method":"register","params":{"username":"chat_server_test","password":"test password"},"id":"register-duplicate"})";
        auto [duplicate_registration_write_ec] = co_await send_websocket_text(socket, duplicate_registration);
        if (duplicate_registration_write_ec)
        {
            std::cerr << "FAIL registration duplicate write\n";
            co_return 1;
        }

        auto duplicate_registration_reply_result = co_await receive_websocket_text(socket);
        auto& [duplicate_registration_read_ec, duplicate_registration_reply] = duplicate_registration_reply_result;
        constexpr std::string_view expected_duplicate_registration_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32002,"message":"Username already exists"},"id":"register-duplicate"})";
        if (duplicate_registration_read_ec ||
            !json_matches(duplicate_registration_reply, expected_duplicate_registration_reply))
        {
            std::cerr << "FAIL registration duplicate\n";
            co_return 1;
        }
        std::cout << "PASS registration duplicate\n";

        for (auto const* method : {"begin_avatar_upload", "upload_avatar_chunk", "finish_avatar_upload",
                                    "cancel_avatar_upload", "get_avatar", "clear_avatar"})
        {
            auto [write_ec] = co_await send_websocket_text(socket,
                std::string("{\"jsonrpc\":\"2.0\",\"method\":\"") + method + "\",\"params\":{},\"id\":\"avatar-auth\"}");
            auto reply_result = co_await receive_websocket_text(socket);
            auto& [read_ec, reply] = reply_result;
            if (write_ec || read_ec || !json_matches(reply, R"({"error":{"code":-32001}})"))
            {
                std::cerr << "FAIL unauthenticated avatar RPC\n";
                co_return 1;
            }
        }
        constexpr std::string_view notification = R"({"jsonrpc":"2.0","method":"echo","params":{"text":"ignored"}})";
        auto [notification_ec] = co_await send_websocket_text(socket, notification);
        if (notification_ec)
        {
            std::cerr << "FAIL server JSON-RPC notification write\n";
            co_return 1;
        }

        constexpr std::string_view unauthenticated_echo_request =
            R"({"jsonrpc":"2.0","method":"echo","params":{"text":"hello chat"},"id":"auth-required"})";
        auto [unauthenticated_echo_write_ec] = co_await send_websocket_text(socket, unauthenticated_echo_request);
        if (unauthenticated_echo_write_ec)
        {
            std::cerr << "FAIL unauthenticated echo write\n";
            co_return 1;
        }

        auto unauthenticated_echo_reply_result = co_await receive_websocket_text(socket);
        auto& [unauthenticated_echo_read_ec, unauthenticated_echo_reply] = unauthenticated_echo_reply_result;
        constexpr std::string_view expected_unauthenticated_echo_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"},"id":"auth-required"})";
        if (unauthenticated_echo_read_ec ||
            !json_matches(unauthenticated_echo_reply, expected_unauthenticated_echo_reply))
        {
            std::cerr << "FAIL unauthenticated echo rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated echo rejected\n";

        constexpr std::string_view unauthenticated_send_message =
            R"({"jsonrpc":"2.0","method":"send_message","params":{"conversation":1,"text":"hello"},"id":"send-auth-required"})";
        auto [unauthenticated_send_message_write_ec] = co_await send_websocket_text(socket, unauthenticated_send_message);
        if (unauthenticated_send_message_write_ec)
        {
            std::cerr << "FAIL unauthenticated send message write\n";
            co_return 1;
        }

        auto unauthenticated_send_message_reply_result = co_await receive_websocket_text(socket);
        auto& [unauthenticated_send_message_read_ec, unauthenticated_send_message_reply] = unauthenticated_send_message_reply_result;
        constexpr std::string_view expected_unauthenticated_send_message_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"},"id":"send-auth-required"})";
        if (unauthenticated_send_message_read_ec ||
            !json_matches(unauthenticated_send_message_reply, expected_unauthenticated_send_message_reply))
        {
            std::cerr << "FAIL unauthenticated send message rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated send message rejected\n";

        constexpr std::string_view unauthenticated_get_messages =
            R"({"jsonrpc":"2.0","method":"get_messages","params":{"conversation":1},"id":"messages-auth-required"})";
        auto [unauthenticated_get_messages_write_ec] = co_await send_websocket_text(socket, unauthenticated_get_messages);
        if (unauthenticated_get_messages_write_ec)
        {
            std::cerr << "FAIL unauthenticated get messages write\n";
            co_return 1;
        }

        auto unauthenticated_get_messages_reply_result = co_await receive_websocket_text(socket);
        auto& [unauthenticated_get_messages_read_ec, unauthenticated_get_messages_reply] = unauthenticated_get_messages_reply_result;
        constexpr std::string_view expected_unauthenticated_get_messages_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"},"id":"messages-auth-required"})";
        if (unauthenticated_get_messages_read_ec ||
            !json_matches(unauthenticated_get_messages_reply, expected_unauthenticated_get_messages_reply))
        {
            std::cerr << "FAIL unauthenticated get messages rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated get messages rejected\n";

        constexpr std::string_view unauthenticated_get_conversations =
            R"({"jsonrpc":"2.0","method":"get_conversations","id":"conversations-auth-required"})";
        auto [unauthenticated_get_conversations_write_ec] =
            co_await send_websocket_text(socket, unauthenticated_get_conversations);
        if (unauthenticated_get_conversations_write_ec)
        {
            std::cerr << "FAIL unauthenticated get conversations write\n";
            co_return 1;
        }

        auto unauthenticated_get_conversations_reply_result = co_await receive_websocket_text(socket);
        auto& [unauthenticated_get_conversations_read_ec, unauthenticated_get_conversations_reply] =
            unauthenticated_get_conversations_reply_result;
        constexpr std::string_view expected_unauthenticated_get_conversations_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"},"id":"conversations-auth-required"})";
        if (unauthenticated_get_conversations_read_ec ||
            !json_matches(unauthenticated_get_conversations_reply, expected_unauthenticated_get_conversations_reply))
        {
            std::cerr << "FAIL unauthenticated get conversations rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated get conversations rejected\n";

        constexpr std::string_view unauthenticated_get_contacts =
            R"({"jsonrpc":"2.0","method":"get_contacts","id":"contacts-auth-required"})";
        auto [unauthenticated_get_contacts_write_ec] =
            co_await send_websocket_text(socket, unauthenticated_get_contacts);
        if (unauthenticated_get_contacts_write_ec)
        {
            std::cerr << "FAIL unauthenticated get contacts write\n";
            co_return 1;
        }

        auto unauthenticated_get_contacts_reply_result = co_await receive_websocket_text(socket);
        auto& [unauthenticated_get_contacts_read_ec, unauthenticated_get_contacts_reply] =
            unauthenticated_get_contacts_reply_result;
        constexpr std::string_view expected_unauthenticated_get_contacts_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"},"id":"contacts-auth-required"})";
        if (unauthenticated_get_contacts_read_ec ||
            !json_matches(unauthenticated_get_contacts_reply, expected_unauthenticated_get_contacts_reply))
        {
            std::cerr << "FAIL unauthenticated get contacts rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated get contacts rejected\n";

        constexpr std::string_view unauthenticated_add_contact =
            R"({"jsonrpc":"2.0","method":"add_contact","params":{"user":1},"id":"add-contact-auth-required"})";
        auto [unauthenticated_add_contact_write_ec] =
            co_await send_websocket_text(socket, unauthenticated_add_contact);
        if (unauthenticated_add_contact_write_ec)
        {
            std::cerr << "FAIL unauthenticated add contact write\n";
            co_return 1;
        }

        auto unauthenticated_add_contact_reply_result = co_await receive_websocket_text(socket);
        auto& [unauthenticated_add_contact_read_ec, unauthenticated_add_contact_reply] =
            unauthenticated_add_contact_reply_result;
        constexpr std::string_view expected_unauthenticated_add_contact_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"},"id":"add-contact-auth-required"})";
        if (unauthenticated_add_contact_read_ec ||
            !json_matches(unauthenticated_add_contact_reply, expected_unauthenticated_add_contact_reply))
        {
            std::cerr << "FAIL unauthenticated add contact rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated add contact rejected\n";

        constexpr std::string_view unauthenticated_search_users =
            R"({"jsonrpc":"2.0","method":"search_users","params":{"query":"chat"},"id":"search-auth-required"})";
        auto [unauthenticated_search_users_write_ec] = co_await send_websocket_text(socket, unauthenticated_search_users);
        if (unauthenticated_search_users_write_ec)
        {
            std::cerr << "FAIL unauthenticated search users write\n";
            co_return 1;
        }

        auto unauthenticated_search_users_reply_result = co_await receive_websocket_text(socket);
        auto& [unauthenticated_search_users_read_ec, unauthenticated_search_users_reply] =
            unauthenticated_search_users_reply_result;
        constexpr std::string_view expected_unauthenticated_search_users_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"},"id":"search-auth-required"})";
        if (unauthenticated_search_users_read_ec ||
            !json_matches(unauthenticated_search_users_reply, expected_unauthenticated_search_users_reply))
        {
            std::cerr << "FAIL unauthenticated search users rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated search users rejected\n";

        constexpr std::string_view unauthenticated_get_unread_count =
            R"({"jsonrpc":"2.0","method":"get_unread_count","params":{"conversation":1},"id":"unread-auth-required"})";
        auto [unauthenticated_get_unread_count_write_ec] = co_await send_websocket_text(socket, unauthenticated_get_unread_count);
        if (unauthenticated_get_unread_count_write_ec)
        {
            std::cerr << "FAIL unauthenticated get unread count write\n";
            co_return 1;
        }

        auto unauthenticated_get_unread_count_reply_result = co_await receive_websocket_text(socket);
        auto& [unauthenticated_get_unread_count_read_ec, unauthenticated_get_unread_count_reply] =
            unauthenticated_get_unread_count_reply_result;
        constexpr std::string_view expected_unauthenticated_get_unread_count_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"},"id":"unread-auth-required"})";
        if (unauthenticated_get_unread_count_read_ec ||
            !json_matches(unauthenticated_get_unread_count_reply, expected_unauthenticated_get_unread_count_reply))
        {
            std::cerr << "FAIL unauthenticated get unread count rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated get unread count rejected\n";

        constexpr std::string_view unauthenticated_mark_read =
            R"({"jsonrpc":"2.0","method":"mark_read","params":{"conversation":1,"message":1},"id":"mark-read-auth-required"})";
        auto [unauthenticated_mark_read_write_ec] = co_await send_websocket_text(socket, unauthenticated_mark_read);
        if (unauthenticated_mark_read_write_ec)
        {
            std::cerr << "FAIL unauthenticated mark read write\n";
            co_return 1;
        }

        auto unauthenticated_mark_read_reply_result = co_await receive_websocket_text(socket);
        auto& [unauthenticated_mark_read_read_ec, unauthenticated_mark_read_reply] = unauthenticated_mark_read_reply_result;
        constexpr std::string_view expected_unauthenticated_mark_read_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"},"id":"mark-read-auth-required"})";
        if (unauthenticated_mark_read_read_ec ||
            !json_matches(unauthenticated_mark_read_reply, expected_unauthenticated_mark_read_reply))
        {
            std::cerr << "FAIL unauthenticated mark read rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated mark read rejected\n";

        constexpr std::string_view invalid_authentication =
            R"({"jsonrpc":"2.0","method":"authenticate","params":{"username":"chat_server_test","password":""},"id":"auth-invalid"})";
        auto [invalid_authentication_write_ec] = co_await send_websocket_text(socket, invalid_authentication);
        if (invalid_authentication_write_ec)
        {
            std::cerr << "FAIL authentication invalid params write\n";
            co_return 1;
        }

        auto invalid_authentication_reply_result = co_await receive_websocket_text(socket);
        auto& [invalid_authentication_read_ec, invalid_authentication_reply] = invalid_authentication_reply_result;
        constexpr std::string_view expected_invalid_authentication_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32602,"message":"Invalid params"},"id":"auth-invalid"})";
        if (invalid_authentication_read_ec ||
            !json_matches(invalid_authentication_reply, expected_invalid_authentication_reply))
        {
            std::cerr << "FAIL authentication invalid params\n";
            co_return 1;
        }
        std::cout << "PASS authentication invalid params\n";

        constexpr std::string_view missing_user_authentication =
            R"({"jsonrpc":"2.0","method":"authenticate","params":{"username":"__chat_missing_user__","password":"test password"},"id":"auth-missing"})";
        auto [missing_user_authentication_write_ec] = co_await send_websocket_text(socket, missing_user_authentication);
        if (missing_user_authentication_write_ec)
        {
            std::cerr << "FAIL authentication missing user write\n";
            co_return 1;
        }

        auto missing_user_authentication_reply_result = co_await receive_websocket_text(socket);
        auto& [missing_user_authentication_read_ec, missing_user_authentication_reply] = missing_user_authentication_reply_result;
        constexpr std::string_view expected_failed_authentication_reply =
            R"({"jsonrpc":"2.0","result":{"authenticated":false},"id":"auth-missing"})";
        if (missing_user_authentication_read_ec ||
            !json_matches(missing_user_authentication_reply, expected_failed_authentication_reply))
        {
            std::cerr << "FAIL authentication missing user\n";
            co_return 1;
        }
        std::cout << "PASS authentication missing user\n";

        constexpr std::string_view wrong_password_authentication =
            R"({"jsonrpc":"2.0","method":"authenticate","params":{"username":"chat_server_test","password":"wrong password"},"id":"auth-wrong"})";
        auto [wrong_password_authentication_write_ec] = co_await send_websocket_text(socket, wrong_password_authentication);
        if (wrong_password_authentication_write_ec)
        {
            std::cerr << "FAIL authentication wrong password write\n";
            co_return 1;
        }

        auto wrong_password_authentication_reply_result = co_await receive_websocket_text(socket);
        auto& [wrong_password_authentication_read_ec, wrong_password_authentication_reply] = wrong_password_authentication_reply_result;
        constexpr std::string_view expected_wrong_password_authentication_reply =
            R"({"jsonrpc":"2.0","result":{"authenticated":false},"id":"auth-wrong"})";
        if (wrong_password_authentication_read_ec ||
            !json_matches(wrong_password_authentication_reply, expected_wrong_password_authentication_reply))
        {
            std::cerr << "FAIL authentication wrong password\n";
            co_return 1;
        }
        std::cout << "PASS authentication wrong password\n";

        constexpr std::string_view authentication_request =
            R"({"jsonrpc":"2.0","method":"authenticate","params":{"username":"chat_server_test","password":"test password"},"id":"auth-ok"})";
        auto [authentication_write_ec] = co_await send_websocket_text(socket, authentication_request);
        if (authentication_write_ec)
        {
            std::cerr << "FAIL authentication success write\n";
            co_return 1;
        }

        auto authentication_reply_result = co_await receive_websocket_text(socket);
        auto& [authentication_read_ec, authentication_reply] = authentication_reply_result;
        constexpr std::string_view expected_authentication_reply =
            R"({"jsonrpc":"2.0","result":{"authenticated":true},"id":"auth-ok"})";
        if (authentication_read_ec || !json_matches(authentication_reply, expected_authentication_reply))
        {
            std::cerr << "FAIL authentication success\n";
            co_return 1;
        }
        std::cout << "PASS authentication success\n";

        constexpr std::string_view repeated_authentication =
            R"({"jsonrpc":"2.0","method":"authenticate","params":{"username":"chat_server_test","password":"test password"},"id":"auth-repeat"})";
        auto [repeated_authentication_write_ec] = co_await send_websocket_text(socket, repeated_authentication);
        if (repeated_authentication_write_ec)
        {
            std::cerr << "FAIL repeated authentication write\n";
            co_return 1;
        }

        auto repeated_authentication_reply_result = co_await receive_websocket_text(socket);
        auto& [repeated_authentication_read_ec, repeated_authentication_reply] = repeated_authentication_reply_result;
        constexpr std::string_view expected_repeated_authentication_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32003,"message":"Already authenticated"},"id":"auth-repeat"})";
        if (repeated_authentication_read_ec ||
            !json_matches(repeated_authentication_reply, expected_repeated_authentication_reply))
        {
            std::cerr << "FAIL repeated authentication rejected\n";
            co_return 1;
        }
        std::cout << "PASS repeated authentication rejected\n";

        constexpr std::string_view invalid_search_users =
            R"({"jsonrpc":"2.0","method":"search_users","params":{"query":""},"id":"search-invalid"})";
        auto [invalid_search_users_write_ec] = co_await send_websocket_text(socket, invalid_search_users);
        if (invalid_search_users_write_ec)
        {
            std::cerr << "FAIL invalid search users write\n";
            co_return 1;
        }

        auto invalid_search_users_reply_result = co_await receive_websocket_text(socket);
        auto& [invalid_search_users_read_ec, invalid_search_users_reply] = invalid_search_users_reply_result;
        constexpr std::string_view expected_invalid_search_users_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32602,"message":"Invalid params"},"id":"search-invalid"})";
        if (invalid_search_users_read_ec ||
            !json_matches(invalid_search_users_reply, expected_invalid_search_users_reply))
        {
            std::cerr << "FAIL invalid search users rejected\n";
            co_return 1;
        }
        std::cout << "PASS invalid search users rejected\n";

        constexpr std::string_view invalid_get_messages =
            R"({"jsonrpc":"2.0","method":"get_messages","params":{"conversation":0},"id":"messages-invalid"})";
        auto [invalid_get_messages_write_ec] = co_await send_websocket_text(socket, invalid_get_messages);
        if (invalid_get_messages_write_ec)
        {
            std::cerr << "FAIL invalid get messages write\n";
            co_return 1;
        }

        auto invalid_get_messages_reply_result = co_await receive_websocket_text(socket);
        auto& [invalid_get_messages_read_ec, invalid_get_messages_reply] = invalid_get_messages_reply_result;
        constexpr std::string_view expected_invalid_get_messages_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32602,"message":"Invalid params"},"id":"messages-invalid"})";
        if (invalid_get_messages_read_ec ||
            !json_matches(invalid_get_messages_reply, expected_invalid_get_messages_reply))
        {
            std::cerr << "FAIL invalid get messages rejected\n";
            co_return 1;
        }
        std::cout << "PASS invalid get messages rejected\n";

        constexpr std::string_view invalid_get_unread_count =
            R"({"jsonrpc":"2.0","method":"get_unread_count","params":{"conversation":0},"id":"unread-invalid"})";
        auto [invalid_get_unread_count_write_ec] = co_await send_websocket_text(socket, invalid_get_unread_count);
        if (invalid_get_unread_count_write_ec)
        {
            std::cerr << "FAIL invalid get unread count write\n";
            co_return 1;
        }

        auto invalid_get_unread_count_reply_result = co_await receive_websocket_text(socket);
        auto& [invalid_get_unread_count_read_ec, invalid_get_unread_count_reply] = invalid_get_unread_count_reply_result;
        constexpr std::string_view expected_invalid_get_unread_count_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32602,"message":"Invalid params"},"id":"unread-invalid"})";
        if (invalid_get_unread_count_read_ec ||
            !json_matches(invalid_get_unread_count_reply, expected_invalid_get_unread_count_reply))
        {
            std::cerr << "FAIL invalid get unread count rejected\n";
            co_return 1;
        }
        std::cout << "PASS invalid get unread count rejected\n";

        constexpr std::string_view invalid_mark_read =
            R"({"jsonrpc":"2.0","method":"mark_read","params":{"conversation":0,"message":0},"id":"mark-read-invalid"})";
        auto [invalid_mark_read_write_ec] = co_await send_websocket_text(socket, invalid_mark_read);
        if (invalid_mark_read_write_ec)
        {
            std::cerr << "FAIL invalid mark read write\n";
            co_return 1;
        }

        auto invalid_mark_read_reply_result = co_await receive_websocket_text(socket);
        auto& [invalid_mark_read_read_ec, invalid_mark_read_reply] = invalid_mark_read_reply_result;
        constexpr std::string_view expected_invalid_mark_read_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32602,"message":"Invalid params"},"id":"mark-read-invalid"})";
        if (invalid_mark_read_read_ec || !json_matches(invalid_mark_read_reply, expected_invalid_mark_read_reply))
        {
            std::cerr << "FAIL invalid mark read rejected\n";
            co_return 1;
        }
        std::cout << "PASS invalid mark read rejected\n";

        constexpr std::string_view invalid_send_message =
            R"({"jsonrpc":"2.0","method":"send_message","params":{"conversation":0,"text":"hello"},"id":"send-invalid"})";
        auto [invalid_send_message_write_ec] = co_await send_websocket_text(socket, invalid_send_message);
        if (invalid_send_message_write_ec)
        {
            std::cerr << "FAIL invalid send message write\n";
            co_return 1;
        }

        auto invalid_send_message_reply_result = co_await receive_websocket_text(socket);
        auto& [invalid_send_message_read_ec, invalid_send_message_reply] = invalid_send_message_reply_result;
        constexpr std::string_view expected_invalid_send_message_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32602,"message":"Invalid params"},"id":"send-invalid"})";
        if (invalid_send_message_read_ec ||
            !json_matches(invalid_send_message_reply, expected_invalid_send_message_reply))
        {
            std::cerr << "FAIL invalid send message rejected\n";
            co_return 1;
        }
        std::cout << "PASS invalid send message rejected\n";

        constexpr std::string_view unavailable_send_message =
            R"({"jsonrpc":"2.0","method":"send_message","params":{"conversation":9223372036854775807,"text":"hello"},"id":"send-unavailable"})";
        auto [unavailable_send_message_write_ec] = co_await send_websocket_text(socket, unavailable_send_message);
        if (unavailable_send_message_write_ec)
        {
            std::cerr << "FAIL unavailable send message write\n";
            co_return 1;
        }

        auto unavailable_send_message_reply_result = co_await receive_websocket_text(socket);
        auto& [unavailable_send_message_read_ec, unavailable_send_message_reply] = unavailable_send_message_reply_result;
        constexpr std::string_view expected_unavailable_send_message_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32006,"message":"Conversation unavailable"},"id":"send-unavailable"})";
        if (unavailable_send_message_read_ec ||
            !json_matches(unavailable_send_message_reply, expected_unavailable_send_message_reply))
        {
            std::cerr << "FAIL unavailable send message rejected\n";
            co_return 1;
        }
        std::cout << "PASS unavailable send message rejected\n";

        auto self_conversation = co_await open_direct(socket, registered_user->front());
        if (self_conversation.empty())
        {
            std::cerr << "FAIL self conversation\n";
            co_return 1;
        }

        std::string self_send_message = R"({"jsonrpc":"2.0","method":"send_message","params":{"conversation":)";
        self_send_message.append(self_conversation);
        self_send_message.append(R"(,"text":"hello self"},"id":"send-self"})");
        auto [self_send_message_write_ec] = co_await send_websocket_text(socket, self_send_message);
        if (self_send_message_write_ec)
        {
            std::cerr << "FAIL self send message write\n";
            co_return 1;
        }

        auto self_send_message_reply_result = co_await receive_websocket_text(socket);
        auto& [self_send_message_read_ec, self_send_message_reply] = self_send_message_reply_result;
        if (self_send_message_read_ec)
        {
            std::cerr << "FAIL self send message response\n";
            co_return 1;
        }

        std::vector<std::string> self_message_parameters;
        self_message_parameters.push_back(registered_user->front());
        auto self_message_result = co_await fixture_connection.execute_row(
            "SELECT id::text, sender_id::text, sender_id::text, body, "
            "((extract(epoch from created_at) * 1000)::bigint)::text FROM messages "
            "WHERE sender_id = $1::bigint AND conversation_id=(SELECT id FROM conversations WHERE kind='direct' AND "
            "direct_user_low=$1::bigint AND direct_user_high=$1::bigint) "
            "ORDER BY id DESC LIMIT 1",
            std::move(self_message_parameters));
        auto& [self_message_ec, self_message_row] = self_message_result;
        if (self_message_ec || !self_message_row || self_message_row->size() != 5 ||
            self_message_row->at(1) != registered_user->front() || self_message_row->at(2) != registered_user->front() ||
            self_message_row->at(3) != "hello self")
        {
            std::cerr << "FAIL self message persistence: " << fixture_connection.error_message() << '\n';
            co_return 1;
        }
        std::cout << "PASS self message persistence\n";

        std::string expected_self_send_message_reply = R"({"jsonrpc":"2.0","result":{"message":)";
        expected_self_send_message_reply.append(self_message_row->at(0));
        expected_self_send_message_reply.append(R"(,"timestamp":)");
        expected_self_send_message_reply.append(self_message_row->at(4));
        expected_self_send_message_reply.append(R"(,"realtime":true},"id":"send-self"})");
        if (!json_matches(self_send_message_reply, expected_self_send_message_reply))
        {
            std::cerr << "FAIL self send message response\n";
            co_return 1;
        }
        std::cout << "PASS self send message response\n";

        auto self_message_notification_result = co_await receive_websocket_text(socket);
        auto& [self_message_notification_read_ec, self_message_notification] = self_message_notification_result;
        std::string expected_self_message_notification =
            R"({"jsonrpc":"2.0","method":"message","params":{"id":)";
        expected_self_message_notification.append(self_message_row->at(0));
        expected_self_message_notification.append(R"(,"from":)");
        expected_self_message_notification.append(registered_user->front());
        expected_self_message_notification.append(R"(,"timestamp":)");
        expected_self_message_notification.append(self_message_row->at(4));
        expected_self_message_notification.append(R"(,"text":"hello self"}})");
        if (self_message_notification_read_ec ||
            !json_matches(self_message_notification, expected_self_message_notification))
        {
            std::cerr << "FAIL self message notification\n";
            co_return 1;
        }
        std::cout << "PASS self message notification\n";

        std::string get_messages_request = R"({"jsonrpc":"2.0","method":"get_messages","params":{"conversation":)";
        get_messages_request.append(self_conversation);
        get_messages_request.append(R"(},"id":"messages-latest"})");
        auto [get_messages_write_ec] = co_await send_websocket_text(socket, get_messages_request);
        if (get_messages_write_ec)
        {
            std::cerr << "FAIL get messages write\n";
            co_return 1;
        }

        auto get_messages_reply_result = co_await receive_websocket_text(socket);
        auto& [get_messages_read_ec, get_messages_reply] = get_messages_reply_result;
        std::string expected_get_messages_reply = R"({"jsonrpc":"2.0","result":{"messages":[{"id":)";
        expected_get_messages_reply.append(self_message_row->at(0));
        expected_get_messages_reply.append(R"(,"from":)");
        expected_get_messages_reply.append(registered_user->front());
        expected_get_messages_reply.append(R"(,"timestamp":)");
        expected_get_messages_reply.append(self_message_row->at(4));
        expected_get_messages_reply.append(R"(,"text":"hello self"}],"has_more":false},"id":"messages-latest"})");
        if (get_messages_read_ec || !json_matches(get_messages_reply, expected_get_messages_reply))
        {
            std::cerr << "FAIL get messages latest\n";
            co_return 1;
        }
        std::cout << "PASS get messages latest\n";

        std::string get_messages_before_request =
            R"({"jsonrpc":"2.0","method":"get_messages","params":{"conversation":)";
        get_messages_before_request.append(self_conversation);
        get_messages_before_request.append(R"(,"before":)");
        get_messages_before_request.append(self_message_row->at(0));
        get_messages_before_request.append(R"(},"id":"messages-before"})");
        auto [get_messages_before_write_ec] = co_await send_websocket_text(socket, get_messages_before_request);
        if (get_messages_before_write_ec)
        {
            std::cerr << "FAIL get messages before write\n";
            co_return 1;
        }

        auto get_messages_before_reply_result = co_await receive_websocket_text(socket);
        auto& [get_messages_before_read_ec, get_messages_before_reply] = get_messages_before_reply_result;
        constexpr std::string_view expected_get_messages_before_reply =
            R"({"jsonrpc":"2.0","result":{"messages":[],"has_more":false},"id":"messages-before"})";
        if (get_messages_before_read_ec || !json_matches(get_messages_before_reply, expected_get_messages_before_reply))
        {
            std::cerr << "FAIL get messages before cursor\n";
            co_return 1;
        }
        std::cout << "PASS get messages before cursor\n";

        constexpr std::string_view echo_request = R"({"jsonrpc":"2.0","method":"echo","params":{"text":"hello chat"},"id":"1"})";
        auto [echo_write_ec] = co_await send_websocket_text(socket, echo_request);
        if (echo_write_ec)
        {
            std::cerr << "FAIL server JSON-RPC echo write\n";
            co_return 1;
        }

        auto echo_reply_result = co_await receive_websocket_text(socket);
        auto& [echo_read_ec, echo_reply] = echo_reply_result;
        constexpr std::string_view expected_echo_reply = R"({"jsonrpc":"2.0","result":{"text":"hello chat"},"id":"1"})";
        if (echo_read_ec || !json_matches(echo_reply, expected_echo_reply))
        {
            std::cerr << "FAIL server JSON-RPC result\n";
            co_return 1;
        }
        std::cout << "PASS server JSON-RPC notification\n";
        std::cout << "PASS server JSON-RPC result\n";

        constexpr std::string_view null_id_request = R"({"jsonrpc":"2.0","method":"echo","params":{"text":"null id"},"id":null})";
        auto [null_id_write_ec] = co_await send_websocket_text(socket, null_id_request);
        if (null_id_write_ec)
        {
            std::cerr << "FAIL server JSON-RPC null id write\n";
            co_return 1;
        }

        auto null_id_reply_result = co_await receive_websocket_text(socket);
        auto& [null_id_read_ec, null_id_reply] = null_id_reply_result;
        constexpr std::string_view expected_null_id_reply = R"({"jsonrpc":"2.0","result":{"text":"null id"},"id":null})";
        if (null_id_read_ec || !json_matches(null_id_reply, expected_null_id_reply))
        {
            std::cerr << "FAIL server JSON-RPC null id\n";
            co_return 1;
        }
        std::cout << "PASS server JSON-RPC null id\n";

        constexpr std::string_view missing_method_request = R"({"jsonrpc":"2.0","method":"missing","id":2})";
        auto [missing_method_write_ec] = co_await send_websocket_text(socket, missing_method_request);
        if (missing_method_write_ec)
        {
            std::cerr << "FAIL server JSON-RPC method-not-found write\n";
            co_return 1;
        }

        auto missing_method_reply_result = co_await receive_websocket_text(socket);
        auto& [missing_method_read_ec, missing_method_reply] = missing_method_reply_result;
        constexpr std::string_view expected_missing_method_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"Method not found"},"id":2})";
        if (missing_method_read_ec || !json_matches(missing_method_reply, expected_missing_method_reply))
        {
            std::cerr << "FAIL server JSON-RPC method not found\n";
            co_return 1;
        }
        std::cout << "PASS server JSON-RPC method not found\n";

        constexpr std::string_view invalid_params_request = R"({"jsonrpc":"2.0","method":"echo","params":{},"id":"3"})";
        auto [invalid_params_write_ec] = co_await send_websocket_text(socket, invalid_params_request);
        if (invalid_params_write_ec)
        {
            std::cerr << "FAIL server JSON-RPC invalid-params write\n";
            co_return 1;
        }

        auto invalid_params_reply_result = co_await receive_websocket_text(socket);
        auto& [invalid_params_read_ec, invalid_params_reply] = invalid_params_reply_result;
        constexpr std::string_view expected_invalid_params_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32602,"message":"Invalid params"},"id":"3"})";
        if (invalid_params_read_ec || !json_matches(invalid_params_reply, expected_invalid_params_reply))
        {
            std::cerr << "FAIL server JSON-RPC invalid params\n";
            co_return 1;
        }
        std::cout << "PASS server JSON-RPC invalid params\n";

        constexpr std::string_view malformed_request = R"({"jsonrpc":"2.0","method":"echo","params":)";
        auto [malformed_write_ec] = co_await send_websocket_text(socket, malformed_request);
        if (malformed_write_ec)
        {
            std::cerr << "FAIL server JSON-RPC malformed write\n";
            co_return 1;
        }

        auto malformed_reply_result = co_await receive_websocket_text(socket);
        auto& [malformed_read_ec, malformed_reply] = malformed_reply_result;
        constexpr std::string_view expected_malformed_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Parse error"},"id":null})";
        if (malformed_read_ec || !json_matches(malformed_reply, expected_malformed_reply))
        {
            std::cerr << "FAIL server JSON-RPC parse error\n";
            co_return 1;
        }
        std::cout << "PASS server JSON-RPC parse error\n";

        constexpr std::string_view invalid_request = R"({"jsonrpc":"2.0","method":"echo","params":"bad","id":"4"})";
        auto [invalid_request_write_ec] = co_await send_websocket_text(socket, invalid_request);
        if (invalid_request_write_ec)
        {
            std::cerr << "FAIL server JSON-RPC invalid-request write\n";
            co_return 1;
        }

        auto invalid_request_reply_result = co_await receive_websocket_text(socket);
        auto& [invalid_request_read_ec, invalid_request_reply] = invalid_request_reply_result;
        constexpr std::string_view expected_invalid_request_reply =
            R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Invalid Request"},"id":null})";
        if (invalid_request_read_ec || !json_matches(invalid_request_reply, expected_invalid_request_reply))
        {
            std::cerr << "FAIL server JSON-RPC invalid request\n";
            co_return 1;
        }
        std::cout << "PASS server JSON-RPC invalid request\n";

        constexpr std::array<std::uint8_t, 10> ping = {0x89, 0x84, 0x01, 0x02, 0x03, 0x04, 0x71, 0x6b, 0x6d, 0x63};
        auto [ping_ec, ping_written] = co_await boost::capy::write(socket, boost::capy::const_buffer(ping.data(), ping.size()));
        if (ping_ec || ping_written != ping.size())
        {
            std::cerr << "FAIL server WebSocket ping write\n";
            co_return 1;
        }

        std::array<std::uint8_t, 6> pong{};
        auto [pong_ec, pong_read] = co_await boost::capy::read(socket, boost::capy::mutable_buffer(pong.data(), pong.size()));
        constexpr std::array<std::uint8_t, 6> expected_pong = {0x8a, 0x04, 'p', 'i', 'n', 'g'};
        if (pong_ec || pong_read != pong.size() || pong != expected_pong)
        {
            std::cerr << "FAIL server WebSocket pong\n";
            co_return 1;
        }
        std::cout << "PASS server WebSocket ping/pong\n";

        constexpr std::array<std::uint8_t, 8> close = {0x88, 0x82, 0x12, 0x34, 0x56, 0x78, 0x11, 0xdc};
        auto [close_ec, close_written] = co_await boost::capy::write(socket, boost::capy::const_buffer(close.data(), close.size()));
        if (close_ec || close_written != close.size())
        {
            std::cerr << "FAIL server WebSocket close write\n";
            co_return 1;
        }

        std::array<std::uint8_t, 4> close_reply{};
        auto [reply_ec, reply_read] = co_await boost::capy::read(socket, boost::capy::mutable_buffer(close_reply.data(), close_reply.size()));
        constexpr std::array<std::uint8_t, 4> expected_close = {0x88, 0x02, 0x03, 0xe8};
        if (reply_ec || reply_read != close_reply.size() || close_reply != expected_close)
        {
            std::cerr << "FAIL server WebSocket close response\n";
            co_return 1;
        }

        socket.close();
        std::cout << "PASS server WebSocket close\n";
    }

    {
        boost::corosio::tcp_socket socket(io_context);
        auto [connect_ec] = co_await connect(socket, port);
        if (connect_ec)
        {
            std::cerr << "FAIL released authentication connect: " << connect_ec.message() << '\n';
            co_return 1;
        }

        auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
        boost::http::response_parser parser(parser_config);
        auto [upgrade_ec] = co_await upgrade_websocket(socket, parser);
        if (upgrade_ec)
        {
            std::cerr << "FAIL released authentication upgrade: " << upgrade_ec.message() << '\n';
            co_return 1;
        }

        constexpr std::string_view authentication_request =
            R"({"jsonrpc":"2.0","method":"authenticate","params":{"username":"chat_server_test","password":"test password"},"id":"auth-released"})";
        auto [authentication_write_ec] = co_await send_websocket_text(socket, authentication_request);
        if (authentication_write_ec)
        {
            std::cerr << "FAIL released authentication write\n";
            co_return 1;
        }

        auto authentication_reply_result = co_await receive_websocket_text(socket);
        auto& [authentication_read_ec, authentication_reply] = authentication_reply_result;
        constexpr std::string_view expected_authentication_reply =
            R"({"jsonrpc":"2.0","result":{"authenticated":true},"id":"auth-released"})";
        if (authentication_read_ec || !json_matches(authentication_reply, expected_authentication_reply))
        {
            std::cerr << "FAIL authentication session release\n";
            co_return 1;
        }

        socket.close();
        std::cout << "PASS authentication session release\n";
    }

    {
        boost::corosio::tcp_socket socket(io_context);
        auto [connect_ec] = co_await connect(socket, port);
        if (connect_ec)
        {
            std::cerr << "FAIL oversized WebSocket connect: " << connect_ec.message() << '\n';
            co_return 1;
        }

        auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
        boost::http::response_parser parser(parser_config);
        auto [upgrade_ec] = co_await upgrade_websocket(socket, parser);
        if (upgrade_ec)
        {
            std::cerr << "FAIL oversized WebSocket upgrade: " << upgrade_ec.message() << '\n';
            co_return 1;
        }

        std::string max_size_fragment(64 * 1024, 'x');
        auto first_fragment = make_masked_frame(0x01, false, max_size_fragment);
        auto [first_ec, first_written] =
            co_await boost::capy::write(socket, boost::capy::const_buffer(first_fragment.data(), first_fragment.size()));
        if (first_ec || first_written != first_fragment.size())
        {
            std::cerr << "FAIL oversized WebSocket first fragment write\n";
            co_return 1;
        }

        auto final_fragment = make_masked_frame(0x00, true, "x");
        auto [final_ec, final_written] =
            co_await boost::capy::write(socket, boost::capy::const_buffer(final_fragment.data(), final_fragment.size()));
        if (final_ec || final_written != final_fragment.size())
        {
            std::cerr << "FAIL oversized WebSocket final fragment write\n";
            co_return 1;
        }

        std::array<std::uint8_t, 4> close_reply{};
        auto [close_ec, close_read] = co_await boost::capy::read(socket, boost::capy::mutable_buffer(close_reply.data(), close_reply.size()));
        constexpr std::array<std::uint8_t, 4> expected_close = {0x88, 0x02, 0x03, 0xf1};
        if (close_ec || close_read != close_reply.size() || close_reply != expected_close)
        {
            std::cerr << "FAIL oversized WebSocket close response\n";
            co_return 1;
        }

        socket.close();
        std::cout << "PASS oversized WebSocket rejected\n";
    }

    {
        boost::corosio::tcp_socket socket(io_context);
        auto [connect_ec] = co_await connect(socket, port);
        if (connect_ec)
        {
            std::cerr << "FAIL reused worker connect: " << connect_ec.message() << '\n';
            co_return 1;
        }

        constexpr std::string_view request =
            "GET /health HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        auto [write_ec] = co_await send_request(socket, request);
        if (write_ec)
        {
            std::cerr << "FAIL reused worker request: " << write_ec.message() << '\n';
            co_return 1;
        }

        auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
        boost::http::response_parser parser(parser_config);
        parser.reset();
        parser.start();
        auto [read_ec] = co_await parser.read(socket);
        if (read_ec || !valid_health_response(parser))
        {
            std::cerr << "FAIL reused worker response\n";
            co_return 1;
        }

        socket.close();
        std::cout << "PASS server worker reuse\n";
    }

    {
        boost::corosio::tcp_socket socket(io_context);
        auto [connect_ec] = co_await connect(socket, port);
        if (connect_ec)
        {
            std::cerr << "FAIL active WebSocket connect: " << connect_ec.message() << '\n';
            co_return 1;
        }

        auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
        boost::http::response_parser parser(parser_config);
        auto [upgrade_ec] = co_await upgrade_websocket(socket, parser);
        if (upgrade_ec)
        {
            std::cerr << "FAIL active WebSocket upgrade: " << upgrade_ec.message() << '\n';
            co_return 1;
        }

        server.stop();

        std::array<std::uint8_t, 1> input{};
        auto [read_ec, size] = co_await socket.read_some(boost::capy::mutable_buffer(input.data(), input.size()));
        if (!read_ec && size != 0)
        {
            std::cerr << "FAIL active WebSocket remained open\n";
            co_return 1;
        }

        socket.close();
        std::cout << "PASS active WebSocket shutdown\n";
    }

    co_return 0;
}


boost::capy::task<int> run_peer_routing(boost::corosio::io_context& io_context,
                                        chat_server& server,
                                        unsigned short port,
                                        boost::capy::async_event& primary_done,
                                        int const& primary_exit_code)
{
    server_stop_guard stop_guard{server};

    auto primary_wait_result = co_await primary_done.wait();
    auto& [primary_wait_ec] = primary_wait_result;
    if (primary_wait_ec || primary_exit_code != 0)
    {
        co_return primary_wait_ec ? 1 : 0;
    }

    pg_connection fixture_connection(io_context);
    auto fixture_connect_result = co_await fixture_connection.connect(std::string(kDatabaseConnectionString));
    auto& [fixture_connect_ec] = fixture_connect_result;
    if (fixture_connect_ec)
    {
        std::cerr << "FAIL peer routing fixture connect: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    std::vector<std::string> cleanup_parameters;
    cleanup_parameters.emplace_back(kPeerUsername);
    auto cleanup_result = co_await fixture_connection.execute_row(
        "DELETE FROM users WHERE username = $1 RETURNING id::text", std::move(cleanup_parameters));
    auto& [cleanup_ec, cleanup_row] = cleanup_result;
    (void)cleanup_row;
    if (cleanup_ec)
    {
        std::cerr << "FAIL peer routing fixture cleanup: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    std::vector<std::string> source_parameters;
    source_parameters.emplace_back(kTestUsername);
    auto source_result = co_await fixture_connection.execute_row(
        "SELECT id::text, password_hash FROM users WHERE username = $1", std::move(source_parameters));
    auto& [source_ec, source_user] = source_result;
    if (source_ec || !source_user || source_user->size() != 2)
    {
        std::cerr << "FAIL peer routing source user: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    std::vector<std::string> peer_parameters;
    peer_parameters.emplace_back(kPeerUsername);
    peer_parameters.push_back(source_user->at(1));
    auto peer_result = co_await fixture_connection.execute_row(
        "INSERT INTO users (username, password_hash) VALUES ($1, $2) RETURNING id::text", std::move(peer_parameters));
    auto& [peer_ec, peer_user] = peer_result;
    if (peer_ec || !peer_user || peer_user->size() != 1)
    {
        std::cerr << "FAIL peer routing target user: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    auto const& source_user_id = source_user->front();
    auto const& peer_user_id = peer_user->front();

    auto peer_last_seen_result = co_await fixture_connection.execute_scalar(
        "SELECT ((extract(epoch from last_seen_at) * 1000)::bigint)::text FROM users WHERE id = $1::bigint",
        {peer_user_id});
    auto& [peer_last_seen_ec, peer_last_seen] = peer_last_seen_result;
    if (peer_last_seen_ec || peer_last_seen.empty())
    {
        std::cerr << "FAIL peer presence fixture: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    std::vector<std::string> contact_cleanup_parameters;
    contact_cleanup_parameters.push_back(source_user_id);
    auto contact_cleanup_result = co_await fixture_connection.execute_scalar(
        "WITH deleted AS (DELETE FROM contacts WHERE owner_id = $1::bigint RETURNING 1) "
        "SELECT count(*)::text FROM deleted",
        std::move(contact_cleanup_parameters));
    auto& [contact_cleanup_ec, contact_cleanup_count] = contact_cleanup_result;
    (void)contact_cleanup_count;
    if (contact_cleanup_ec)
    {
        std::cerr << "FAIL contacts fixture cleanup: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    boost::corosio::tcp_socket source_socket(io_context);
    auto source_connect_result = co_await connect(source_socket, port);
    auto& [source_connect_ec] = source_connect_result;
    if (source_connect_ec)
    {
        std::cerr << "FAIL peer routing source connect: " << source_connect_ec.message() << '\n';
        co_return 1;
    }

    auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
    boost::http::response_parser source_parser(parser_config);
    auto source_upgrade_result = co_await upgrade_websocket(source_socket, source_parser);
    auto& [source_upgrade_ec] = source_upgrade_result;
    if (source_upgrade_ec)
    {
        std::cerr << "FAIL peer routing source upgrade: " << source_upgrade_ec.message() << '\n';
        co_return 1;
    }

    auto source_auth_result = co_await authenticate_websocket(source_socket, kTestUsername, "peer-auth-source");
    auto& [source_auth_ec] = source_auth_result;
    if (source_auth_ec)
    {
        std::cerr << "FAIL peer routing source authentication\n";
        co_return 1;
    }

    auto direct_conversation = co_await open_direct(source_socket, peer_user_id);
    if (direct_conversation.empty())
    {
        std::cerr << "FAIL direct conversation\n";
        co_return 1;
    }

    auto peer_rpc = [&](std::string method, std::string params) -> boost::capy::task<std::string> {
        auto [write_ec] = co_await send_websocket_text(source_socket,
            "{\"jsonrpc\":\"2.0\",\"method\":\"" + method + "\",\"params\":" + params +
            ",\"id\":\"attachment-validation\"}");
        if (write_ec)
        {
            co_return std::string{};
        }
        auto reply_result = co_await receive_websocket_text(source_socket);
        auto& [read_ec, reply] = reply_result;
        if (read_ec)
        {
            co_return std::string{};
        }
        co_return std::move(reply);
    };
    auto attachment_begin = "{\"conversation\":" + direct_conversation + ",\"filename\":\"probe.bin\",\"size\":3}";
    auto avatar_begin_reply = co_await peer_rpc("begin_avatar_upload", "{\"size\":3}");
    auto avatar_begin_value = boost::json::parse(avatar_begin_reply);
    if (!avatar_begin_value.as_object().contains("result"))
    {
        std::cerr << "FAIL avatar upload begin: " << avatar_begin_reply << '\n';
        co_return 1;
    }
    auto avatar_id = avatar_begin_value.as_object().at("result").as_object().at("upload").as_int64();
    auto avatar_params = "{\"upload\":" + std::to_string(avatar_id);
    auto avatar_cases = std::vector<std::tuple<std::string, std::string, int>>{
        {"begin_avatar_upload", "{\"size\":1048577}", -32602},
        {"begin_avatar_upload", "{\"size\":0}", -32602},
        {"begin_avatar_upload", "{\"size\":1,\"user\":2}", -32602},
        {"begin_avatar_upload", "{\"size\":1}", -32011},
        {"finish_avatar_upload", avatar_params + "}", -32011},
        {"upload_avatar_chunk", avatar_params + ",\"offset\":1,\"data\":\"eA==\"}", -32602},
        {"upload_avatar_chunk", avatar_params + ",\"offset\":0,\"data\":\"!bad\"}", -32602},
        {"upload_avatar_chunk", avatar_params + ",\"offset\":0,\"data\":\"YWJjZA==\"}", -32602},
        {"upload_avatar_chunk", "{\"upload\":2,\"offset\":0,\"data\":\"eA==\"}", -32011},
        {"clear_avatar", "{\"user\":2}", -32602},
        {"get_avatar", "{\"user\":1,\"revision\":-1,\"offset\":0}", -32602}};
    for (auto const& [method, params, code] : avatar_cases)
    {
        auto rejected = co_await peer_rpc(method, params);
        auto value = boost::json::parse(rejected);
        if (!value.as_object().contains("error") || value.at("error").at("code").as_int64() != code)
        {
            std::cerr << "FAIL avatar validation: " << rejected << '\n';
            co_return 1;
        }
    }
    auto avatar_chunk_reply = co_await peer_rpc("upload_avatar_chunk", avatar_params + ",\"offset\":0,\"data\":\"YWJj\"}");
    if (!boost::json::parse(avatar_chunk_reply).as_object().contains("result")) { co_return 1; }
    auto invalid_avatar = co_await peer_rpc("finish_avatar_upload", avatar_params + "}");
    if (boost::json::parse(invalid_avatar).at("error").at("code").as_int64() != -32602) { co_return 1; }
    auto cancelled_avatar = co_await peer_rpc("cancel_avatar_upload", avatar_params + "}");
    if (boost::json::parse(cancelled_avatar).at("result").at("cancelled").as_bool()) { co_return 1; }
    avatar_begin_reply = co_await peer_rpc("begin_avatar_upload", "{\"size\":3}");
    avatar_id = boost::json::parse(avatar_begin_reply).at("result").at("upload").as_int64();
    avatar_params = "{\"upload\":" + std::to_string(avatar_id);
    cancelled_avatar = co_await peer_rpc("cancel_avatar_upload", avatar_params + "}");
    if (!boost::json::parse(cancelled_avatar).at("result").at("cancelled").as_bool()) { co_return 1; }
    avatar_begin_reply = co_await peer_rpc("begin_avatar_upload", "{\"size\":3}");
    avatar_id = boost::json::parse(avatar_begin_reply).at("result").at("upload").as_int64();
    auto attachment_begin_reply = co_await peer_rpc("begin_attachment", attachment_begin);
    boost::system::error_code attachment_parse_ec;
    auto attachment_value = boost::json::parse(attachment_begin_reply, attachment_parse_ec);
    if (attachment_parse_ec || !attachment_value.is_object() || !attachment_value.as_object().contains("result"))
    {
        std::cerr << "FAIL attachment begin: " << attachment_begin_reply << '\n';
        co_return 1;
    }
    auto upload_id = std::to_string(attachment_value.at("result").at("upload").as_int64());
    struct attachment_case { std::string method; std::string params; std::string expected; };
    std::vector<attachment_case> attachment_cases{
        {"begin_attachment", attachment_begin, R"({"error":{"code":-32008}})"},
        {"finish_attachment", "{\"upload\":" + upload_id + "}", R"({"error":{"code":-32008}})"},
        {"upload_attachment", "{\"upload\":" + upload_id + ",\"offset\":0,\"data\":\"AA=A\"}", R"({"error":{"code":-32602}})"},
        {"upload_attachment", "{\"upload\":" + upload_id + ",\"offset\":1,\"data\":\"AAEC\"}", R"({"error":{"code":-32602}})"},
        {"upload_attachment", "{\"upload\":" + upload_id + ",\"offset\":0,\"data\":\"AAECAw==\"}", R"({"error":{"code":-32602}})"},
        {"cancel_attachment", "{\"upload\":" + std::to_string(std::stoll(upload_id) + 1) + "}", R"({"result":{"cancelled":false}})"},
        {"upload_attachment", "{\"upload\":" + upload_id + ",\"offset\":0,\"data\":\"AAEC\"}", R"({"result":{"offset":3}})"},
        {"upload_attachment", "{\"upload\":" + upload_id + ",\"offset\":0,\"data\":\"AAEC\"}", R"({"error":{"code":-32602}})"},
        {"cancel_attachment", "{\"upload\":" + upload_id + "}", R"({"result":{"cancelled":true}})"},
        {"cancel_attachment", "{\"upload\":" + upload_id + "}", R"({"result":{"cancelled":false}})"},
        {"begin_attachment", "{\"conversation\":" + direct_conversation + ",\"filename\":\"../bad\",\"size\":3}", R"({"error":{"code":-32602}})"},
        {"begin_attachment", "{\"conversation\":" + direct_conversation + ",\"filename\":\"big\",\"size\":10485761}", R"({"error":{"code":-32602}})"}
    };
    for (auto const& test : attachment_cases)
    {
        auto reply = co_await peer_rpc(test.method, test.params);
        if (!json_matches(reply, test.expected))
        {
            std::cerr << "FAIL attachment validation " << test.method << ": " << reply << '\n';
            co_return 1;
        }
    }
    std::cout << "PASS attachment upload validation and cancellation\n";
    auto interrupted_reply = co_await peer_rpc("begin_attachment", attachment_begin);
    auto interrupted_upload = boost::json::parse(interrupted_reply).at("result").at("upload").as_int64();
    auto interrupted_chunk = co_await peer_rpc("upload_attachment", "{\"upload\":" + std::to_string(interrupted_upload) +
        ",\"offset\":0,\"data\":\"AAEC\"}");
    if (!json_matches(interrupted_chunk, R"({"result":{"offset":3}})"))
    {
        std::cerr << "FAIL interrupted upload fixture\n";
        co_return 1;
    }
    source_socket.close();
    auto [reconnect_ec] = co_await connect(source_socket, port);
    auto [reupgrade_ec] = co_await upgrade_websocket(source_socket, source_parser);
    auto [reauth_ec] = co_await authenticate_websocket(source_socket, kTestUsername, "upload-reconnect");
    if (reconnect_ec || reupgrade_ec || reauth_ec)
    {
        std::cerr << "FAIL upload reconnect\n";
        co_return 1;
    }
    auto stale_avatar_finish = co_await peer_rpc("finish_avatar_upload", "{\"upload\":" + std::to_string(avatar_id) + "}");
    auto stale_avatar_chunk = co_await peer_rpc("upload_avatar_chunk", "{\"upload\":" + std::to_string(avatar_id) +
        ",\"offset\":0,\"data\":\"YWJj\"}");
    if (!json_matches(stale_avatar_finish, R"({"error":{"code":-32011}})") ||
        !json_matches(stale_avatar_chunk, R"({"error":{"code":-32011}})"))
    {
        std::cerr << "FAIL reconnect retained avatar upload state\n";
        co_return 1;
    }
    std::cout << "PASS avatar invalid input, cancellation, attachment isolation and reconnect cleanup\n";
    auto interrupted_finish = co_await peer_rpc("finish_attachment", "{\"upload\":" + std::to_string(interrupted_upload) + "}");
    auto fresh_upload = co_await peer_rpc("begin_attachment", attachment_begin);
    if (!json_matches(interrupted_finish, R"({"error":{"code":-32008}})") ||
        !json_matches(fresh_upload, R"({"result":{"upload":1}})"))
    {
        std::cerr << "FAIL disconnected upload retained state\n";
        co_return 1;
    }
    co_await peer_rpc("cancel_attachment", "{\"upload\":1}");
    auto [group_ec, lifecycle_group] = co_await fixture_connection.execute_scalar(
        "WITH c AS (INSERT INTO conversations(kind,title,owner_id) VALUES('group','upload lifecycle',$1::bigint) RETURNING id), "
        "members AS (INSERT INTO conversation_members(conversation_id,user_id) SELECT id,$1::bigint FROM c "
        "UNION ALL SELECT id,$2::bigint FROM c) SELECT id::text FROM c", {peer_user_id, source_user_id});
    if (group_ec || lifecycle_group.empty())
    {
        std::cerr << "FAIL attachment leave fixture\n";
        co_return 1;
    }
    auto group_upload_reply = co_await peer_rpc("begin_attachment", "{\"conversation\":" + lifecycle_group +
        ",\"filename\":\"leave.bin\",\"size\":3}");
    auto group_upload = boost::json::parse(group_upload_reply).at("result").at("upload").as_int64();
    co_await peer_rpc("upload_attachment", "{\"upload\":" + std::to_string(group_upload) +
        ",\"offset\":0,\"data\":\"AAEC\"}");
    auto left_group = co_await peer_rpc("leave_group", "{\"conversation\":" + lifecycle_group + "}");
    auto leave_notification_result = co_await receive_websocket_text(source_socket);
    auto& [leave_notification_ec, leave_notification] = leave_notification_result;
    auto left_finish = co_await peer_rpc("finish_attachment", "{\"upload\":" + std::to_string(group_upload) + "}");
    auto after_leave_upload = co_await peer_rpc("begin_attachment", attachment_begin);
    if (!json_matches(left_group, R"({"result":{"changed":true}})") || leave_notification_ec ||
        !json_matches(leave_notification, R"({"method":"conversation"})") ||
        !json_matches(left_finish, R"({"error":{"code":-32008}})") ||
        !json_matches(after_leave_upload, R"({"result":{"upload":3}})"))
    {
        std::cerr << "FAIL leaving group retained upload state\n";
        co_return 1;
    }
    co_await peer_rpc("cancel_attachment", "{\"upload\":3}");
    auto [cleanup_group_ec, cleanup_group_row] = co_await fixture_connection.execute_row(
        "DELETE FROM conversations WHERE id=$1::bigint", {lifecycle_group});
    if (cleanup_group_ec)
    {
        co_return 1;
    }
    std::cout << "PASS attachment interruption, reconnect and group leave cleanup\n";
    {
        auto [create_ec, removal_group] = co_await fixture_connection.execute_scalar(
            "WITH c AS (INSERT INTO conversations(kind,title,owner_id) VALUES('group','removal upload',$1::bigint) RETURNING id), "
            "members AS (INSERT INTO conversation_members(conversation_id,user_id) SELECT id,$1::bigint FROM c "
            "UNION ALL SELECT id,$2::bigint FROM c) SELECT id::text FROM c", {peer_user_id, source_user_id});
        boost::corosio::tcp_socket owner_socket(io_context);
        boost::http::response_parser owner_parser(parser_config);
        auto [owner_connect_ec] = co_await connect(owner_socket, port);
        auto [owner_upgrade_ec] = co_await upgrade_websocket(owner_socket, owner_parser);
        auto [owner_auth_ec] = co_await authenticate_websocket(owner_socket, kPeerUsername, "remove-owner-auth");
        if (create_ec || owner_connect_ec || owner_upgrade_ec || owner_auth_ec)
        {
            co_return 1;
        }
        boost::capy::io_result<std::string> removed_result;
        boost::capy::io_result<std::string> notice_result;
        for (bool finishing : {false, true})
        {
            auto begin = co_await peer_rpc("begin_attachment", "{\"conversation\":" + removal_group +
                ",\"filename\":\"race.bin\",\"size\":3}");
            auto upload = boost::json::parse(begin).at("result").at("upload").as_int64();
            co_await peer_rpc("upload_attachment", "{\"upload\":" + std::to_string(upload) +
                ",\"offset\":0,\"data\":\"AAEC\"}");
            if (finishing)
            {
                co_await fixture_connection.execute_row("BEGIN");
                co_await fixture_connection.execute_row("SELECT id FROM conversations WHERE id=$1::bigint FOR UPDATE", {removal_group});
            }
            auto [remove_write_ec] = co_await send_websocket_text(owner_socket,
                "{\"jsonrpc\":\"2.0\",\"method\":\"remove_group_member\",\"params\":{\"conversation\":" + removal_group +
                ",\"user\":" + source_user_id + "},\"id\":\"remove-upload\"}");
            if (remove_write_ec) { co_return 1; }
            if (finishing)
            {
                for (int contenders : {1, 2})
                {
                    bool waiting = false;
                    for (int i = 0; i < 100 && !waiting; ++i)
                    {
                        auto [wait_ec, count] = co_await fixture_connection.execute_scalar(
                            "SELECT (count(*)>=$1::int)::text FROM pg_stat_activity WHERE datname=current_database() "
                            "AND wait_event_type='Lock' AND cardinality(pg_blocking_pids(pid))>0", {std::to_string(contenders)});
                        waiting = !wait_ec && count == "true";
                    }
                    if (!waiting)
                    {
                        co_await fixture_connection.execute_row("ROLLBACK");
                        std::cerr << "FAIL removal/finish lock contenders\n";
                        co_return 1;
                    }
                    if (contenders == 1)
                    {
                        auto [finish_write_ec] = co_await send_websocket_text(source_socket,
                            "{\"jsonrpc\":\"2.0\",\"method\":\"finish_attachment\",\"params\":{\"upload\":" +
                            std::to_string(upload) + "},\"id\":\"finish-race\"}");
                        if (finish_write_ec) { co_return 1; }
                    }
                }
                co_await fixture_connection.execute_row("COMMIT");
            }
            removed_result = co_await receive_websocket_text(owner_socket);
            auto& [removed_ec, removed_reply] = removed_result;
            if (removed_ec || !json_matches(removed_reply, R"({"result":{"changed":true}})")) { co_return 1; }
            if (finishing)
            {
                auto finish_result = co_await receive_websocket_text(source_socket);
                auto& [finish_ec, finish_reply] = finish_result;
                if (finish_ec || !json_matches(finish_reply, R"({"id":"finish-race","error":{"code":-32006}})"))
                {
                    std::cerr << "FAIL removed suspended attachment finish: " << finish_reply << '\n';
                    co_return 1;
                }
            }
            notice_result = co_await receive_websocket_text(source_socket);
            auto& [notice_ec, notice] = notice_result;
            auto chunk = co_await peer_rpc("upload_attachment", "{\"upload\":" + std::to_string(upload) +
                ",\"offset\":0,\"data\":\"AAEC\"}");
            auto unread = co_await peer_rpc("get_unread_count", "{\"conversation\":" + removal_group + "}");
            if (notice_ec || !json_matches(notice, R"({"method":"conversation","params":{"removed":true}})") ||
                !json_matches(chunk, R"({"error":{"code":-32008}})") ||
                !json_matches(unread, R"({"error":{"code":-32006}})"))
            {
                std::cerr << "FAIL removed upload cleanup\n";
                co_return 1;
            }
            if (!finishing)
            {
                co_await fixture_connection.execute_row(
                    "INSERT INTO conversation_members(conversation_id,user_id) VALUES($1::bigint,$2::bigint)",
                    {removal_group, source_user_id});
            }
        }
        auto [files_ec, files] = co_await fixture_connection.execute_scalar(
            "SELECT count(*)::text FROM messages WHERE conversation_id=$1::bigint", {removal_group});
        if (files_ec || files != "0") { co_return 1; }
        co_await fixture_connection.execute_row("DELETE FROM conversations WHERE id=$1::bigint", {removal_group});
        owner_socket.close();
        bool owner_closed = false;
        for (int i = 0; i < 100 && !owner_closed; ++i)
        {
            auto [seen_ec, seen] = co_await fixture_connection.execute_scalar(
                "SELECT ((extract(epoch FROM last_seen_at)*1000)::bigint)::text FROM users WHERE id=$1::bigint",
                {peer_user_id});
            owner_closed = !seen_ec && seen != peer_last_seen;
            if (owner_closed) { peer_last_seen = std::move(seen); }
        }
        if (!owner_closed) { co_return 1; }
        std::cout << "PASS removed partial upload and suspended finish race\n";
    }
    for (auto const& params : std::vector<std::string>{
             "{\"conversation\":" + direct_conversation + "}",
             "{\"conversation\":" + direct_conversation + ",\"typing\":1}",
             "{\"conversation\":" + direct_conversation + ",\"typing\":true,\"user\":1}",
             "{\"conversation\":0,\"typing\":true}"})
    {
        auto reply = co_await peer_rpc("set_typing", params);
        if (!json_matches(reply, R"({"error":{"code":-32602}})"))
        {
            std::cerr << "FAIL invalid typing params: " << reply << '\n';
            co_return 1;
        }
    }
    auto offline_typing_reply = co_await peer_rpc("set_typing",
        "{\"conversation\":" + direct_conversation + ",\"typing\":true}");
    if (!json_matches(offline_typing_reply, R"({"result":{"realtime":false}})"))
    {
        std::cerr << "FAIL offline typing response: " << offline_typing_reply << '\n';
        co_return 1;
    }
    std::cout << "PASS typing strict params and offline response\n";

    constexpr std::string_view empty_contacts_request =
        R"({"jsonrpc":"2.0","method":"get_contacts","id":"contacts-empty"})";
    auto empty_contacts_write_result = co_await send_websocket_text(source_socket, empty_contacts_request);
    auto& [empty_contacts_write_ec] = empty_contacts_write_result;
    if (empty_contacts_write_ec)
    {
        std::cerr << "FAIL empty contacts write\n";
        co_return 1;
    }

    auto empty_contacts_reply_result = co_await receive_websocket_text(source_socket);
    auto& [empty_contacts_read_ec, empty_contacts_reply] = empty_contacts_reply_result;
    constexpr std::string_view expected_empty_contacts_reply =
        R"({"jsonrpc":"2.0","result":{"users":[]},"id":"contacts-empty"})";
    if (empty_contacts_read_ec || !json_matches(empty_contacts_reply, expected_empty_contacts_reply))
    {
        std::cerr << "FAIL empty contacts\n";
        co_return 1;
    }
    std::cout << "PASS empty contacts\n";

    constexpr std::string_view search_users_request =
        R"({"jsonrpc":"2.0","method":"search_users","params":{"query":"chat_server_p"},"id":"peer-search"})";
    auto search_users_write_result = co_await send_websocket_text(source_socket, search_users_request);
    auto& [search_users_write_ec] = search_users_write_result;
    if (search_users_write_ec)
    {
        std::cerr << "FAIL user search write\n";
        co_return 1;
    }

    auto search_users_reply_result = co_await receive_websocket_text(source_socket);
    auto& [search_users_read_ec, search_users_reply] = search_users_reply_result;
    std::string expected_search_users_reply = R"({"jsonrpc":"2.0","result":{"users":[{"id":)";
    expected_search_users_reply.append(peer_user_id);
    expected_search_users_reply.append(R"(,"username":"chat_server_peer"}]},"id":"peer-search"})");
    if (search_users_read_ec || !json_matches(search_users_reply, expected_search_users_reply))
    {
        std::cerr << "FAIL user search\n";
        co_return 1;
    }
    std::cout << "PASS user search\n";

    constexpr std::string_view self_search_request =
        R"({"jsonrpc":"2.0","method":"search_users","params":{"query":"chat_server_test"},"id":"self-search"})";
    auto self_search_write_result = co_await send_websocket_text(source_socket, self_search_request);
    auto& [self_search_write_ec] = self_search_write_result;
    if (self_search_write_ec)
    {
        std::cerr << "FAIL self user search write\n";
        co_return 1;
    }

    auto self_search_reply_result = co_await receive_websocket_text(source_socket);
    auto& [self_search_read_ec, self_search_reply] = self_search_reply_result;
    constexpr std::string_view expected_self_search_reply =
        R"({"jsonrpc":"2.0","result":{"users":[]},"id":"self-search"})";
    if (self_search_read_ec || !json_matches(self_search_reply, expected_self_search_reply))
    {
        std::cerr << "FAIL self user search exclusion\n";
        co_return 1;
    }
    std::cout << "PASS self user search exclusion\n";

    std::string add_contact_request = R"({"jsonrpc":"2.0","method":"add_contact","params":{"user":)";
    add_contact_request.append(peer_user_id);
    add_contact_request.append(R"(},"id":"contact-add"})");
    auto add_contact_write_result = co_await send_websocket_text(source_socket, add_contact_request);
    auto& [add_contact_write_ec] = add_contact_write_result;
    if (add_contact_write_ec)
    {
        std::cerr << "FAIL add contact write\n";
        co_return 1;
    }

    auto add_contact_reply_result = co_await receive_websocket_text(source_socket);
    auto& [add_contact_read_ec, add_contact_reply] = add_contact_reply_result;
    std::string expected_add_contact_reply = R"({"jsonrpc":"2.0","result":{"user":{"id":)";
    expected_add_contact_reply.append(peer_user_id);
    expected_add_contact_reply.append(R"(,"username":"chat_server_peer"}},"id":"contact-add"})");
    if (add_contact_read_ec || !json_matches(add_contact_reply, expected_add_contact_reply))
    {
        std::cerr << "FAIL add contact\n";
        co_return 1;
    }
    std::cout << "PASS add contact\n";

    constexpr std::string_view contacts_request =
        R"({"jsonrpc":"2.0","method":"get_contacts","id":"contacts-list"})";
    auto contacts_write_result = co_await send_websocket_text(source_socket, contacts_request);
    auto& [contacts_write_ec] = contacts_write_result;
    if (contacts_write_ec)
    {
        std::cerr << "FAIL contacts list write\n";
        co_return 1;
    }

    auto contacts_reply_result = co_await receive_websocket_text(source_socket);
    auto& [contacts_read_ec, contacts_reply] = contacts_reply_result;
    std::string expected_contacts_reply = R"({"jsonrpc":"2.0","result":{"users":[{"id":)";
    expected_contacts_reply.append(peer_user_id);
    expected_contacts_reply.append(R"(,"username":"chat_server_peer"}]},"id":"contacts-list"})");
    if (contacts_read_ec || !json_matches(contacts_reply, expected_contacts_reply))
    {
        std::cerr << "FAIL contacts list\n";
        co_return 1;
    }
    std::cout << "PASS contacts list\n";

    constexpr std::string_view existing_contact_search =
        R"({"jsonrpc":"2.0","method":"search_users","params":{"query":"chat_server_p"},"id":"contact-search-existing"})";
    auto existing_contact_search_write_result = co_await send_websocket_text(source_socket, existing_contact_search);
    auto& [existing_contact_search_write_ec] = existing_contact_search_write_result;
    if (existing_contact_search_write_ec)
    {
        std::cerr << "FAIL existing contact search write\n";
        co_return 1;
    }

    auto existing_contact_search_reply_result = co_await receive_websocket_text(source_socket);
    auto& [existing_contact_search_read_ec, existing_contact_search_reply] = existing_contact_search_reply_result;
    constexpr std::string_view expected_existing_contact_search_reply =
        R"({"jsonrpc":"2.0","result":{"users":[]},"id":"contact-search-existing"})";
    if (existing_contact_search_read_ec ||
        !json_matches(existing_contact_search_reply, expected_existing_contact_search_reply))
    {
        std::cerr << "FAIL existing contact search exclusion\n";
        co_return 1;
    }
    std::cout << "PASS existing contact search exclusion\n";

    std::string offline_send_request = R"({"jsonrpc":"2.0","method":"send_message","params":{"conversation":)";
    offline_send_request.append(direct_conversation);
    offline_send_request.append(R"(,"text":"offline hello"},"id":"peer-offline"})");
    auto offline_send_write_result = co_await send_websocket_text(source_socket, offline_send_request);
    auto& [offline_send_write_ec] = offline_send_write_result;
    if (offline_send_write_ec)
    {
        std::cerr << "FAIL offline message write\n";
        co_return 1;
    }

    auto offline_send_reply_result = co_await receive_websocket_text(source_socket);
    auto& [offline_send_read_ec, offline_send_reply] = offline_send_reply_result;
    if (offline_send_read_ec)
    {
        std::cerr << "FAIL offline message response\n";
        co_return 1;
    }

    std::vector<std::string> offline_message_parameters;
    offline_message_parameters.push_back(source_user_id);
    offline_message_parameters.push_back(peer_user_id);
    auto offline_message_result = co_await fixture_connection.execute_row(
        "SELECT id::text, body, ((extract(epoch from created_at) * 1000)::bigint)::text FROM messages "
        "WHERE sender_id = $1::bigint AND conversation_id=(SELECT id FROM conversations WHERE kind='direct' AND "
        "direct_user_low=least($1::bigint,$2::bigint) AND direct_user_high=greatest($1::bigint,$2::bigint)) "
        "ORDER BY id DESC LIMIT 1",
        std::move(offline_message_parameters));
    auto& [offline_message_ec, offline_message] = offline_message_result;
    if (offline_message_ec || !offline_message || offline_message->size() != 3 || offline_message->at(1) != "offline hello")
    {
        std::cerr << "FAIL offline message persistence: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    std::string expected_offline_send_reply = R"({"jsonrpc":"2.0","result":{"message":)";
    expected_offline_send_reply.append(offline_message->at(0));
    expected_offline_send_reply.append(R"(,"timestamp":)");
    expected_offline_send_reply.append(offline_message->at(2));
    expected_offline_send_reply.append(R"(,"realtime":false},"id":"peer-offline"})");
    if (!json_matches(offline_send_reply, expected_offline_send_reply))
    {
        std::cerr << "FAIL offline message response\n";
        co_return 1;
    }
    std::cout << "PASS offline message response\n";

    boost::corosio::tcp_socket peer_socket(io_context);
    auto peer_connect_result = co_await connect(peer_socket, port);
    auto& [peer_connect_ec] = peer_connect_result;
    if (peer_connect_ec)
    {
        std::cerr << "FAIL peer routing target connect: " << peer_connect_ec.message() << '\n';
        co_return 1;
    }

    boost::http::response_parser peer_parser(parser_config);
    auto peer_upgrade_result = co_await upgrade_websocket(peer_socket, peer_parser);
    auto& [peer_upgrade_ec] = peer_upgrade_result;
    if (peer_upgrade_ec)
    {
        std::cerr << "FAIL peer routing target upgrade: " << peer_upgrade_ec.message() << '\n';
        co_return 1;
    }

    auto peer_auth_result = co_await authenticate_websocket(peer_socket, kPeerUsername, "peer-auth-target");
    auto& [peer_auth_ec] = peer_auth_result;
    if (peer_auth_ec)
    {
        std::cerr << "FAIL peer routing target authentication\n";
        co_return 1;
    }

    auto online_presence_result = co_await receive_websocket_text(source_socket);
    auto& [online_presence_ec, online_presence] = online_presence_result;
    std::string expected_online_presence = R"({"jsonrpc":"2.0","method":"presence","params":{"user":)";
    expected_online_presence.append(peer_user_id);
    expected_online_presence.append(R"(,"online":true,"last_seen":0}})");
    if (online_presence_ec || online_presence != expected_online_presence)
    {
        std::cerr << "FAIL online presence notification\n";
        co_return 1;
    }
    std::cout << "PASS online presence notification\n";

    constexpr std::string_view online_presence_request =
        R"({"jsonrpc":"2.0","method":"get_presence","id":"source-presence-online"})";
    auto online_presence_write_result = co_await send_websocket_text(source_socket, online_presence_request);
    auto& [online_presence_write_ec] = online_presence_write_result;
    if (online_presence_write_ec)
    {
        std::cerr << "FAIL online presence query write\n";
        co_return 1;
    }

    auto online_presence_reply_result = co_await receive_websocket_text(source_socket);
    auto& [online_presence_reply_ec, online_presence_reply] = online_presence_reply_result;
    std::string expected_online_presence_reply = R"({"jsonrpc":"2.0","result":{"users":[{"user":)";
    expected_online_presence_reply.append(peer_user_id);
    expected_online_presence_reply.append(R"(,"online":true,"last_seen":)");
    expected_online_presence_reply.append(peer_last_seen);
    expected_online_presence_reply.append(R"(}]},"id":"source-presence-online"})");
    if (online_presence_reply_ec || !json_matches(online_presence_reply, expected_online_presence_reply))
    {
        std::cerr << "FAIL online presence query\n";
        co_return 1;
    }
    std::cout << "PASS online presence query\n";

    constexpr std::string_view conversations_unread_request =
        R"({"jsonrpc":"2.0","method":"get_conversations","id":"peer-conversations-unread"})";
    auto conversations_unread_write_result = co_await send_websocket_text(peer_socket, conversations_unread_request);
    auto& [conversations_unread_write_ec] = conversations_unread_write_result;
    if (conversations_unread_write_ec)
    {
        std::cerr << "FAIL conversation unread summary write\n";
        co_return 1;
    }

    auto conversations_unread_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [conversations_unread_read_ec, conversations_unread_reply] = conversations_unread_reply_result;
    std::string expected_conversations_unread_reply =
        R"({"jsonrpc":"2.0","result":{"conversations":[{"user":)";
    expected_conversations_unread_reply.append(source_user_id);
    expected_conversations_unread_reply.append(R"(,"username":"chat_server_test","last":{"id":)");
    expected_conversations_unread_reply.append(offline_message->at(0));
    expected_conversations_unread_reply.append(R"(,"from":)");
    expected_conversations_unread_reply.append(source_user_id);
    expected_conversations_unread_reply.append(R"(,"timestamp":)");
    expected_conversations_unread_reply.append(offline_message->at(2));
    expected_conversations_unread_reply.append(
        R"(,"text":"offline hello"},"unread":1}]},"id":"peer-conversations-unread"})");
    if (conversations_unread_read_ec || !json_matches(conversations_unread_reply, expected_conversations_unread_reply))
    {
        std::cerr << "FAIL conversation unread summary\n";
        co_return 1;
    }
    std::cout << "PASS conversation unread summary\n";

    std::string offline_history_request = R"({"jsonrpc":"2.0","method":"get_messages","params":{"conversation":)";
    offline_history_request.append(direct_conversation);
    offline_history_request.append(R"(},"id":"peer-history"})");
    auto offline_history_write_result = co_await send_websocket_text(peer_socket, offline_history_request);
    auto& [offline_history_write_ec] = offline_history_write_result;
    if (offline_history_write_ec)
    {
        std::cerr << "FAIL offline message history write\n";
        co_return 1;
    }

    auto offline_history_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [offline_history_read_ec, offline_history_reply] = offline_history_reply_result;
    std::string expected_offline_history_reply = R"({"jsonrpc":"2.0","result":{"messages":[{"id":)";
    expected_offline_history_reply.append(offline_message->at(0));
    expected_offline_history_reply.append(R"(,"from":)");
    expected_offline_history_reply.append(source_user_id);
    expected_offline_history_reply.append(R"(,"timestamp":)");
    expected_offline_history_reply.append(offline_message->at(2));
    expected_offline_history_reply.append(R"(,"text":"offline hello"}],"has_more":false},"id":"peer-history"})");
    if (offline_history_read_ec || !json_matches(offline_history_reply, expected_offline_history_reply))
    {
        std::cerr << "FAIL offline message history\n";
        co_return 1;
    }
    std::cout << "PASS offline message history\n";

    std::string unread_before_read_request =
        R"({"jsonrpc":"2.0","method":"get_unread_count","params":{"conversation":)";
    unread_before_read_request.append(direct_conversation);
    unread_before_read_request.append(R"(},"id":"peer-unread-before-read"})");
    auto unread_before_read_write_result = co_await send_websocket_text(peer_socket, unread_before_read_request);
    auto& [unread_before_read_write_ec] = unread_before_read_write_result;
    if (unread_before_read_write_ec)
    {
        std::cerr << "FAIL unread before read write\n";
        co_return 1;
    }

    auto unread_before_read_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [unread_before_read_read_ec, unread_before_read_reply] = unread_before_read_reply_result;
    constexpr std::string_view expected_unread_before_read_reply =
        R"({"jsonrpc":"2.0","result":{"count":1},"id":"peer-unread-before-read"})";
    if (unread_before_read_read_ec || !json_matches(unread_before_read_reply, expected_unread_before_read_reply))
    {
        std::cerr << "FAIL unread before read\n";
        co_return 1;
    }
    std::cout << "PASS unread before read\n";

    std::string mark_offline_read_request = R"({"jsonrpc":"2.0","method":"mark_read","params":{"conversation":)";
    mark_offline_read_request.append(direct_conversation);
    mark_offline_read_request.append(R"(,"message":)");
    mark_offline_read_request.append(offline_message->at(0));
    mark_offline_read_request.append(R"(},"id":"peer-mark-offline-read"})");
    auto mark_offline_read_write_result = co_await send_websocket_text(peer_socket, mark_offline_read_request);
    auto& [mark_offline_read_write_ec] = mark_offline_read_write_result;
    if (mark_offline_read_write_ec)
    {
        std::cerr << "FAIL mark offline read write\n";
        co_return 1;
    }

    auto mark_offline_read_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [mark_offline_read_read_ec, mark_offline_read_reply] = mark_offline_read_reply_result;
    std::string expected_mark_offline_read_reply = R"({"jsonrpc":"2.0","result":{"message":)";
    expected_mark_offline_read_reply.append(offline_message->at(0));
    expected_mark_offline_read_reply.append(R"(},"id":"peer-mark-offline-read"})");
    if (mark_offline_read_read_ec || !json_matches(mark_offline_read_reply, expected_mark_offline_read_reply))
    {
        std::cerr << "FAIL mark offline read\n";
        co_return 1;
    }
    std::cout << "PASS mark offline read\n";

    auto offline_read_notification_result = co_await receive_websocket_text(source_socket);
    auto& [offline_read_notification_ec, offline_read_notification] = offline_read_notification_result;
    std::string expected_offline_read_notification = R"({"jsonrpc":"2.0","method":"read","params":{"user":)";
    expected_offline_read_notification.append(peer_user_id);
    expected_offline_read_notification.append(R"(,"message":)");
    expected_offline_read_notification.append(offline_message->at(0));
    expected_offline_read_notification.append("}}");
    if (offline_read_notification_ec || !json_matches(offline_read_notification, expected_offline_read_notification))
    {
        std::cerr << "FAIL offline read notification\n";
        co_return 1;
    }
    std::cout << "PASS offline read notification\n";

    std::string unread_after_read_request = R"({"jsonrpc":"2.0","method":"get_unread_count","params":{"conversation":)";
    unread_after_read_request.append(direct_conversation);
    unread_after_read_request.append(R"(},"id":"peer-unread-after-read"})");
    auto unread_after_read_write_result = co_await send_websocket_text(peer_socket, unread_after_read_request);
    auto& [unread_after_read_write_ec] = unread_after_read_write_result;
    if (unread_after_read_write_ec)
    {
        std::cerr << "FAIL unread after read write\n";
        co_return 1;
    }

    auto unread_after_read_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [unread_after_read_read_ec, unread_after_read_reply] = unread_after_read_reply_result;
    constexpr std::string_view expected_unread_after_read_reply =
        R"({"jsonrpc":"2.0","result":{"count":0},"id":"peer-unread-after-read"})";
    if (unread_after_read_read_ec || !json_matches(unread_after_read_reply, expected_unread_after_read_reply))
    {
        std::cerr << "FAIL unread after read\n";
        co_return 1;
    }
    std::cout << "PASS unread after read\n";

    std::string send_request_json = R"({"jsonrpc":"2.0","method":"send_message","params":{"conversation":)";
    send_request_json.append(direct_conversation);
    send_request_json.append(R"(,"text":"peer hello"},"id":"peer-send"})");
    auto send_write_result = co_await send_websocket_text(source_socket, send_request_json);
    auto& [send_write_ec] = send_write_result;
    if (send_write_ec)
    {
        std::cerr << "FAIL peer routing send write\n";
        co_return 1;
    }

    auto send_reply_result = co_await receive_websocket_text(source_socket);
    auto& [send_read_ec, send_reply] = send_reply_result;
    if (send_read_ec)
    {
        std::cerr << "FAIL peer routing send response\n";
        co_return 1;
    }

    std::vector<std::string> peer_message_parameters;
    peer_message_parameters.push_back(source_user_id);
    peer_message_parameters.push_back(peer_user_id);
    peer_message_parameters.emplace_back("peer hello");
    auto persisted_peer_message_result = co_await fixture_connection.execute_row(
        "SELECT id::text, ((extract(epoch from created_at) * 1000)::bigint)::text FROM messages "
        "WHERE sender_id = $1::bigint AND conversation_id=(SELECT id FROM conversations WHERE kind='direct' AND "
        "direct_user_low=least($1::bigint,$2::bigint) AND direct_user_high=greatest($1::bigint,$2::bigint)) AND body = "
        "$3 "
        "ORDER BY id DESC LIMIT 1",
        std::move(peer_message_parameters));
    auto& [persisted_peer_message_ec, persisted_peer_message] = persisted_peer_message_result;
    if (persisted_peer_message_ec || !persisted_peer_message || persisted_peer_message->size() != 2)
    {
        std::cerr << "FAIL peer message persistence: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }
    std::cout << "PASS peer message persistence\n";

    std::string expected_send_reply = R"({"jsonrpc":"2.0","result":{"message":)";
    expected_send_reply.append(persisted_peer_message->front());
    expected_send_reply.append(R"(,"timestamp":)");
    expected_send_reply.append(persisted_peer_message->at(1));
    expected_send_reply.append(R"(,"realtime":true},"id":"peer-send"})");
    if (!json_matches(send_reply, expected_send_reply))
    {
        std::cerr << "FAIL peer routing send response\n";
        co_return 1;
    }
    std::cout << "PASS peer message response\n";

    auto notification_result = co_await receive_websocket_text(peer_socket);
    auto& [notification_ec, notification] = notification_result;
    if (notification_ec)
    {
        std::cerr << "FAIL peer routing notification read\n";
        co_return 1;
    }

    std::string expected_notification = R"({"jsonrpc":"2.0","method":"message","params":{"id":)";
    expected_notification.append(persisted_peer_message->at(0));
    expected_notification.append(R"(,"from":)");
    expected_notification.append(source_user_id);
    expected_notification.append(R"(,"timestamp":)");
    expected_notification.append(persisted_peer_message->at(1));
    expected_notification.append(R"(,"text":"peer hello"}})");
    if (!json_matches(notification, expected_notification))
    {
        std::cerr << "FAIL peer message notification\n";
        co_return 1;
    }
    std::cout << "PASS peer message notification\n";

    std::string unread_after_live_request = R"({"jsonrpc":"2.0","method":"get_unread_count","params":{"conversation":)";
    unread_after_live_request.append(direct_conversation);
    unread_after_live_request.append(R"(},"id":"peer-unread-after-live"})");
    auto unread_after_live_write_result = co_await send_websocket_text(peer_socket, unread_after_live_request);
    auto& [unread_after_live_write_ec] = unread_after_live_write_result;
    if (unread_after_live_write_ec)
    {
        std::cerr << "FAIL unread after live write\n";
        co_return 1;
    }

    auto unread_after_live_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [unread_after_live_read_ec, unread_after_live_reply] = unread_after_live_reply_result;
    constexpr std::string_view expected_unread_after_live_reply =
        R"({"jsonrpc":"2.0","result":{"count":1},"id":"peer-unread-after-live"})";
    if (unread_after_live_read_ec || !json_matches(unread_after_live_reply, expected_unread_after_live_reply))
    {
        std::cerr << "FAIL unread after live\n";
        co_return 1;
    }
    std::cout << "PASS unread after live\n";

    std::string mark_live_read_request = R"({"jsonrpc":"2.0","method":"mark_read","params":{"conversation":)";
    mark_live_read_request.append(direct_conversation);
    mark_live_read_request.append(R"(,"message":)");
    mark_live_read_request.append(persisted_peer_message->at(0));
    mark_live_read_request.append(R"(},"id":"peer-mark-live-read"})");
    auto mark_live_read_write_result = co_await send_websocket_text(peer_socket, mark_live_read_request);
    auto& [mark_live_read_write_ec] = mark_live_read_write_result;
    if (mark_live_read_write_ec)
    {
        std::cerr << "FAIL mark live read write\n";
        co_return 1;
    }

    auto mark_live_read_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [mark_live_read_read_ec, mark_live_read_reply] = mark_live_read_reply_result;
    std::string expected_mark_live_read_reply = R"({"jsonrpc":"2.0","result":{"message":)";
    expected_mark_live_read_reply.append(persisted_peer_message->at(0));
    expected_mark_live_read_reply.append(R"(},"id":"peer-mark-live-read"})");
    if (mark_live_read_read_ec || !json_matches(mark_live_read_reply, expected_mark_live_read_reply))
    {
        std::cerr << "FAIL mark live read\n";
        co_return 1;
    }
    std::cout << "PASS mark live read\n";

    auto live_read_notification_result = co_await receive_websocket_text(source_socket);
    auto& [live_read_notification_ec, live_read_notification] = live_read_notification_result;
    std::string expected_live_read_notification = R"({"jsonrpc":"2.0","method":"read","params":{"user":)";
    expected_live_read_notification.append(peer_user_id);
    expected_live_read_notification.append(R"(,"message":)");
    expected_live_read_notification.append(persisted_peer_message->at(0));
    expected_live_read_notification.append("}}");
    if (live_read_notification_ec || !json_matches(live_read_notification, expected_live_read_notification))
    {
        std::cerr << "FAIL live read notification\n";
        co_return 1;
    }
    std::cout << "PASS live read notification\n";

    std::string source_history_request = R"({"jsonrpc":"2.0","method":"get_messages","params":{"conversation":)";
    source_history_request.append(direct_conversation);
    source_history_request.append(R"(},"id":"source-history-read"})");
    auto source_history_write_result = co_await send_websocket_text(source_socket, source_history_request);
    auto& [source_history_write_ec] = source_history_write_result;
    if (source_history_write_ec)
    {
        std::cerr << "FAIL source read history write\n";
        co_return 1;
    }

    auto source_history_reply_result = co_await receive_websocket_text(source_socket);
    auto& [source_history_read_ec, source_history_reply] = source_history_reply_result;
    std::string expected_source_history_reply = R"({"jsonrpc":"2.0","result":{"messages":[{"id":)";
    expected_source_history_reply.append(offline_message->at(0));
    expected_source_history_reply.append(R"(,"from":)");
    expected_source_history_reply.append(source_user_id);
    expected_source_history_reply.append(R"(,"timestamp":)");
    expected_source_history_reply.append(offline_message->at(2));
    expected_source_history_reply.append(R"(,"text":"offline hello"},{"id":)");
    expected_source_history_reply.append(persisted_peer_message->at(0));
    expected_source_history_reply.append(R"(,"from":)");
    expected_source_history_reply.append(source_user_id);
    expected_source_history_reply.append(R"(,"timestamp":)");
    expected_source_history_reply.append(persisted_peer_message->at(1));
    expected_source_history_reply.append(R"(,"text":"peer hello"}],"read_positions":[{"user":)");
    expected_source_history_reply.append(source_user_id);
    expected_source_history_reply.append(R"(,"message":0},{"user":)");
    expected_source_history_reply.append(peer_user_id);
    expected_source_history_reply.append(R"(,"message":)");
    expected_source_history_reply.append(persisted_peer_message->at(0));
    expected_source_history_reply.append("}]");
    expected_source_history_reply.append(R"(},"id":"source-history-read"})");
    if (source_history_read_ec || !json_matches(source_history_reply, expected_source_history_reply))
    {
        std::cerr << "FAIL source persisted read position\n";
        co_return 1;
    }
    std::cout << "PASS source persisted read position\n";

    std::string unread_after_live_read_request =
        R"({"jsonrpc":"2.0","method":"get_unread_count","params":{"conversation":)";
    unread_after_live_read_request.append(direct_conversation);
    unread_after_live_read_request.append(R"(},"id":"peer-unread-after-live-read"})");
    auto unread_after_live_read_write_result = co_await send_websocket_text(peer_socket, unread_after_live_read_request);
    auto& [unread_after_live_read_write_ec] = unread_after_live_read_write_result;
    if (unread_after_live_read_write_ec)
    {
        std::cerr << "FAIL unread after live read write\n";
        co_return 1;
    }

    auto unread_after_live_read_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [unread_after_live_read_read_ec, unread_after_live_read_reply] = unread_after_live_read_reply_result;
    constexpr std::string_view expected_unread_after_live_read_reply =
        R"({"jsonrpc":"2.0","result":{"count":0},"id":"peer-unread-after-live-read"})";
    if (unread_after_live_read_read_ec ||
        !json_matches(unread_after_live_read_reply, expected_unread_after_live_read_reply))
    {
        std::cerr << "FAIL unread after live read\n";
        co_return 1;
    }
    std::cout << "PASS unread after live read\n";

    constexpr std::string_view conversations_live_request =
        R"({"jsonrpc":"2.0","method":"get_conversations","id":"peer-conversations-live"})";
    auto conversations_live_write_result = co_await send_websocket_text(peer_socket, conversations_live_request);
    auto& [conversations_live_write_ec] = conversations_live_write_result;
    if (conversations_live_write_ec)
    {
        std::cerr << "FAIL conversation live summary write\n";
        co_return 1;
    }

    auto conversations_live_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [conversations_live_read_ec, conversations_live_reply] = conversations_live_reply_result;
    std::string expected_conversations_live_reply =
        R"({"jsonrpc":"2.0","result":{"conversations":[{"user":)";
    expected_conversations_live_reply.append(source_user_id);
    expected_conversations_live_reply.append(R"(,"username":"chat_server_test","last":{"id":)");
    expected_conversations_live_reply.append(persisted_peer_message->at(0));
    expected_conversations_live_reply.append(R"(,"from":)");
    expected_conversations_live_reply.append(source_user_id);
    expected_conversations_live_reply.append(R"(,"timestamp":)");
    expected_conversations_live_reply.append(persisted_peer_message->at(1));
    expected_conversations_live_reply.append(
        R"(,"text":"peer hello"},"unread":0}]},"id":"peer-conversations-live"})");
    if (conversations_live_read_ec || !json_matches(conversations_live_reply, expected_conversations_live_reply))
    {
        std::cerr << "FAIL conversation live summary\n";
        co_return 1;
    }
    std::cout << "PASS conversation live summary\n";

    std::string conversations_cursor_request =
        R"({"jsonrpc":"2.0","method":"get_conversations","params":{"before":)";
    auto cursor_result = co_await fixture_connection.execute_scalar(
        "SELECT json_build_object('activity',activity,'id',id)::text FROM conversations WHERE id=$1::bigint",
        {direct_conversation});
    auto& [cursor_ec, cursor] = cursor_result;
    if (cursor_ec)
    {
        co_return 1;
    }
    conversations_cursor_request.append(cursor);
    conversations_cursor_request.append(R"(},"id":"peer-conversations-cursor"})");
    auto conversations_cursor_write_result = co_await send_websocket_text(peer_socket, conversations_cursor_request);
    auto& [conversations_cursor_write_ec] = conversations_cursor_write_result;
    if (conversations_cursor_write_ec)
    {
        std::cerr << "FAIL conversation cursor write\n";
        co_return 1;
    }

    auto conversations_cursor_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [conversations_cursor_read_ec, conversations_cursor_reply] = conversations_cursor_reply_result;
    constexpr std::string_view expected_conversations_cursor_reply =
        R"({"jsonrpc":"2.0","result":{"conversations":[]},"id":"peer-conversations-cursor"})";
    if (conversations_cursor_read_ec || !json_matches(conversations_cursor_reply, expected_conversations_cursor_reply))
    {
        std::cerr << "FAIL conversation cursor\n";
        co_return 1;
    }
    std::cout << "PASS conversation cursor\n";

    std::string mark_older_read_request = R"({"jsonrpc":"2.0","method":"mark_read","params":{"conversation":)";
    mark_older_read_request.append(direct_conversation);
    mark_older_read_request.append(R"(,"message":)");
    mark_older_read_request.append(offline_message->at(0));
    mark_older_read_request.append(R"(},"id":"peer-mark-older-read"})");
    auto mark_older_read_write_result = co_await send_websocket_text(peer_socket, mark_older_read_request);
    auto& [mark_older_read_write_ec] = mark_older_read_write_result;
    if (mark_older_read_write_ec)
    {
        std::cerr << "FAIL mark older read write\n";
        co_return 1;
    }

    auto mark_older_read_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [mark_older_read_read_ec, mark_older_read_reply] = mark_older_read_reply_result;
    std::string expected_mark_older_read_reply = R"({"jsonrpc":"2.0","result":{"message":)";
    expected_mark_older_read_reply.append(persisted_peer_message->at(0));
    expected_mark_older_read_reply.append(R"(},"id":"peer-mark-older-read"})");
    if (mark_older_read_read_ec || !json_matches(mark_older_read_reply, expected_mark_older_read_reply))
    {
        std::cerr << "FAIL mark read monotonic\n";
        co_return 1;
    }
    std::cout << "PASS mark read monotonic\n";

    auto monotonic_read_notification_result = co_await receive_websocket_text(source_socket);
    auto& [monotonic_read_notification_ec, monotonic_read_notification] = monotonic_read_notification_result;
    std::string expected_monotonic_read_notification = R"({"jsonrpc":"2.0","method":"read","params":{"user":)";
    expected_monotonic_read_notification.append(peer_user_id);
    expected_monotonic_read_notification.append(R"(,"message":)");
    expected_monotonic_read_notification.append(persisted_peer_message->at(0));
    expected_monotonic_read_notification.append("}}");
    if (monotonic_read_notification_ec ||
        !json_matches(monotonic_read_notification, expected_monotonic_read_notification))
    {
        std::cerr << "FAIL monotonic read notification\n";
        co_return 1;
    }
    std::cout << "PASS monotonic read notification\n";

    std::string peer_history_request = R"({"jsonrpc":"2.0","method":"get_messages","params":{"conversation":)";
    peer_history_request.append(direct_conversation);
    peer_history_request.append(R"(},"id":"peer-history-order"})");
    auto peer_history_write_result = co_await send_websocket_text(peer_socket, peer_history_request);
    auto& [peer_history_write_ec] = peer_history_write_result;
    if (peer_history_write_ec)
    {
        std::cerr << "FAIL peer message history write\n";
        co_return 1;
    }

    auto peer_history_reply_result = co_await receive_websocket_text(peer_socket);
    auto& [peer_history_read_ec, peer_history_reply] = peer_history_reply_result;
    std::string expected_peer_history_reply = R"({"jsonrpc":"2.0","result":{"messages":[{"id":)";
    expected_peer_history_reply.append(offline_message->at(0));
    expected_peer_history_reply.append(R"(,"from":)");
    expected_peer_history_reply.append(source_user_id);
    expected_peer_history_reply.append(R"(,"timestamp":)");
    expected_peer_history_reply.append(offline_message->at(2));
    expected_peer_history_reply.append(R"(,"text":"offline hello"},{"id":)");
    expected_peer_history_reply.append(persisted_peer_message->at(0));
    expected_peer_history_reply.append(R"(,"from":)");
    expected_peer_history_reply.append(source_user_id);
    expected_peer_history_reply.append(R"(,"timestamp":)");
    expected_peer_history_reply.append(persisted_peer_message->at(1));
    expected_peer_history_reply.append(R"(,"text":"peer hello"}],"has_more":false},"id":"peer-history-order"})");
    if (peer_history_read_ec || !json_matches(peer_history_reply, expected_peer_history_reply))
    {
        std::cerr << "FAIL peer message history order\n";
        co_return 1;
    }
    std::cout << "PASS peer message history order\n";

    peer_socket.close();

    auto offline_presence_result = co_await receive_websocket_text(source_socket);
    auto& [offline_presence_ec, offline_presence] = offline_presence_result;
    if (offline_presence_ec)
    {
        std::cerr << "FAIL offline presence notification read\n";
        co_return 1;
    }

    auto peer_offline_last_seen_result = co_await fixture_connection.execute_scalar(
        "SELECT ((extract(epoch from last_seen_at) * 1000)::bigint)::text FROM users WHERE id = $1::bigint",
        {peer_user_id});
    auto& [peer_offline_last_seen_ec, peer_offline_last_seen] = peer_offline_last_seen_result;
    if (peer_offline_last_seen_ec || peer_offline_last_seen.empty())
    {
        std::cerr << "FAIL offline presence persistence: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }

    std::string expected_offline_presence = R"({"jsonrpc":"2.0","method":"presence","params":{"user":)";
    expected_offline_presence.append(peer_user_id);
    expected_offline_presence.append(R"(,"online":false,"last_seen":)");
    expected_offline_presence.append(peer_offline_last_seen);
    expected_offline_presence.append("}}");
    if (offline_presence != expected_offline_presence)
    {
        std::cerr << "FAIL offline presence notification\n";
        co_return 1;
    }
    std::cout << "PASS offline presence notification\n";

    source_socket.close();
    co_return 0;
}


}    // namespace

int run_group_tests();

int main()
{
    boost::corosio::io_context io_context;

    boost::http::router<boost::http::route_params> router;
    router.add(boost::http::method::get, "/health", health_handler);
    router.use(not_found_handler);

    chat_server server(io_context, 1, std::move(router), std::string(kDatabaseConnectionString), 1);
    if (auto ec = server.bind(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "FAIL server bind: " << ec.message() << '\n';
        return 1;
    }

    boost::http::router<boost::http::route_params> peer_router;
    peer_router.add(boost::http::method::get, "/health", health_handler);
    peer_router.use(not_found_handler);
    chat_server peer_server(io_context, 2, std::move(peer_router), std::string(kDatabaseConnectionString), 2);
    if (auto ec = peer_server.bind(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "FAIL peer server bind: " << ec.message() << '\n';
        return 1;
    }

    auto const port = server.local_endpoint().port();
    auto const peer_port = peer_server.local_endpoint().port();
    server.start();
    peer_server.start();

    int exit_code = 1;
    int peer_exit_code = 1;
    boost::capy::async_event primary_done;
    boost::capy::run_async(io_context.get_executor(), [&exit_code, &primary_done](int result) {
        exit_code = result;
        primary_done.set();
    })(run_client(io_context, server, port));
    boost::capy::run_async(io_context.get_executor(), [&peer_exit_code](int result) { peer_exit_code = result; })(
        run_peer_routing(io_context, peer_server, peer_port, primary_done, exit_code));

    io_context.run();
    server.join();
    peer_server.join();

    if (exit_code != 0)
    {
        return exit_code;
    }
    if (peer_exit_code != 0)
    {
        return peer_exit_code;
    }

    std::cout << "PASS chat server shutdown\n";
    std::cout << "PASS chat server validation\n";
    return run_group_tests();
}
