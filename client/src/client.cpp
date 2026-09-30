#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <boost/json.hpp>

#include <chat/client.hpp>

namespace chat
{

namespace
{

using response_handler = std::function<void(std::expected<boost::json::value, error>)>;

error make_error(error_kind kind, std::string message, int code = 0)
{
    error value;
    value.kind = kind;
    value.code = code;
    value.message = std::move(message);
    return value;
}

std::optional<std::int64_t> parse_response_id(boost::json::value const& value)
{
    if (value.is_int64())
    {
        return value.as_int64();
    }
    if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return static_cast<std::int64_t>(value.as_uint64());
    }
    return std::nullopt;
}

}    // namespace

struct client::impl
{
    explicit impl(std::unique_ptr<transport> value) : transport_(std::move(value))
    {
    }

    void report_error(error value)
    {
        if (error_handler_)
        {
            error_handler_(value);
        }
    }

    void fail_pending(error value)
    {
        auto pending = std::move(pending_);
        pending_.clear();
        for (auto& [id, handler] : pending)
        {
            (void)id;
            handler(std::unexpected(value));
        }
    }

    void send_request(std::string method, boost::json::object params, response_handler handler)
    {
        auto const id = next_request_id_++;
        pending_.emplace(id, std::move(handler));

        boost::json::object request;
        request.emplace("jsonrpc", "2.0");
        request.emplace("method", std::move(method));
        request.emplace("params", std::move(params));
        request.emplace("id", id);
        transport_->send(boost::json::serialize(request));
    }

    void receive(std::string const& message)
    {
        boost::system::error_code ec;
        auto value = boost::json::parse(message, ec);
        if (ec || !value.is_object())
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC message"));
            return;
        }

        auto const& object = value.as_object();
        auto const* version = object.if_contains("jsonrpc");
        auto const* id_value = object.if_contains("id");
        if (!version || !version->is_string() || version->as_string() != "2.0" || !id_value)
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC response"));
            return;
        }

        auto id = parse_response_id(*id_value);
        if (!id)
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC response id"));
            return;
        }

        auto pending = pending_.find(*id);
        if (pending == pending_.end())
        {
            report_error(make_error(error_kind::protocol, "Unknown JSON-RPC response id"));
            return;
        }

        auto handler = std::move(pending->second);
        pending_.erase(pending);

        auto const* result = object.if_contains("result");
        auto const* error_value = object.if_contains("error");
        if ((result == nullptr) == (error_value == nullptr))
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Invalid JSON-RPC response")));
            return;
        }

        if (result)
        {
            handler(*result);
            return;
        }

        if (!error_value->is_object())
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Invalid JSON-RPC error")));
            return;
        }

        auto const& error_object = error_value->as_object();
        auto const* code = error_object.if_contains("code");
        auto const* error_message = error_object.if_contains("message");
        if (!code || !code->is_int64() || !error_message || !error_message->is_string())
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Invalid JSON-RPC error")));
            return;
        }

        handler(std::unexpected(make_error(error_kind::rpc,
                                           std::string(error_message->as_string()),
                                           static_cast<int>(code->as_int64()))));
    }

    std::unique_ptr<transport> transport_;
    std::int64_t next_request_id_ = 1;
    std::unordered_map<std::int64_t, response_handler> pending_;
    connection_handler connected_handler_;
    connection_handler disconnected_handler_;
    error_handler error_handler_;
};

client::client(std::unique_ptr<transport> transport) : impl_(std::make_unique<impl>(std::move(transport)))
{
    impl_->transport_->set_listener(this);
}

client::~client()
{
    impl_->transport_->set_listener(nullptr);
}

void client::set_connected_handler(connection_handler handler)
{
    impl_->connected_handler_ = std::move(handler);
}

void client::set_disconnected_handler(connection_handler handler)
{
    impl_->disconnected_handler_ = std::move(handler);
}

void client::set_error_handler(error_handler handler)
{
    impl_->error_handler_ = std::move(handler);
}

void client::open(std::string url)
{
    impl_->transport_->open(std::move(url));
}

void client::close()
{
    impl_->transport_->close();
}

void client::authenticate(std::string username, std::string password, authenticate_handler handler)
{
    boost::json::object params;
    params.emplace("username", std::move(username));
    params.emplace("password", std::move(password));

    impl_->send_request("authenticate", std::move(params), [handler = std::move(handler)](auto response) mutable {
        if (!response)
        {
            handler(std::unexpected(std::move(response.error())));
            return;
        }
        if (!response->is_object())
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Invalid authenticate result")));
            return;
        }

        auto const* authenticated = response->as_object().if_contains("authenticated");
        if (!authenticated || !authenticated->is_bool())
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Invalid authenticate result")));
            return;
        }
        handler(authenticated->as_bool());
    });
}

void client::on_transport_open()
{
    if (impl_->connected_handler_)
    {
        impl_->connected_handler_();
    }
}

void client::on_transport_text(std::string message)
{
    impl_->receive(message);
}

void client::on_transport_close()
{
    impl_->fail_pending(make_error(error_kind::transport, "Connection closed"));
    if (impl_->disconnected_handler_)
    {
        impl_->disconnected_handler_();
    }
}

void client::on_transport_error(std::string message)
{
    auto value = make_error(error_kind::transport, std::move(message));
    impl_->fail_pending(value);
    impl_->report_error(std::move(value));
}

}    // namespace chat
