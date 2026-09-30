#include <atomic>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/work_guard.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/json.hpp>

#include <chat/client.hpp>

#include "websocket.hpp"

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
    enum class connection_state
    {
        disconnected,
        connecting,
        connected,
    };

    impl()
        : websocket_(io_context_),
          work_(boost::capy::make_work_guard(io_context_.get_executor())),
          network_thread_([this] { io_context_.run(); })
    {
    }

    ~impl()
    {
        suppress_callbacks_.store(true);

        std::promise<void> stopped;
        auto stopped_future = stopped.get_future();
        boost::capy::run_async(io_context_.get_executor())(shutdown(&stopped));
        stopped_future.wait();

        work_.reset();
        network_thread_.join();
    }

    void report_error(error value)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        error_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = error_handler_;
        }
        if (handler)
        {
            handler(value);
        }
    }

    void notify_connected()
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        connection_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = connected_handler_;
        }
        if (handler)
        {
            handler();
        }
    }

    void notify_disconnected()
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        connection_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = disconnected_handler_;
        }
        if (handler)
        {
            handler();
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
        if (state_ != connection_state::connected || closing_)
        {
            handler(std::unexpected(make_error(error_kind::transport, "Not connected")));
            return;
        }

        auto const id = next_request_id_++;
        pending_.emplace(id, std::move(handler));

        boost::json::object request;
        request.emplace("jsonrpc", "2.0");
        request.emplace("method", std::move(method));
        request.emplace("params", std::move(params));
        request.emplace("id", id);
        outgoing_.push_back(boost::json::serialize(request));
        websocket_.interrupt_receive();
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

        handler(std::unexpected(make_error(error_kind::rpc, std::string(error_message->as_string()), static_cast<int>(code->as_int64()))));
    }

    boost::capy::task<> open(std::string url)
    {
        if (state_ != connection_state::disconnected)
        {
            report_error(make_error(error_kind::transport, "Connection already open"));
            co_return;
        }

        state_ = connection_state::connecting;
        closing_ = false;

        auto [connect_ec] = co_await websocket_.connect(url);
        if (connect_ec)
        {
            websocket_.close();
            state_ = connection_state::disconnected;
            if (!closing_)
            {
                report_error(make_error(error_kind::transport, connect_ec.message()));
            }
            closing_ = false;
            co_return;
        }

        if (closing_)
        {
            websocket_.close();
            state_ = connection_state::disconnected;
            closing_ = false;
            co_return;
        }

        state_ = connection_state::connected;
        notify_connected();

        std::optional<std::error_code> connection_error;
        for (;;)
        {
            while (!outgoing_.empty() && !closing_)
            {
                auto message = std::move(outgoing_.front());
                outgoing_.pop_front();
                auto [send_ec] = co_await websocket_.send_text(message);
                if (send_ec)
                {
                    connection_error = send_ec;
                    break;
                }
            }

            if (closing_ || connection_error)
            {
                break;
            }

            auto receive_result = co_await websocket_.receive();
            auto& [receive_ec, message] = receive_result;
            if (receive_ec)
            {
                if (receive_ec == std::errc::interrupted)
                {
                    continue;
                }
                connection_error = receive_ec;
                break;
            }
            if (message.message_type == detail::websocket_message::type::close)
            {
                break;
            }
            receive(message.payload);
        }

        websocket_.close();
        outgoing_.clear();
        state_ = connection_state::disconnected;
        fail_pending(make_error(error_kind::transport, "Connection closed"));

        if (connection_error && !closing_)
        {
            report_error(make_error(error_kind::transport, connection_error->message()));
        }
        closing_ = false;
        notify_disconnected();
    }

    boost::capy::task<> close()
    {
        if (state_ != connection_state::disconnected)
        {
            closing_ = true;
            websocket_.cancel();
        }
        co_return;
    }

    boost::capy::task<> shutdown(std::promise<void>* stopped)
    {
        closing_ = true;
        websocket_.cancel();
        stopped->set_value();
        co_return;
    }

    boost::capy::task<> authenticate(std::string username, std::string password, authenticate_handler handler)
    {
        boost::json::object params;
        params.emplace("username", std::move(username));
        params.emplace("password", std::move(password));

        send_request("authenticate", std::move(params), [handler = std::move(handler)](auto response) mutable {
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

        co_return;
    }

    boost::corosio::io_context io_context_{1};
    detail::websocket_client websocket_;
    boost::capy::work_guard<boost::corosio::io_context::executor_type> work_;
    std::thread network_thread_;

    std::int64_t next_request_id_ = 1;
    std::unordered_map<std::int64_t, response_handler> pending_;
    std::deque<std::string> outgoing_;
    connection_state state_ = connection_state::disconnected;
    bool closing_ = false;

    std::mutex handler_mutex_;
    connection_handler connected_handler_;
    connection_handler disconnected_handler_;
    error_handler error_handler_;
    std::atomic_bool suppress_callbacks_ = false;
};

client::client() : impl_(std::make_unique<impl>()) {}

client::~client() = default;

void client::set_connected_handler(connection_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->connected_handler_ = std::move(handler);
}

void client::set_disconnected_handler(connection_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->disconnected_handler_ = std::move(handler);
}

void client::set_error_handler(error_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->error_handler_ = std::move(handler);
}

void client::connect(std::string url)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->open(std::move(url)));
}

void client::close()
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->close());
}

void client::authenticate(std::string username, std::string password, authenticate_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->authenticate(std::move(username), std::move(password), std::move(handler)));
}

}    // namespace chat
