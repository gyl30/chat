#include <array>
#include <string>
#include <vector>
#include <cstdint>
#include <utility>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <system_error>

#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/http/field.hpp>
#include <boost/http/bcrypt.hpp>
#include <boost/http/config.hpp>
#include <boost/http/method.hpp>
#include <boost/http/status.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/endpoint.hpp>
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

constexpr std::string_view kHealthBody = R"({"status":"ok"})";
constexpr std::string_view kNotFoundBody = "not found";
constexpr std::string_view kWebSocketKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr std::string_view kWebSocketAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
constexpr std::string_view kDatabaseConnectionString =
    "hostaddr=172.20.54.83 "
    "port=5432 "
    "dbname=chat "
    "user=chat "
    "sslmode=disable";
constexpr char kTestUsername[] = "chat_server_test";
constexpr char kTestPassword[] = "test password";

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
    if (header_read != header.size() || header[0] != 0x81 || (header[1] & 0x80U) != 0 || (header[1] & 0x7fU) > 125)
    {
        co_return boost::capy::io_result<std::string>{std::make_error_code(std::errc::protocol_error), {}};
    }

    std::string payload(header[1] & 0x7fU, '\0');
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

    auto fixture_hash = co_await boost::http::bcrypt::hash_async(kTestPassword, 4, boost::http::bcrypt::version::v2b);
    std::vector<std::string> fixture_parameters;
    fixture_parameters.emplace_back(kTestUsername);
    fixture_parameters.emplace_back(fixture_hash.data(), fixture_hash.size());
    auto fixture_result = co_await fixture_connection.execute_scalar(
        "INSERT INTO users (username, password_hash) VALUES ($1, $2) "
        "ON CONFLICT (username) DO UPDATE SET password_hash = EXCLUDED.password_hash "
        "RETURNING id::text",
        std::move(fixture_parameters));
    auto& [fixture_ec, fixture_user_id] = fixture_result;
    (void)fixture_user_id;
    if (fixture_ec)
    {
        std::cerr << "FAIL authentication fixture: " << fixture_connection.error_message() << '\n';
        co_return 1;
    }
    fixture_connection.close();

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
        if (unauthenticated_echo_read_ec || unauthenticated_echo_reply != expected_unauthenticated_echo_reply)
        {
            std::cerr << "FAIL unauthenticated echo rejected\n";
            co_return 1;
        }
        std::cout << "PASS unauthenticated echo rejected\n";

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
        if (invalid_authentication_read_ec || invalid_authentication_reply != expected_invalid_authentication_reply)
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
        if (missing_user_authentication_read_ec || missing_user_authentication_reply != expected_failed_authentication_reply)
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
        if (wrong_password_authentication_read_ec || wrong_password_authentication_reply != expected_wrong_password_authentication_reply)
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
        if (authentication_read_ec || authentication_reply != expected_authentication_reply)
        {
            std::cerr << "FAIL authentication success\n";
            co_return 1;
        }
        std::cout << "PASS authentication success\n";

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
        if (echo_read_ec || echo_reply != expected_echo_reply)
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
        if (null_id_read_ec || null_id_reply != expected_null_id_reply)
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
        if (missing_method_read_ec || missing_method_reply != expected_missing_method_reply)
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
        if (invalid_params_read_ec || invalid_params_reply != expected_invalid_params_reply)
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
        if (malformed_read_ec || malformed_reply != expected_malformed_reply)
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
        if (invalid_request_read_ec || invalid_request_reply != expected_invalid_request_reply)
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


}    // namespace

int main()
{
    boost::corosio::io_context io_context;

    boost::http::router<boost::http::route_params> router;
    router.add(boost::http::method::get, "/health", health_handler);
    router.use(not_found_handler);

    chat_server server(io_context, 1, std::move(router), std::string(kDatabaseConnectionString));
    if (auto ec = server.bind(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "FAIL server bind: " << ec.message() << '\n';
        return 1;
    }

    auto const port = server.local_endpoint().port();
    server.start();

    int exit_code = 1;
    boost::capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(run_client(io_context, server, port));

    io_context.run();
    server.join();

    if (exit_code != 0)
    {
        return exit_code;
    }

    std::cout << "PASS chat server shutdown\n";
    std::cout << "PASS chat server validation\n";
    return 0;
}
