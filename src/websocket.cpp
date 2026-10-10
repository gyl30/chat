#include <cstdint>
#include <cstring>
#include <utility>
#include <algorithm>
#include <system_error>

#include <openssl/evp.h>
#include <boost/capy/cond.hpp>
#include <boost/corosio/timeout.hpp>
#include <chat/detail/websocket_limits.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/write.hpp>
#include <boost/http/field.hpp>
#include <boost/http/method.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/http/version.hpp>
#include <boost/capy/io_result.hpp>

#include "websocket.hpp"

namespace
{

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
using chat::detail::websocket_heartbeat;

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

bool valid_websocket_key(std::string_view key)
{
    if (key.size() != 24)
    {
        return false;
    }

    std::array<unsigned char, 32> decoded{};
    auto const size = EVP_DecodeBlock(decoded.data(), reinterpret_cast<unsigned char const*>(key.data()), static_cast<int>(key.size()));
    if (size < 0)
    {
        return false;
    }

    int decoded_size = size;
    for (auto it = key.rbegin(); it != key.rend() && *it == '='; ++it)
    {
        --decoded_size;
    }
    return decoded_size == 16;
}

bool make_websocket_accept(std::string_view key, std::string& accept)
{
    struct evp_md_ctx_deleter
    {
        void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
    };

    std::unique_ptr<EVP_MD_CTX, evp_md_ctx_deleter> digest_context(EVP_MD_CTX_new());
    if (!digest_context)
    {
        return false;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestInit_ex(digest_context.get(), EVP_sha1(), nullptr) != 1 || EVP_DigestUpdate(digest_context.get(), key.data(), key.size()) != 1 ||
        EVP_DigestUpdate(digest_context.get(), kWebSocketGuid.data(), kWebSocketGuid.size()) != 1 ||
        EVP_DigestFinal_ex(digest_context.get(), digest.data(), &digest_size) != 1)
    {
        return false;
    }

    std::array<unsigned char, 64> encoded{};
    auto const encoded_size = EVP_EncodeBlock(encoded.data(), digest.data(), static_cast<int>(digest_size));
    if (encoded_size <= 0)
    {
        return false;
    }

    accept.assign(reinterpret_cast<char const*>(encoded.data()), static_cast<std::size_t>(encoded_size));
    return true;
}

std::error_code websocket_protocol_error() { return std::make_error_code(std::errc::protocol_error); }

}    // namespace

bool websocket_upgrade_accept(boost::http::request_base const& request, std::string& accept)
{
    if (request.method() != boost::http::method::get || request.version() != boost::http::version::http_1_1 ||
        !request.exists(boost::http::field::host))
    {
        return false;
    }

    auto const connection = as_string_view(request.value_or(boost::http::field::connection, ""));
    auto const upgrade = as_string_view(request.value_or(boost::http::field::upgrade, ""));
    auto const version = trim(as_string_view(request.value_or(boost::http::field::sec_websocket_version, "")));
    auto const key = trim(as_string_view(request.value_or(boost::http::field::sec_websocket_key, "")));

    return contains_token(connection, "upgrade") && contains_token(upgrade, "websocket") && version == "13" && valid_websocket_key(key) &&
           make_websocket_accept(key, accept);
}

void websocket_connection::context_deleter::operator()(wslay_event_context* context) const noexcept { wslay_event_context_free(context); }

websocket_connection::websocket_connection(boost::corosio::tcp_socket& socket)
    : websocket_connection(socket, boost::capy::any_stream(&socket))
{
}

websocket_connection::websocket_connection(boost::corosio::tcp_socket& socket, boost::capy::any_stream stream,
                                             chat::detail::websocket_heartbeat_config heartbeat)
    : socket_(socket), stream_(std::move(stream)), heartbeat_(heartbeat)
{
    heartbeat_.reset(chat::detail::websocket_heartbeat::clock::now());
    wslay_event_callbacks callbacks{};
    callbacks.recv_callback = &receive_callback;
    callbacks.on_msg_recv_callback = &message_callback;

    wslay_event_context_ptr context = nullptr;
    if (wslay_event_context_server_init(&context, &callbacks, this) == 0)
    {
        wslay_event_config_set_max_recv_msg_length(context, chat::detail::max_websocket_request_size);
        context_.reset(context);
    }
}

