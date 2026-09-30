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

std::optional<std::int64_t> parse_int64(boost::json::value const& value)
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

std::optional<std::uint64_t> parse_uint64(boost::json::value const& value)
{
    if (value.is_uint64())
    {
        return value.as_uint64();
    }
    if (value.is_int64() && value.as_int64() >= 0)
    {
        return static_cast<std::uint64_t>(value.as_int64());
    }
    return std::nullopt;
}

bool parse_user(boost::json::object const& object, user& value)
{
    auto const* id_value = object.if_contains("id");
    auto const* username_value = object.if_contains("username");
    if (!id_value || !username_value || !username_value->is_string())
    {
        return false;
    }

    auto id = parse_int64(*id_value);
    if (!id || *id <= 0)
    {
        return false;
    }

    value.id = *id;
    value.username = std::string(username_value->as_string());
    return true;
}

bool parse_message(boost::json::object const& object, message& value)
{
    auto const* id_value = object.if_contains("id");
    auto const* from_value = object.if_contains("from");
    auto const* timestamp_value = object.if_contains("timestamp");
    auto const* text_value = object.if_contains("text");
    if (!id_value || !from_value || !timestamp_value || !text_value || !text_value->is_string())
    {
        return false;
    }

    auto id = parse_int64(*id_value);
    auto from = parse_int64(*from_value);
    auto timestamp = parse_int64(*timestamp_value);
    if (!id || *id <= 0 || !from || *from <= 0 || !timestamp || *timestamp <= 0)
    {
        return false;
    }

    value.id = *id;
    value.from = *from;
    value.timestamp = *timestamp;
    value.text = std::string(text_value->as_string());
    return true;
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

    void notify_message(message value)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        message_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = message_handler_;
        }
        if (handler)
        {
            handler(std::move(value));
        }
    }

    void notify_read(std::int64_t user, std::int64_t message)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        read_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = read_handler_;
        }
        if (handler)
        {
            handler(user, message);
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

    void receive(std::string const& payload)
    {
        boost::system::error_code ec;
        auto value = boost::json::parse(payload, ec);
        if (ec || !value.is_object())
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC message"));
            return;
        }

        auto const& object = value.as_object();
        auto const* version = object.if_contains("jsonrpc");
        if (!version || !version->is_string() || version->as_string() != "2.0")
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC message"));
            return;
        }

        auto const* id_value = object.if_contains("id");
        if (!id_value)
        {
            auto const* method = object.if_contains("method");
            auto const* params = object.if_contains("params");
            if (!method || !method->is_string() || !params || !params->is_object())
            {
                report_error(make_error(error_kind::protocol, "Invalid JSON-RPC notification"));
                return;
            }

            if (method->as_string() == "message")
            {
                message notification;
                if (!parse_message(params->as_object(), notification))
                {
                    report_error(make_error(error_kind::protocol, "Invalid message notification"));
                    return;
                }
                notify_message(std::move(notification));
                return;
            }

            if (method->as_string() == "read")
            {
                auto const* user_value = params->as_object().if_contains("user");
                auto const* message_value = params->as_object().if_contains("message");
                if (!user_value || !message_value)
                {
                    report_error(make_error(error_kind::protocol, "Invalid read notification"));
                    return;
                }
                auto user = parse_int64(*user_value);
                auto read_message = parse_int64(*message_value);
                if (!user || *user <= 0 || !read_message || *read_message <= 0)
                {
                    report_error(make_error(error_kind::protocol, "Invalid read notification"));
                    return;
                }
                notify_read(*user, *read_message);
                return;
            }

            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC notification"));
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

    boost::capy::task<> register_user(std::string username, std::string password, register_handler handler)
    {
        boost::json::object params;
        params.emplace("username", std::move(username));
        params.emplace("password", std::move(password));

        send_request("register", std::move(params), [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid register result")));
                return;
            }

            auto const* user_value = response->as_object().if_contains("user");
            if (!user_value)
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid register result")));
                return;
            }

            auto user = parse_int64(*user_value);
            if (!user || *user <= 0)
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid register result")));
                return;
            }
            handler(*user);
        });

        co_return;
    }

    boost::capy::task<> get_conversations(std::optional<std::int64_t> before, conversations_handler handler)
    {
        boost::json::object params;
        if (before)
        {
            params.emplace("before", *before);
        }

        send_request("get_conversations", std::move(params), [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_conversations result")));
                return;
            }

            auto const* conversations_value = response->as_object().if_contains("conversations");
            if (!conversations_value || !conversations_value->is_array())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_conversations result")));
                return;
            }

            std::vector<conversation> conversations;
            conversations.reserve(conversations_value->as_array().size());
            for (auto const& value : conversations_value->as_array())
            {
                if (!value.is_object())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation")));
                    return;
                }

                auto const& object = value.as_object();
                auto const* user_value = object.if_contains("user");
                auto const* username_value = object.if_contains("username");
                auto const* last_value = object.if_contains("last");
                auto const* unread_value = object.if_contains("unread");
                if (!user_value || !username_value || !username_value->is_string() || !last_value || !last_value->is_object() || !unread_value)
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation")));
                    return;
                }

                auto user = parse_int64(*user_value);
                auto unread = parse_uint64(*unread_value);
                auto const& last_object = last_value->as_object();
                auto const* id_value = last_object.if_contains("id");
                auto const* from_value = last_object.if_contains("from");
                auto const* timestamp_value = last_object.if_contains("timestamp");
                auto const* text_value = last_object.if_contains("text");
                if (!user || *user <= 0 || !unread || !id_value || !from_value || !timestamp_value || !text_value ||
                    !text_value->is_string())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation")));
                    return;
                }

                auto id = parse_int64(*id_value);
                auto from = parse_int64(*from_value);
                auto timestamp = parse_int64(*timestamp_value);
                if (!id || *id <= 0 || !from || *from <= 0 || !timestamp || *timestamp <= 0)
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation")));
                    return;
                }

                conversation item;
                item.user = *user;
                item.username = std::string(username_value->as_string());
                item.last.id = *id;
                item.last.from = *from;
                item.last.timestamp = *timestamp;
                item.last.text = std::string(text_value->as_string());
                item.unread = *unread;
                conversations.push_back(std::move(item));
            }

            handler(std::move(conversations));
        });

        co_return;
    }

    boost::capy::task<> get_contacts(users_handler handler)
    {
        send_request("get_contacts", {}, [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_contacts result")));
                return;
            }

            auto const* users_value = response->as_object().if_contains("users");
            if (!users_value || !users_value->is_array())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_contacts result")));
                return;
            }

            std::vector<user> users;
            users.reserve(users_value->as_array().size());
            for (auto const& value : users_value->as_array())
            {
                if (!value.is_object())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid user")));
                    return;
                }

                user item;
                if (!parse_user(value.as_object(), item))
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid user")));
                    return;
                }
                users.push_back(std::move(item));
            }

            handler(std::move(users));
        });

        co_return;
    }

    boost::capy::task<> get_messages(std::int64_t user, std::optional<std::int64_t> before, messages_handler handler)
    {
        boost::json::object params;
        params.emplace("user", user);
        if (before)
        {
            params.emplace("before", *before);
        }

        send_request("get_messages", std::move(params), [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_messages result")));
                return;
            }

            auto const* messages_value = response->as_object().if_contains("messages");
            auto const* read_value = response->as_object().if_contains("read");
            if (!messages_value || !messages_value->is_array() || !read_value)
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_messages result")));
                return;
            }

            auto read_message = parse_int64(*read_value);
            if (!read_message || *read_message < 0)
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_messages result")));
                return;
            }

            messages_result result;
            result.messages.reserve(messages_value->as_array().size());
            for (auto const& value : messages_value->as_array())
            {
                if (!value.is_object())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid message")));
                    return;
                }

                message item;
                if (!parse_message(value.as_object(), item))
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid message")));
                    return;
                }
                result.messages.push_back(std::move(item));
            }
            result.read_message = *read_message;
            handler(std::move(result));
        });

        co_return;
    }

    boost::capy::task<> send_message(std::int64_t user, std::string text, send_message_handler handler)
    {
        boost::json::object params;
        params.emplace("user", user);
        params.emplace("text", std::move(text));

        send_request("send_message", std::move(params), [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid send_message result")));
                return;
            }

            auto const& object = response->as_object();
            auto const* message_value = object.if_contains("message");
            auto const* timestamp_value = object.if_contains("timestamp");
            auto const* realtime_value = object.if_contains("realtime");
            if (!message_value || !timestamp_value || !realtime_value || !realtime_value->is_bool())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid send_message result")));
                return;
            }

            auto message_id = parse_int64(*message_value);
            auto timestamp = parse_int64(*timestamp_value);
            if (!message_id || *message_id <= 0 || !timestamp || *timestamp <= 0)
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid send_message result")));
                return;
            }

            send_message_result result;
            result.message_id = *message_id;
            result.timestamp = *timestamp;
            result.realtime = realtime_value->as_bool();
            handler(result);
        });

        co_return;
    }

    boost::capy::task<> search_users(std::string query, users_handler handler)
    {
        boost::json::object params;
        params.emplace("query", std::move(query));

        send_request("search_users", std::move(params), [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid search_users result")));
                return;
            }

            auto const* users_value = response->as_object().if_contains("users");
            if (!users_value || !users_value->is_array())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid search_users result")));
                return;
            }

            std::vector<user> users;
            users.reserve(users_value->as_array().size());
            for (auto const& value : users_value->as_array())
            {
                if (!value.is_object())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid user")));
                    return;
                }

                user item;
                if (!parse_user(value.as_object(), item))
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid user")));
                    return;
                }
                users.push_back(std::move(item));
            }

            handler(std::move(users));
        });

        co_return;
    }

    boost::capy::task<> add_contact(std::int64_t contact, user_handler handler)
    {
        boost::json::object params;
        params.emplace("user", contact);

        send_request("add_contact", std::move(params), [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid add_contact result")));
                return;
            }

            auto const* user_value = response->as_object().if_contains("user");
            if (!user_value || !user_value->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid add_contact result")));
                return;
            }

            user value;
            if (!parse_user(user_value->as_object(), value))
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid user")));
                return;
            }

            handler(std::move(value));
        });

        co_return;
    }

    boost::capy::task<> mark_read(std::int64_t user, std::int64_t message_id, mark_read_handler handler)
    {
        boost::json::object params;
        params.emplace("user", user);
        params.emplace("message", message_id);

        send_request("mark_read", std::move(params), [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid mark_read result")));
                return;
            }

            auto const* message_value = response->as_object().if_contains("message");
            if (!message_value)
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid mark_read result")));
                return;
            }

            auto read_message = parse_int64(*message_value);
            if (!read_message || *read_message <= 0)
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid mark_read result")));
                return;
            }
            handler(*read_message);
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
    message_handler message_handler_;
    read_handler read_handler_;
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

void client::set_message_handler(message_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->message_handler_ = std::move(handler);
}

void client::set_read_handler(read_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->read_handler_ = std::move(handler);
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

void client::register_user(std::string username, std::string password, register_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->register_user(std::move(username), std::move(password), std::move(handler)));
}

void client::get_conversations(std::optional<std::int64_t> before, conversations_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_conversations(before, std::move(handler)));
}

void client::get_contacts(users_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_contacts(std::move(handler)));
}

void client::get_messages(std::int64_t user, std::optional<std::int64_t> before, messages_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_messages(user, before, std::move(handler)));
}

void client::send_message(std::int64_t user, std::string text, send_message_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->send_message(user, std::move(text), std::move(handler)));
}

void client::search_users(std::string query, users_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->search_users(std::move(query), std::move(handler)));
}

void client::add_contact(std::int64_t user, user_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->add_contact(user, std::move(handler)));
}

void client::mark_read(std::int64_t user, std::int64_t message, mark_read_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->mark_read(user, message, std::move(handler)));
}

}    // namespace chat
