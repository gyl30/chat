#include "../src/websocket.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/timeout.hpp>
#include <boost/http/config.hpp>
#include <boost/http/field.hpp>
#include <boost/http/request_parser.hpp>

namespace
{
using namespace std::chrono_literals;
using heartbeat = chat::detail::websocket_heartbeat;
enum class behavior { silent, matching, wrong, traffic, partial, batch, ack_before_timeout };
constexpr std::array behaviors{behavior::silent, behavior::matching, behavior::wrong, behavior::traffic, behavior::partial, behavior::batch, behavior::ack_before_timeout};
constexpr std::array names{"silent", "matching pong", "wrong pong", "unrelated traffic", "partial frame and interrupt", "busy outgoing queue consumes pong", "received ACK survives pump timeout"};

struct case_state
{
    bool upgraded = false;
    bool passed = false;
    bool hello = false;
    int pings = 0;
    int interrupts = 0;
    int requests = 0;
    bool batch_finished = false;
};

bool check_heartbeat_clock()
{
    auto const start = heartbeat::clock::time_point{};
    heartbeat state({10ms, 20ms});
    state.reset(start);
    if (state.probe_due(start + 9ms) || !state.probe_due(start + 10ms)) { return false; }
    auto first = state.start_probe(start + 10ms);
    if (first != heartbeat::token{0, 0, 0, 0, 0, 0, 0, 1} || !state.pending() || state.deadline() != start + 30ms) { return false; }
    auto wrong = first;
    ++wrong.back();
    state.pong(wrong, start + 15ms);
    if (!state.pending()) { return false; }
    state.pong(first, start + 29ms);
    if (state.pending() || state.deadline() != start + 39ms) { return false; }
    auto second = state.start_probe(start + 39ms);
    state.pong(first, start + 40ms);
    if (!state.pending() || !state.expired(start + 59ms)) { return false; }
    state.pong(second, start + 59ms);
    if (!state.expired(start + 59ms)) { return false; }
    state.reset(start + 60ms);
    auto third = state.start_probe(start + 70ms);
    return third.back() == 3 && third != first && third != second;
}

struct raw_frame
{
    unsigned char opcode = 0;
    std::string payload;
};

class heartbeat_worker final : public boost::corosio::tcp_server::worker_base
{
   public:
    heartbeat_worker(boost::corosio::io_context& context, std::array<case_state, 7>& states)
        : context_(context), socket_(context), states_(states), parser_(boost::http::make_parser_config(boost::http::parser_config{true})) {}
    boost::corosio::tcp_socket& socket() override { return socket_; }
    void run(boost::corosio::tcp_server::launcher launch) override { launch(context_.get_executor(), session()); }

   private:
    boost::capy::io_task<> write_bytes(std::string const& bytes)
    {
        auto [ec, size] = co_await boost::capy::write(socket_, boost::capy::const_buffer(bytes.data(), bytes.size()));
        co_return ec ? ec : size == bytes.size() ? std::error_code{} : std::make_error_code(std::errc::io_error);
    }

    boost::capy::io_task<> write_frame(unsigned char opcode, std::string payload)
    {
        std::string bytes{static_cast<char>(0x80 | opcode), static_cast<char>(payload.size())};
        bytes += payload;
        co_return co_await write_bytes(bytes);
    }

    boost::capy::io_task<raw_frame> read_frame()
    {
        std::array<unsigned char, 2> header{};
        auto [ec, size] = co_await boost::capy::read(socket_, boost::capy::mutable_buffer(header.data(), header.size()));
        if (ec) { co_return boost::capy::io_result<raw_frame>{ec, {}}; }
        if ((header[1] & 0x80) == 0 || (header[1] & 0x7f) > 125)
        { co_return boost::capy::io_result<raw_frame>{std::make_error_code(std::errc::protocol_error), {}}; }
        std::array<unsigned char, 4> mask{};
        auto [mask_ec, mask_size] = co_await boost::capy::read(socket_, boost::capy::mutable_buffer(mask.data(), mask.size()));
        if (mask_ec) { co_return boost::capy::io_result<raw_frame>{mask_ec, {}}; }
        raw_frame frame{static_cast<unsigned char>(header[0] & 0x0f), std::string(header[1] & 0x7f, '\0')};
        auto [body_ec, body_size] = co_await boost::capy::read(socket_, boost::capy::mutable_buffer(frame.payload.data(), frame.payload.size()));
        if (body_ec) { co_return boost::capy::io_result<raw_frame>{body_ec, {}}; }
        for (std::size_t i = 0; i < frame.payload.size(); ++i) { frame.payload[i] ^= mask[i % mask.size()]; }
        co_return boost::capy::io_result<raw_frame>{std::error_code{}, std::move(frame)};
    }

    boost::capy::io_task<> unrelated_traffic()
    {
        for (;;)
        {
            auto [delay_ec] = co_await boost::corosio::delay(10ms);
            if (delay_ec) { co_return delay_ec; }
            auto [ec] = co_await write_frame(1, "unrelated");
            if (ec) { co_return ec; }
        }
    }

