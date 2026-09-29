#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/http/config.hpp>
#include <boost/http/field.hpp>
#include <boost/http/method.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/response.hpp>
#include <boost/http/response_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/status.hpp>
#include <wslay/wslay.h>

#include "websocket.hpp"

namespace capy = boost::capy;
namespace corosio = boost::corosio;
namespace http = boost::http;

namespace
{

constexpr std::string_view kWebSocketKey = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr std::string_view kWebSocketAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
constexpr std::string_view kTextMessage = "hello websocket";
constexpr std::string_view kPingPayload = "ping";

std::string_view as_string_view(boost::core::string_view value) { return {value.data(), value.size()}; }

std::string_view trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    {
        value.remove_suffix(1);
    }
    return value;
}

char ascii_lower(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool ascii_iequals(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        if (ascii_lower(lhs[i]) != ascii_lower(rhs[i]))
        {
            return false;
        }
    }
    return true;
}

bool contains_token(std::string_view value, std::string_view token)
{
    while (!value.empty())
    {
        auto const comma = value.find(',');
        auto const current = trim(value.substr(0, comma));
        if (ascii_iequals(current, token))
        {
            return true;
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return false;
}

class websocket_peer
{
   public:
    websocket_peer(bool server, bool echo_messages) : echo_messages_(echo_messages)
    {
        wslay_event_callbacks callbacks{};
        callbacks.recv_callback = &receive_callback;
        callbacks.genmask_callback = server ? nullptr : &genmask_callback;
        callbacks.on_msg_recv_callback = &message_callback;

        wslay_event_context_ptr context = nullptr;
        auto const result =
            server ? wslay_event_context_server_init(&context, &callbacks, this) : wslay_event_context_client_init(&context, &callbacks, this);
        if (result == 0)
        {
            context_.reset(context);
        }
    }

    bool valid() const noexcept { return context_ != nullptr; }

    bool want_read() const noexcept { return context_ && wslay_event_want_read(context_.get()) != 0; }

    bool want_write() const noexcept { return context_ && wslay_event_want_write(context_.get()) != 0; }

    bool close_received() const noexcept { return context_ && wslay_event_get_close_received(context_.get()) != 0; }

    bool got_pong() const noexcept { return got_pong_; }

    std::string const& last_text() const noexcept { return last_text_; }

    std::error_code queue_text(std::string_view text) { return queue_message(WSLAY_TEXT_FRAME, text); }

    std::error_code queue_ping(std::string_view payload) { return queue_message(WSLAY_PING, payload); }

    std::error_code queue_close()
    {
        if (!context_ || wslay_event_queue_close(context_.get(), WSLAY_CODE_NORMAL_CLOSURE, nullptr, 0) != 0)
        {
            return std::make_error_code(std::errc::protocol_error);
        }
        return {};
    }

    capy::io_task<> send_pending(corosio::tcp_socket& socket)
    {
        std::array<std::uint8_t, 4096> output{};

        while (want_write())
        {
            auto const result = wslay_event_write(context_.get(), output.data(), output.size());
            if (result <= 0)
            {
                co_return std::make_error_code(std::errc::protocol_error);
            }

            auto const size = static_cast<std::size_t>(result);
            auto [ec, written] = co_await capy::write(socket, capy::const_buffer(output.data(), size));
            if (ec)
            {
                co_return ec;
            }
            if (written != size)
            {
                co_return std::make_error_code(std::errc::io_error);
            }
        }

        co_return {};
    }

    capy::io_task<> receive(corosio::tcp_socket& socket)
    {
        auto [ec, size] = co_await socket.read_some(capy::mutable_buffer(input_buffer_.data(), input_buffer_.size()));
        if (ec)
        {
            co_return ec;
        }
        if (size == 0)
        {
            co_return std::make_error_code(std::errc::connection_reset);
        }

        input_ = std::span<std::uint8_t const>(input_buffer_.data(), size);
        callback_error_ = false;
        auto const result = wslay_event_recv(context_.get());
        if (result != 0 || callback_error_ || !input_.empty())
        {
            co_return std::make_error_code(std::errc::protocol_error);
        }

        co_return {};
    }

   private:
    struct context_deleter
    {
        void operator()(wslay_event_context* context) const noexcept { wslay_event_context_free(context); }
    };

    std::error_code queue_message(std::uint8_t opcode, std::string_view payload)
    {
        if (!context_)
        {
            return std::make_error_code(std::errc::not_connected);
        }

        wslay_event_msg message{};
        message.opcode = opcode;
        message.msg = reinterpret_cast<std::uint8_t const*>(payload.data());
        message.msg_length = payload.size();
        if (wslay_event_queue_msg(context_.get(), &message) != 0)
        {
            return std::make_error_code(std::errc::protocol_error);
        }
        return {};
    }

    static ssize_t receive_callback(wslay_event_context_ptr context, std::uint8_t* buffer, std::size_t size, int, void* user_data)
    {
        auto& self = *static_cast<websocket_peer*>(user_data);
        if (self.input_.empty())
        {
            wslay_event_set_error(context, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }

        auto const copied = std::min(size, self.input_.size());
        std::memcpy(buffer, self.input_.data(), copied);
        self.input_ = self.input_.subspan(copied);
        return static_cast<ssize_t>(copied);
    }

    static int genmask_callback(wslay_event_context_ptr, std::uint8_t* buffer, std::size_t size, void* user_data)
    {
        auto& self = *static_cast<websocket_peer*>(user_data);
        for (std::size_t i = 0; i < size; ++i)
        {
            buffer[i] = static_cast<std::uint8_t>((self.mask_seed_ + i * 29U) & 0xffU);
        }
        self.mask_seed_ += static_cast<std::uint32_t>(size);
        return 0;
    }

    static void message_callback(wslay_event_context_ptr context, wslay_event_on_msg_recv_arg const* message, void* user_data)
    {
        auto& self = *static_cast<websocket_peer*>(user_data);

        if (message->opcode == WSLAY_TEXT_FRAME)
        {
            self.last_text_.assign(reinterpret_cast<char const*>(message->msg), message->msg_length);
            if (self.echo_messages_)
            {
                wslay_event_msg reply{};
                reply.opcode = WSLAY_TEXT_FRAME;
                reply.msg = message->msg;
                reply.msg_length = message->msg_length;
                if (wslay_event_queue_msg(context, &reply) != 0)
                {
                    self.callback_error_ = true;
                }
            }
        }
        else if (message->opcode == WSLAY_PONG)
        {
            self.got_pong_ = true;
        }
    }

    std::unique_ptr<wslay_event_context, context_deleter> context_;
    std::array<std::uint8_t, 4096> input_buffer_{};
    std::span<std::uint8_t const> input_;
    std::string last_text_;
    std::uint32_t mask_seed_ = 1;
    bool echo_messages_ = false;
    bool got_pong_ = false;
    bool callback_error_ = false;
};

class websocket_test_worker final : public corosio::tcp_server::worker_base
{
   public:
    websocket_test_worker(corosio::io_context& io_context, http::shared_parser_config parser_config, http::shared_serializer_config serializer_config)
        : io_context_(io_context), socket_(io_context), parser_(std::move(parser_config)), serializer_(std::move(serializer_config))
    {
        serializer_.set_message(response_);
    }

    corosio::tcp_socket& socket() override { return socket_; }

    void run(corosio::tcp_server::launcher launch) override { launch(io_context_.get_executor(), run_session()); }

   private:
    capy::io_task<> send_upgrade_response(std::string_view accept)
    {
        response_.clear();
        response_.set_start_line(http::status::switching_protocols, http::version::http_1_1);
        response_.set(http::field::upgrade, "websocket");
        response_.set(http::field::connection, "Upgrade");
        response_.set(http::field::sec_websocket_accept, accept);

        serializer_.reset();
        serializer_.start();
        while (!serializer_.is_done())
        {
            auto prepared = serializer_.prepare();
            if (prepared.has_error())
            {
                co_return std::error_code(prepared.error());
            }

            if (capy::buffer_empty(*prepared))
            {
                serializer_.consume(0);
                continue;
            }

            auto [ec, written] = co_await capy::write(socket_, *prepared);
            serializer_.consume(written);
            if (ec)
            {
                co_return ec;
            }
        }

        co_return {};
    }

    capy::task<void> run_session()
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
        if (!connection.valid())
        {
            socket_.close();
            co_return;
        }

        for (;;)
        {
            auto [ec, message] = co_await connection.receive();
            if (ec || message.message_type == websocket_message::type::close)
            {
                break;
            }

            if (message.message_type == websocket_message::type::text)
            {
                auto [send_ec] = co_await connection.send_text(message.payload);
                if (send_ec)
                {
                    break;
                }
            }
        }

        socket_.close();
    }

    corosio::io_context& io_context_;
    corosio::tcp_socket socket_;
    http::request_parser parser_;
    http::response response_;
    http::serializer serializer_;
};

std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> make_workers(corosio::io_context& io_context,
                                                                            http::shared_parser_config const& parser_config,
                                                                            http::shared_serializer_config const& serializer_config)
{
    std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> workers;
    workers.push_back(std::make_unique<websocket_test_worker>(io_context, parser_config, serializer_config));
    return workers;
}

struct server_stop_guard
{
    corosio::tcp_server& server;

    ~server_stop_guard() { server.stop(); }
};

capy::task<int> run_client(corosio::io_context& io_context, corosio::tcp_server& server, unsigned short port)
{
    server_stop_guard stop_guard{server};
    corosio::tcp_socket socket(io_context);

    auto [connect_ec] = co_await socket.connect(corosio::endpoint(corosio::ipv4_address::loopback(), port));
    if (connect_ec)
    {
        std::cerr << "FAIL WebSocket connect: " << connect_ec.message() << '\n';
        co_return 1;
    }
    std::cout << "PASS WebSocket connect\n";

    std::string request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: ";
    request.append(kWebSocketKey);
    request.append("\r\nSec-WebSocket-Version: 13\r\n\r\n");

    auto [request_ec, request_size] = co_await capy::write(socket, capy::const_buffer(request.data(), request.size()));
    if (request_ec || request_size != request.size())
    {
        std::cerr << "FAIL WebSocket upgrade request\n";
        co_return 1;
    }

    auto parser_config = http::make_parser_config(http::parser_config{true});
    http::response_parser response_parser(parser_config);
    response_parser.reset();
    response_parser.start();
    auto [response_ec] = co_await response_parser.read(socket);
    if (response_ec)
    {
        std::cerr << "FAIL WebSocket upgrade response: " << response_ec.message() << '\n';
        co_return 1;
    }

    auto const& response = response_parser.get();
    auto const connection = as_string_view(response.value_or(http::field::connection, ""));
    auto const upgrade = as_string_view(response.value_or(http::field::upgrade, ""));
    auto const accept = as_string_view(response.value_or(http::field::sec_websocket_accept, ""));
    if (response.status() != http::status::switching_protocols || !contains_token(connection, "upgrade") || !contains_token(upgrade, "websocket") ||
        accept != kWebSocketAccept || response_parser.has_buffered_data())
    {
        std::cerr << "FAIL WebSocket upgrade validation\n";
        co_return 1;
    }
    std::cout << "PASS WebSocket upgrade\n";

    websocket_peer peer(false, false);
    if (!peer.valid())
    {
        std::cerr << "FAIL WebSocket client context\n";
        co_return 1;
    }

    if (auto ec = peer.queue_text(kTextMessage))
    {
        std::cerr << "FAIL WebSocket queue text: " << ec.message() << '\n';
        co_return 1;
    }
    if (auto [ec] = co_await peer.send_pending(socket); ec)
    {
        std::cerr << "FAIL WebSocket send text: " << ec.message() << '\n';
        co_return 1;
    }
    while (peer.last_text() != kTextMessage)
    {
        auto [ec] = co_await peer.receive(socket);
        if (ec)
        {
            std::cerr << "FAIL WebSocket receive echo: " << ec.message() << '\n';
            co_return 1;
        }
    }
    std::cout << "PASS WebSocket text echo\n";

    if (auto ec = peer.queue_ping(kPingPayload))
    {
        std::cerr << "FAIL WebSocket queue ping: " << ec.message() << '\n';
        co_return 1;
    }
    if (auto [ec] = co_await peer.send_pending(socket); ec)
    {
        std::cerr << "FAIL WebSocket send ping: " << ec.message() << '\n';
        co_return 1;
    }
    while (!peer.got_pong())
    {
        auto [ec] = co_await peer.receive(socket);
        if (ec)
        {
            std::cerr << "FAIL WebSocket receive pong: " << ec.message() << '\n';
            co_return 1;
        }
    }
    std::cout << "PASS WebSocket ping/pong\n";

    if (auto ec = peer.queue_close())
    {
        std::cerr << "FAIL WebSocket queue close: " << ec.message() << '\n';
        co_return 1;
    }
    if (auto [ec] = co_await peer.send_pending(socket); ec)
    {
        std::cerr << "FAIL WebSocket send close: " << ec.message() << '\n';
        co_return 1;
    }
    while (!peer.close_received())
    {
        auto [ec] = co_await peer.receive(socket);
        if (ec)
        {
            std::cerr << "FAIL WebSocket receive close: " << ec.message() << '\n';
            co_return 1;
        }
    }
    std::cout << "PASS WebSocket close handshake\n";

    socket.close();
    co_return 0;
}

}    // namespace

int main()
{
    corosio::io_context io_context;

    auto parser_config = http::make_parser_config(http::parser_config{true});
    auto serializer_config = http::make_serializer_config(http::serializer_config{});

    corosio::tcp_server server(io_context, io_context.get_executor());
    server.set_workers(make_workers(io_context, parser_config, serializer_config));

    if (auto ec = server.bind(corosio::endpoint(corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "FAIL WebSocket bind: " << ec.message() << '\n';
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

    std::cout << "PASS WebSocket server shutdown\n";
    std::cout << "PASS WebSocket validation\n";
    return 0;
}