boost::capy::io_task<websocket_message> websocket_connection::receive()
{
    for (;;)
    {
        if (!messages_.empty())
        {
            auto message = std::move(messages_.front());
            messages_.pop_front();
            co_return boost::capy::io_result<websocket_message>{std::error_code{}, std::move(message)};
        }
        if (deferred_read_error_)
        {
            co_return boost::capy::io_result<websocket_message>{*deferred_read_error_, {}};
        }
        if (interrupt_requested_ && !heartbeat_.pending())
        {
            interrupt_requested_ = false;
            co_return boost::capy::io_result<websocket_message>{std::make_error_code(std::errc::interrupted), {}};
        }
        auto [ec] = co_await read_input();
        if (ec) { deferred_read_error_ = ec; }
    }
}

boost::capy::io_task<> websocket_connection::read_input()
{
    if (!context_) { co_return std::make_error_code(std::errc::not_connected); }
    if (deferred_read_error_) { co_return *deferred_read_error_; }
    if (wslay_event_want_read(context_.get()) == 0)
    {
        co_return boost::capy::make_error_code(boost::capy::error::eof);
    }

    if (auto ec = poll_heartbeat())
    {
        co_return ec;
    }
    if (wslay_event_want_write(context_.get()) != 0)
    {
        auto [ec] = co_await flush();
        if (ec) { co_return ec; }
    }

    // timeout() discards the canceled operation's payload. Keep any bytes that the
    // transport actually produced, including when a TLS read completes during cancellation.
    std::size_t size = 0;
    reading_ = true;
    auto [ec] = co_await boost::corosio::timeout(read_once(size), heartbeat_.deadline());
    reading_ = false;
    if (size != 0)
    {
        input_ = std::span<std::uint8_t const>(input_buffer_.data(), size);
        if (wslay_event_recv(context_.get()) != 0 || (!input_.empty() && wslay_event_want_read(context_.get()) != 0))
        {
            input_ = {};
            co_return websocket_protocol_error();
        }
        input_ = {};
    }

    auto const interrupted = interrupt_requested_ && ec == boost::capy::cond::canceled;
    if (heartbeat_.expired(websocket_heartbeat::clock::now()))
    {
        deferred_read_error_ = boost::capy::make_error_code(boost::capy::error::timeout);
    }
    else if (ec && ec != boost::capy::cond::timeout && !interrupted)
    {
        deferred_read_error_ = ec;
    }
    else if (!ec && size == 0)
    {
        deferred_read_error_ = std::make_error_code(std::errc::connection_reset);
    }
    if (deferred_read_error_)
    {
        // Deliver complete messages already parsed above, then return the retained
        // transport error on the next receive; send_text also refuses further writes.
        co_return *deferred_read_error_;
    }

    auto [flush_ec] = co_await flush();
    if (flush_ec)
    {
        co_return flush_ec;
    }
    if (wslay_event_get_close_received(context_.get()) != 0)
    {
        co_return boost::capy::make_error_code(boost::capy::error::eof);
    }
    co_return {};
}

boost::capy::io_task<> websocket_connection::read_once(std::size_t& transferred)
{
    auto [ec, size] = co_await stream_.read_some(boost::capy::mutable_buffer(input_buffer_.data(), input_buffer_.size()));
    transferred = size;
    co_return ec;
}