    boost::capy::io_task<> controls(std::size_t index)
    {
        auto& state = states_[index];
        auto const mode = behaviors[index];
        for (;;)
        {
            auto [ec, frame] = co_await read_frame();
            if (ec) { co_return ec; }
            if ((mode == behavior::batch || mode == behavior::ack_before_timeout) && frame.opcode == 1)
            {
                if (++state.requests == 80 && mode == behavior::batch)
                {
                    auto [ack_ec] = co_await write_frame(1, "batch-finished");
                    if (ack_ec) { co_return ack_ec; }
                }
                continue;
            }
            if (frame.opcode != 9 || frame.payload.size() != 8)
            { co_return std::make_error_code(std::errc::protocol_error); }
            ++state.pings;
            if (mode == behavior::ack_before_timeout)
            {
                auto [ack_ec] = co_await write_frame(1, "received-ack");
                if (ack_ec) { co_return ack_ec; }
                continue;
            }
            if (mode == behavior::matching || mode == behavior::partial || mode == behavior::batch)
            {
                if (mode == behavior::partial && state.pings == 1)
                {
                    auto [tail_ec] = co_await write_bytes(std::string("ello"));
                    if (tail_ec) { co_return tail_ec; }
                }
                if (mode == behavior::batch)
                {
                    auto [business_ec] = co_await write_frame(1, "queued-pong");
                    if (business_ec) { co_return business_ec; }
                    auto [delay_ec] = co_await boost::corosio::delay(10ms);
                    if (delay_ec) { co_return delay_ec; }
                }
                auto [pong_ec] = co_await write_frame(10, frame.payload);
                if (pong_ec) { co_return pong_ec; }
                if (mode != behavior::batch && state.pings == 3)
                {
                    co_return co_await write_frame(8, "");
                }
            }
            else if (mode == behavior::wrong)
            {
                auto [pong_ec] = co_await write_frame(10, "wrong");
                if (pong_ec) { co_return pong_ec; }
            }
        }
    }

    boost::capy::task<> session()
    {
        parser_.reset();
        parser_.start();
        auto [ec] = co_await parser_.read_header(socket_);
        if (ec) { socket_.close(); co_return; }
        auto target = parser_.get().target();
        if (target.size() != 2 || target[0] != '/' || target[1] < '0' || target[1] > '6')
        { socket_.close(); co_return; }
        auto const index = static_cast<std::size_t>(target[1] - '0');
        auto const key = parser_.get().value_or(boost::http::field::sec_websocket_key, "");
        std::string input(key.data(), key.size());
        input += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::array<unsigned char, 20> digest{};
        unsigned int digest_size = 0;
        std::array<unsigned char, 32> accept{};
        if (EVP_Digest(input.data(), input.size(), digest.data(), &digest_size, EVP_sha1(), nullptr) != 1)
        { socket_.close(); co_return; }
        auto const accept_size = EVP_EncodeBlock(accept.data(), digest.data(), static_cast<int>(digest_size));
        std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
        response.append(reinterpret_cast<char const*>(accept.data()), static_cast<std::size_t>(accept_size));
        response += "\r\n\r\n";
        auto [write_ec] = co_await write_bytes(response);
        if (write_ec) { socket_.close(); co_return; }
        // Start frames only after the SDK has accepted Upgrade, so HTTP buffering does not
        // decide this transport-liveness test. Both sides run on the same io_context.
        while (!states_[index].upgraded)
        {
            auto [wait_ec] = co_await boost::corosio::delay(1ms);
            if (wait_ec) { socket_.close(); co_return; }
        }
        if (behaviors[index] == behavior::partial)
        {
            auto [prefix_ec] = co_await write_bytes(std::string("\x81\x05h", 3));
            if (prefix_ec) { socket_.close(); co_return; }
        }
        if (behaviors[index] == behavior::traffic)
        {
            auto result = co_await boost::capy::when_all(controls(index), unrelated_traffic());
            (void)result;
        }
        else
        {
            auto result = co_await controls(index);
            (void)result;
        }
        socket_.close();
    }

