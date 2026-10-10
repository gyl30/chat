#include "websocket.hpp"

#include <chat/server_url.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/write.hpp>
#include <boost/http/config.hpp>
#include <boost/http/field.hpp>
#include <boost/http/response_parser.hpp>
#include <boost/http/status.hpp>
#include <boost/corosio/socket_option.hpp>

namespace chat::detail
{

namespace
{

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
// History pages and conversation summaries combine many individually bounded messages.
constexpr std::uint64_t kMaxWebSocketMessageSize = 8 * 1024 * 1024;

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

std::error_code websocket_protocol_error() { return std::make_error_code(std::errc::protocol_error); }

bool make_client_key(std::string& key)
{
    std::array<unsigned char, 16> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
    {
        return false;
    }

    std::array<unsigned char, 32> encoded{};
    auto const size = EVP_EncodeBlock(encoded.data(), nonce.data(), static_cast<int>(nonce.size()));
    if (size <= 0)
    {
        return false;
    }

    key.assign(reinterpret_cast<char const*>(encoded.data()), static_cast<std::size_t>(size));
    return true;
}

bool make_accept(std::string_view key, std::string& accept)
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

}    // namespace

void websocket_client::context_deleter::operator()(wslay_event_context* context) const noexcept { wslay_event_context_free(context); }

websocket_client::websocket_client(boost::corosio::io_context& io_context) : resolver_(io_context), socket_(io_context) {}

boost::capy::io_task<> websocket_client::connect(std::string_view url)
{
    close();
    interrupt_requested_ = false;
    reading_ = false;
    messages_.clear();

    auto parsed = parse_server_url(url);
    if (!parsed)
    {
        co_return std::make_error_code(std::errc::invalid_argument);
    }

    auto const& host = parsed->host;
    auto service = parsed->port.empty() ? std::string(parsed->tls ? "443" : "80") : parsed->port;
    auto target = parsed->target.empty() ? std::string("/") : parsed->target;
    auto const& host_header = parsed->host_header;

    auto [resolve_ec, endpoints] = co_await resolver_.resolve(host, service);
    if (resolve_ec)
    {
        co_return resolve_ec;
    }

    std::error_code connect_ec = std::make_error_code(std::errc::host_unreachable);
    for (auto const& endpoint : endpoints)
    {
        auto [ec] = co_await socket_.connect(endpoint);
        if (!ec)
        {
            connect_ec.clear();
            break;
        }
        connect_ec = ec;
        socket_.close();
    }
    if (connect_ec)
    {
        co_return connect_ec;
    }

    try
    {
        socket_.set_option(boost::corosio::socket_option::no_delay(true));
    }
    catch (std::system_error const& error)
    {
        socket_.close();
        co_return error.code();
    }

    if (parsed->tls)
    {
        boost::corosio::tls_context tls_context;
        if (auto ec = tls_context.set_default_verify_paths()) { co_return ec; }
        if (auto ec = tls_context.set_verify_mode(boost::corosio::tls_verify_mode::peer)) { co_return ec; }
        if (auto ec = tls_context.set_min_protocol_version(boost::corosio::tls_version::tls_1_2)) { co_return ec; }
        if (auto ec = tls_context.set_alpn({"http/1.1"})) { co_return ec; }
        tls_ = std::make_unique<boost::corosio::openssl_stream>(&socket_, tls_context);
        tls_->set_hostname(host);
        auto [tls_ec] = co_await tls_->handshake(boost::corosio::tls_role::client);
        if (tls_ec)
        {
            socket_.close();
            co_return tls_ec;
        }
        stream_ = boost::capy::any_stream(tls_.get());
    }
    else
    {
        stream_ = boost::capy::any_stream(&socket_);
    }

    auto [handshake_ec] = co_await handshake(target, host_header);
    if (handshake_ec)
    {
        socket_.close();
        co_return handshake_ec;
    }

    wslay_event_callbacks callbacks{};
    callbacks.recv_callback = &receive_callback;
    callbacks.genmask_callback = &genmask_callback;
    callbacks.on_msg_recv_callback = &message_callback;

    wslay_event_context_ptr context = nullptr;
    if (wslay_event_context_client_init(&context, &callbacks, this) != 0)
    {
        socket_.close();
        co_return std::make_error_code(std::errc::not_enough_memory);
    }

    wslay_event_config_set_max_recv_msg_length(context, kMaxWebSocketMessageSize);
    context_.reset(context);
    co_return {};
}

boost::capy::io_task<> websocket_client::handshake(std::string_view target, std::string_view host_header)
{
    std::string key;
    std::string expected_accept;
    if (!make_client_key(key) || !make_accept(key, expected_accept))
    {
        co_return std::make_error_code(std::errc::io_error);
    }

    std::string request = "GET ";
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host_header);
    request.append("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ");
    request.append(key);
    request.append("\r\nSec-WebSocket-Version: 13\r\n\r\n");

    auto [write_ec, written] = co_await boost::capy::write(stream_, boost::capy::const_buffer(request.data(), request.size()));
    if (write_ec)
    {
        co_return write_ec;
    }
    if (written != request.size())
    {
        co_return std::make_error_code(std::errc::io_error);
    }

    auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
    boost::http::response_parser parser(parser_config);
    parser.reset();
    parser.start();

    auto [read_ec] = co_await parser.read(stream_);
    if (read_ec)
    {
        co_return read_ec;
    }

    auto const& response = parser.get();
    auto const connection = as_string_view(response.value_or(boost::http::field::connection, ""));
    auto const upgrade = as_string_view(response.value_or(boost::http::field::upgrade, ""));
    auto const accept = as_string_view(response.value_or(boost::http::field::sec_websocket_accept, ""));
    if (response.status() != boost::http::status::switching_protocols || !contains_token(connection, "upgrade") ||
        !contains_token(upgrade, "websocket") || accept != expected_accept || parser.has_buffered_data())
    {
        co_return websocket_protocol_error();
    }

    co_return {};
}

boost::capy::io_task<websocket_message> websocket_client::receive()
{
    for (;;)
    {
        if (!messages_.empty())
        {
            auto message = std::move(messages_.front());
            messages_.pop_front();
            co_return boost::capy::io_result<websocket_message>{std::error_code{}, std::move(message)};
        }

        if (interrupt_requested_)
        {
            interrupt_requested_ = false;
            co_return boost::capy::io_result<websocket_message>{std::make_error_code(std::errc::interrupted), {}};
        }

        if (!context_)
        {
            co_return boost::capy::io_result<websocket_message>{std::make_error_code(std::errc::not_connected), {}};
        }

        if (wslay_event_want_read(context_.get()) == 0)
        {
            co_return boost::capy::io_result<websocket_message>{boost::capy::make_error_code(boost::capy::error::eof), {}};
        }

        reading_ = true;
        auto [ec, size] = co_await stream_.read_some(boost::capy::mutable_buffer(input_buffer_.data(), input_buffer_.size()));
        reading_ = false;
        if (ec)
        {
            if (interrupt_requested_ && ec == boost::capy::cond::canceled)
            {
                interrupt_requested_ = false;
                co_return boost::capy::io_result<websocket_message>{std::make_error_code(std::errc::interrupted), {}};
            }
            co_return boost::capy::io_result<websocket_message>{ec, {}};
        }
        if (size == 0)
        {
            co_return boost::capy::io_result<websocket_message>{std::make_error_code(std::errc::connection_reset), {}};
        }

        input_ = std::span<std::uint8_t const>(input_buffer_.data(), size);
        if (wslay_event_recv(context_.get()) != 0 || (!input_.empty() && wslay_event_want_read(context_.get()) != 0))
        {
            input_ = {};
            co_return boost::capy::io_result<websocket_message>{websocket_protocol_error(), {}};
        }
        input_ = {};

        auto [flush_ec] = co_await flush();
        if (flush_ec)
        {
            co_return boost::capy::io_result<websocket_message>{flush_ec, {}};
        }
    }
}

void websocket_client::interrupt_receive() noexcept
{
    interrupt_requested_ = true;
    if (reading_)
    {
        socket_.cancel();
    }
}

boost::capy::io_task<> websocket_client::send_text(std::string_view payload)
{
    if (!context_)
    {
        co_return std::make_error_code(std::errc::not_connected);
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

boost::capy::io_task<> websocket_client::flush()
{
    std::array<std::uint8_t, 4096> output{};

    while (wslay_event_want_write(context_.get()) != 0)
    {
        auto const result = wslay_event_write(context_.get(), output.data(), output.size());
        if (result <= 0)
        {
            co_return websocket_protocol_error();
        }

        auto const size = static_cast<std::size_t>(result);
        auto [ec, written] = co_await boost::capy::write(stream_, boost::capy::const_buffer(output.data(), size));
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

void websocket_client::cancel() noexcept
{
    resolver_.cancel();
    socket_.close();
}

void websocket_client::close() noexcept
{
    resolver_.cancel();
    socket_.close();
    // cancel() wakes the connection coroutine first; TLS resources are released only after
    // its I/O has returned. The selected stream borrows rather than owns the transport.
    stream_ = boost::capy::any_stream{};
    tls_.reset();
    context_.reset();
    input_ = {};
    messages_.clear();
    reading_ = false;
    interrupt_requested_ = false;
}

ssize_t websocket_client::receive_callback(
    wslay_event_context_ptr context,
    std::uint8_t* buffer,
    std::size_t size,
    int,
    void* user_data)
{
    auto& self = *static_cast<websocket_client*>(user_data);
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

int websocket_client::genmask_callback(wslay_event_context_ptr, std::uint8_t* buffer, std::size_t size, void*)
{
    return RAND_bytes(buffer, static_cast<int>(size)) == 1 ? 0 : -1;
}

void websocket_client::message_callback(wslay_event_context_ptr, wslay_event_on_msg_recv_arg const* message, void* user_data)
{
    auto& self = *static_cast<websocket_client*>(user_data);

    websocket_message result;
    if (message->opcode == WSLAY_TEXT_FRAME)
    {
        result.message_type = websocket_message::type::text;
        if (message->msg_length != 0)
        {
            result.payload.assign(reinterpret_cast<char const*>(message->msg), message->msg_length);
        }
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

}    // namespace chat::detail