std::error_code websocket_connection::poll_heartbeat()
{
    auto const now = websocket_heartbeat::clock::now();
    if (heartbeat_.expired(now)) { return boost::capy::make_error_code(boost::capy::error::timeout); }
    if (heartbeat_.probe_due(now))
    {
        auto const token = heartbeat_.start_probe(now);
        wslay_event_msg message{};
        message.opcode = WSLAY_PING;
        message.msg = token.data();
        message.msg_length = token.size();
        if (wslay_event_queue_msg(context_.get(), &message) != 0) { return websocket_protocol_error(); }
    }
    return {};
}

void websocket_connection::interrupt_receive() noexcept
{
    interrupt_requested_ = true;
    if (reading_ && !heartbeat_.pending())
    {
        socket_.cancel();
    }
}

boost::capy::io_task<> websocket_connection::send_text(std::string_view payload)
{
    if (!context_)
    {
        co_return std::make_error_code(std::errc::not_connected);
    }

    if (deferred_read_error_) { co_return *deferred_read_error_; }
    if (auto ec = poll_heartbeat()) { co_return ec; }
    if (heartbeat_.pending())
    {
        auto [flush_ec] = co_await flush();
        if (flush_ec) { co_return flush_ec; }
        // A busy application's outgoing queue must not prevent a matching pong from
        // being read. Pump controls here; business messages remain in messages_.
        while (heartbeat_.pending())
        {
            auto [ec] = co_await read_input();
            if (ec)
            {
                deferred_read_error_ = ec;
                co_return ec;
            }
        }
    }

    wslay_event_msg message{};
    message.opcode = WSLAY_TEXT_FRAME;
    message.msg = reinterpret_cast<std::uint8_t const*>(payload.data());
    message.msg_length = payload.size();
    if (wslay_event_queue_msg(context_.get(), &message) != 0)
    {
        co_return websocket_protocol_error();
    }

    co_return co_await flush();
}

boost::capy::io_task<> websocket_connection::flush()
{
    if (auto ec = poll_heartbeat()) { co_return ec; }
    auto const now = websocket_heartbeat::clock::now();
    auto const write_deadline = heartbeat_.pending()
        ? std::min(heartbeat_.deadline(), now + heartbeat_.grace())
        : now + heartbeat_.grace();
    std::array<std::uint8_t, 4096> output{};

    while (wslay_event_want_write(context_.get()) != 0)
    {
        if (websocket_heartbeat::clock::now() >= write_deadline)
        {
            co_return boost::capy::make_error_code(boost::capy::error::timeout);
        }
        auto const result = wslay_event_write(context_.get(), output.data(), output.size());
        if (result <= 0)
        {
            co_return websocket_protocol_error();
        }

        auto const size = static_cast<std::size_t>(result);
        auto [ec, written] = co_await boost::corosio::timeout(
            boost::capy::write(stream_, boost::capy::const_buffer(output.data(), size)), write_deadline);
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

ssize_t websocket_connection::receive_callback(wslay_event_context_ptr context, std::uint8_t* buffer, std::size_t size, int, void* user_data)
{
    auto& self = *static_cast<websocket_connection*>(user_data);
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

void websocket_connection::message_callback(wslay_event_context_ptr, wslay_event_on_msg_recv_arg const* message, void* user_data)
{
    auto& self = *static_cast<websocket_connection*>(user_data);

    websocket_message result;
    if (message->opcode == WSLAY_TEXT_FRAME)
    {
        result.message_type = websocket_message::type::text;
        if (message->msg_length != 0)
        {
            result.payload.assign(reinterpret_cast<char const*>(message->msg), message->msg_length);
        }
    }
    else if (message->opcode == WSLAY_PONG)
    {
        self.heartbeat_.pong(std::span<std::uint8_t const>(message->msg, message->msg_length), websocket_heartbeat::clock::now());
        return;
    }
    else if (message->opcode == WSLAY_CONNECTION_CLOSE)
    {
        result.message_type = websocket_message::type::close;
    }
    else
    {
        return;
    }

    self.messages_.push_back(std::move(result));
}

void websocket_connection::close() noexcept
{
    socket_.close();
}