    boost::corosio::io_context& context_;
    boost::corosio::tcp_socket socket_;
    std::array<case_state, 7>& states_;
    boost::http::request_parser parser_;
};

boost::capy::io_task<> interrupt_reads(chat::detail::websocket_client& socket)
{
    for (int i = 0; i < 4; ++i)
    {
        auto [ec] = co_await boost::corosio::delay(5ms);
        if (ec) { co_return ec; }
        socket.interrupt_receive();
    }
    co_return {};
}

boost::capy::io_task<> receive_case(chat::detail::websocket_client& socket, case_state& state, behavior mode)
{
    bool unrelated = false;
    for (;;)
    {
        auto [ec, message] = co_await boost::corosio::timeout(socket.receive(), 1s);
        if (ec == std::errc::interrupted) { ++state.interrupts; continue; }
        if (ec)
        {
            state.passed = ec == boost::capy::cond::timeout && state.pings == 1 &&
                (mode == behavior::silent || mode == behavior::wrong || (mode == behavior::traffic && unrelated));
            break;
        }
        if (message.message_type == chat::detail::websocket_message::type::close)
        {
            state.passed = state.pings == 3 && (mode == behavior::matching || (mode == behavior::partial && state.hello));
            break;
        }
        state.hello |= message.payload == "hello";
        unrelated |= message.payload == "unrelated";
    }
    co_return {};
}

boost::capy::io_task<> interrupt_batch_reads(chat::detail::websocket_client& socket, case_state& state)
{
    while (!state.batch_finished)
    {
        auto [ec] = co_await boost::corosio::delay(5ms);
        if (ec) { co_return ec; }
        socket.interrupt_receive();
    }
    co_return {};
}

boost::capy::io_task<> send_batch(chat::detail::websocket_client& socket, case_state& state)
{
    for (int i = 0; i < 80; ++i)
    {
        auto [delay_ec] = co_await boost::corosio::delay(2ms);
        if (delay_ec) { state.batch_finished = true; co_return delay_ec; }
        auto [ec] = co_await socket.send_text("outgoing");
        if (ec) { state.batch_finished = true; co_return ec; }
    }
    int retained_messages = 0;
    for (;;)
    {
        auto [ec, message] = co_await boost::corosio::timeout(socket.receive(), 1s);
        if (ec == std::errc::interrupted) { continue; }
        if (ec) { state.batch_finished = true; co_return ec; }
        if (message.payload == "queued-pong") { ++retained_messages; }
        if (message.payload == "batch-finished")
        {
            state.passed = state.requests == 80 && state.pings >= 3 && retained_messages == state.pings;
            state.batch_finished = true;
            co_return {};
        }
    }
}

boost::capy::io_task<> ack_before_timeout(chat::detail::websocket_client& socket, case_state& state)
{
    for (int i = 0; i < 80; ++i)
    {
        auto [delay_ec] = co_await boost::corosio::delay(2ms);
        if (delay_ec) { state.batch_finished = true; co_return delay_ec; }
        auto [ec] = co_await socket.send_text("outgoing");
        if (ec)
        {
            auto ack = socket.take_pending_message();
            state.passed = ec == boost::capy::cond::timeout && state.pings == 1 && ack &&
                ack->message_type == chat::detail::websocket_message::type::text && ack->payload == "received-ack" &&
                !socket.take_pending_message();
            auto [fatal_ec, ignored] = co_await socket.receive();
            state.passed &= fatal_ec == boost::capy::cond::timeout;
            state.batch_finished = true;
            co_return {};
        }
    }
    state.batch_finished = true;
    co_return {};
}

boost::capy::task<> run_case(boost::corosio::io_context& context, boost::corosio::tcp_server& server,
                           std::array<case_state, 7>& states, std::size_t index, unsigned short port, int& finished)
{
    chat::detail::websocket_client socket(context, {40ms, 80ms});
    auto const url = "ws://127.0.0.1:" + std::to_string(port) + "/" + std::to_string(index);
    auto [ec] = co_await boost::corosio::timeout(socket.connect(url), 1s);
    if (!ec)
    {
        states[index].upgraded = true;
        if (behaviors[index] == behavior::ack_before_timeout)
        {
            auto result = co_await boost::capy::when_all(ack_before_timeout(socket, states[index]), interrupt_batch_reads(socket, states[index]));
            (void)result;
        }
        else if (behaviors[index] == behavior::batch)
        {
            auto result = co_await boost::capy::when_all(send_batch(socket, states[index]), interrupt_batch_reads(socket, states[index]));
            (void)result;
        }
        else
        {
            auto result = co_await boost::capy::when_all(receive_case(socket, states[index], behaviors[index]), interrupt_reads(socket));
            (void)result;
            states[index].passed &= states[index].interrupts == 4;
        }
    }
    socket.close();
    if (++finished == 7) { server.stop(); }
}
}

int run_client_heartbeat_tests()
{
    if (!check_heartbeat_clock()) { std::cerr << "FAIL client heartbeat monotonic probe and late pong\n"; return 1; }
    boost::corosio::io_context context;
    std::array<case_state, 7> states{};
    boost::corosio::tcp_server server(context, context.get_executor());
    std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> workers;
    for (int i = 0; i < 7; ++i) { workers.push_back(std::make_unique<heartbeat_worker>(context, states)); }
    server.set_workers(std::move(workers));
    if (auto ec = server.bind(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), 0)))
    { std::cerr << "FAIL client heartbeat fixture bind: " << ec.message() << '\n'; return 1; }
    auto const port = server.local_endpoint().port();
    server.start();
    int finished = 0;
    for (std::size_t i = 0; i < states.size(); ++i)
    { boost::capy::run_async(context.get_executor())(run_case(context, server, states, i, port, finished)); }
    context.run();
    server.join();
    for (std::size_t i = 0; i < states.size(); ++i)
    {
        if (!states[i].passed) { std::cerr << "FAIL client heartbeat " << names[i] << '\n'; return 1; }
    }
    std::cout << "PASS client heartbeat matching probes, silence, wrong pong, unrelated traffic and partial reads with interrupt and busy outgoing queue and ACK before timeout\n";
    return 0;
}
