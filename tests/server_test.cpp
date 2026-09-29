#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/http/config.hpp>
#include <boost/http/field.hpp>
#include <boost/http/method.hpp>
#include <boost/http/response_parser.hpp>
#include <boost/http/server/router.hpp>
#include <boost/http/status.hpp>

#include "server.hpp"

namespace capy = boost::capy;
namespace corosio = boost::corosio;
namespace http = boost::http;

namespace
{

constexpr std::string_view kHealthBody = R"({"status":"ok"})";
constexpr std::string_view kNotFoundBody = "not found";
constexpr std::string_view kWebSocketKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr std::string_view kWebSocketAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

http::route_task health_handler(http::route_params& params)
{
    params.status(http::status::ok);
    params.res.set(http::field::content_type, "application/json");

    auto [ec] = co_await params.send(kHealthBody);
    if (ec)
    {
        co_return http::route_error(ec);
    }

    co_return http::route_done;
}

http::route_task not_found_handler(http::route_params& params)
{
    params.status(http::status::not_found);

    auto [ec] = co_await params.send(kNotFoundBody);
    if (ec)
    {
        co_return http::route_error(ec);
    }

    co_return http::route_done;
}

struct server_stop_guard
{
    chat_server& server;

    ~server_stop_guard() { server.stop(); }
};

capy::io_task<> connect(corosio::tcp_socket& socket, unsigned short port)
{
    auto [ec] = co_await socket.connect(corosio::endpoint(corosio::ipv4_address::loopback(), port));
    co_return ec;
}

capy::io_task<> send_request(corosio::tcp_socket& socket, std::string_view request)
{
    auto [ec, written] = co_await capy::write(socket, capy::const_buffer(request.data(), request.size()));
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

bool valid_health_response(http::response_parser const& parser)
{
    return parser.get().status() == http::status::ok && parser.body() == kHealthBody;
}

capy::io_task<> upgrade_websocket(corosio::tcp_socket& socket, http::response_parser& parser)
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

    auto const accept = parser.get().value_or(http::field::sec_websocket_accept, "");
    if (parser.get().status() != http::status::switching_protocols || std::string_view(accept.data(), accept.size()) != kWebSocketAccept ||
        parser.has_buffered_data())
    {
        co_return std::make_error_code(std::errc::protocol_error);
    }

    co_return {};
}

capy::task<int> run_client(corosio::io_context& io_context, chat_server& server, unsigned short port)
{
    server_stop_guard stop_guard{server};

    {
        corosio::tcp_socket socket(io_context);
        auto [connect_ec] = co_await connect(socket, port);
        if (connect_ec)
        {
            std::cerr << "FAIL server HTTP connect: " << connect_ec.message() << '\n';
            co_return 1;
        }

        auto parser_config = http::make_parser_config(http::parser_config{true});
        http::response_parser parser(parser_config);

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
        if (second_read_ec || parser.get().status() != http::status::not_found || parser.body() != kNotFoundBody)
        {
            std::cerr << "FAIL second HTTP response\n";
            co_return 1;
        }

        socket.close();
        std::cout << "PASS server HTTP keep-alive\n";
    }

    {
        corosio::tcp_socket socket(io_context);
        auto [connect_ec] = co_await connect(socket, port);
        if (connect_ec)
        {
            std::cerr << "FAIL WebSocket connect: " << connect_ec.message() << '\n';
            co_return 1;
        }

        auto parser_config = http::make_parser_config(http::parser_config{true});
        http::response_parser parser(parser_config);
        auto [upgrade_ec] = co_await upgrade_websocket(socket, parser);
        if (upgrade_ec)
        {
            std::cerr << "FAIL server WebSocket upgrade: " << upgrade_ec.message() << '\n';
            co_return 1;
        }
        std::cout << "PASS server WebSocket upgrade\n";

        constexpr std::array<std::uint8_t, 10> ping = {0x89, 0x84, 0x01, 0x02, 0x03, 0x04, 0x71, 0x6b, 0x6d, 0x63};
        auto [ping_ec, ping_written] = co_await capy::write(socket, capy::const_buffer(ping.data(), ping.size()));
        if (ping_ec || ping_written != ping.size())
        {
            std::cerr << "FAIL server WebSocket ping write\n";
            co_return 1;
        }

        std::array<std::uint8_t, 6> pong{};
        auto [pong_ec, pong_read] = co_await capy::read(socket, capy::mutable_buffer(pong.data(), pong.size()));
        constexpr std::array<std::uint8_t, 6> expected_pong = {0x8a, 0x04, 'p', 'i', 'n', 'g'};
        if (pong_ec || pong_read != pong.size() || pong != expected_pong)
        {
            std::cerr << "FAIL server WebSocket pong\n";
            co_return 1;
        }
        std::cout << "PASS server WebSocket ping/pong\n";

        constexpr std::array<std::uint8_t, 8> close = {0x88, 0x82, 0x12, 0x34, 0x56, 0x78, 0x11, 0xdc};
        auto [close_ec, close_written] = co_await capy::write(socket, capy::const_buffer(close.data(), close.size()));
        if (close_ec || close_written != close.size())
        {
            std::cerr << "FAIL server WebSocket close write\n";
            co_return 1;
        }

        std::array<std::uint8_t, 4> close_reply{};
        auto [reply_ec, reply_read] = co_await capy::read(socket, capy::mutable_buffer(close_reply.data(), close_reply.size()));
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
        corosio::tcp_socket socket(io_context);
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

        auto parser_config = http::make_parser_config(http::parser_config{true});
        http::response_parser parser(parser_config);
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
        corosio::tcp_socket socket(io_context);
        auto [connect_ec] = co_await connect(socket, port);
        if (connect_ec)
        {
            std::cerr << "FAIL active WebSocket connect: " << connect_ec.message() << '\n';
            co_return 1;
        }

        auto parser_config = http::make_parser_config(http::parser_config{true});
        http::response_parser parser(parser_config);
        auto [upgrade_ec] = co_await upgrade_websocket(socket, parser);
        if (upgrade_ec)
        {
            std::cerr << "FAIL active WebSocket upgrade: " << upgrade_ec.message() << '\n';
            co_return 1;
        }

        server.stop();

        std::array<std::uint8_t, 1> input{};
        auto [read_ec, size] = co_await socket.read_some(capy::mutable_buffer(input.data(), input.size()));
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
    corosio::io_context io_context;

    http::router<http::route_params> router;
    router.add(http::method::get, "/health", health_handler);
    router.use(not_found_handler);

    chat_server server(io_context, 1, std::move(router));
    if (auto ec = server.bind(corosio::endpoint(corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "FAIL server bind: " << ec.message() << '\n';
        return 1;
    }

    auto const port = server.local_endpoint().port();
    server.start();

    int exit_code = 1;
    capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(run_client(io_context, server, port));

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
