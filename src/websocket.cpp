#include <cstring>
#include <utility>
#include <algorithm>
#include <system_error>

#include <openssl/evp.h>
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

websocket_connection::websocket_connection(boost::corosio::tcp_socket& socket) : socket_(socket)
{
    wslay_event_callbacks callbacks{};
    callbacks.recv_callback = &receive_callback;
    callbacks.on_msg_recv_callback = &message_callback;

    wslay_event_context_ptr context = nullptr;
    if (wslay_event_context_server_init(&context, &callbacks, this) == 0)
    {
        context_.reset(context);
    }
}

websocket_connection::~websocket_connection() = default;

bool websocket_connection::valid() const noexcept { return context_ != nullptr; }

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

        if (!context_)
        {
            co_return boost::capy::io_result<websocket_message>{std::make_error_code(std::errc::not_connected), {}};
        }

        if (wslay_event_want_read(context_.get()) == 0)
        {
            co_return boost::capy::io_result<websocket_message>{boost::capy::make_error_code(boost::capy::error::eof), {}};
        }

        auto [ec, size] = co_await socket_.read_some(boost::capy::mutable_buffer(input_buffer_.data(), input_buffer_.size()));
        if (ec)
        {
            co_return boost::capy::io_result<websocket_message>{ec, {}};
        }
        if (size == 0)
        {
            co_return boost::capy::io_result<websocket_message>{std::make_error_code(std::errc::connection_reset), {}};
        }

        input_ = std::span<std::uint8_t const>(input_buffer_.data(), size);
        if (wslay_event_recv(context_.get()) != 0 || !input_.empty())
        {
            co_return boost::capy::io_result<websocket_message>{websocket_protocol_error(), {}};
        }

        auto [flush_ec] = co_await flush();
        if (flush_ec)
        {
            co_return boost::capy::io_result<websocket_message>{flush_ec, {}};
        }
    }
}

boost::capy::io_task<> websocket_connection::send_text(std::string_view payload)
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

boost::capy::io_task<> websocket_connection::flush()
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
        auto [ec, written] = co_await boost::capy::write(socket_, boost::capy::const_buffer(output.data(), size));
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
