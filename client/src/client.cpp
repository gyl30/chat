#include <atomic>
#include <algorithm>
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
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/work_guard.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/json.hpp>

#include <chat/client.hpp>
#include <chat/detail/base64.hpp>

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

bool parse_deleted(boost::json::object const& object, bool& deleted)
{
    auto const* value = object.if_contains("deleted");
    if (!value)
    {
        return true;
    }
    if (!value->is_bool())
    {
        return false;
    }
    deleted = value->as_bool();
    return true;
}

bool parse_edited_at(boost::json::object const& object, std::optional<std::int64_t>& edited_at)
{
    auto const* value = object.if_contains("edited_at");
    if (!value || value->is_null())
    {
        return true;
    }
    edited_at = parse_int64(*value);
    return edited_at && *edited_at > 0;
}

bool parse_reply(boost::json::object const& object, std::optional<quoted_message>& reply)
{
    auto const* value = object.if_contains("reply");
    if (!value || value->is_null())
    {
        return true;
    }
    if (!value->is_object())
    {
        return false;
    }
    auto const& fields = value->as_object();
    auto const* id = fields.if_contains("id");
    auto const* from = fields.if_contains("from");
    auto const* username = fields.if_contains("username");
    auto const* text = fields.if_contains("text");
    if (!id || !from || !username || !username->is_string() || !text || !text->is_string())
    {
        return false;
    }
    auto parsed_id = parse_int64(*id);
    auto parsed_from = parse_int64(*from);
    if (!parsed_id || *parsed_id <= 0 || !parsed_from || *parsed_from <= 0)
    {
        return false;
    }
    reply = quoted_message{
        *parsed_id, *parsed_from, std::string(username->as_string()), std::string(text->as_string()), {}};
    return parse_edited_at(fields, reply->edited_at) && parse_deleted(fields, reply->deleted);
}

bool parse_message(boost::json::object const& object, message& value)
{
    auto const* id_value = object.if_contains("id");
    auto const* from_value = object.if_contains("from");
    auto const* timestamp_value = object.if_contains("timestamp");
    auto const* text_value = object.if_contains("text");
    auto const* conversation_value = object.if_contains("conversation");
    auto const* username_value = object.if_contains("username");
    if (!conversation_value || !username_value || !username_value->is_string() || !id_value || !from_value ||
        !timestamp_value || !text_value || !text_value->is_string())
    {
        return false;
    }

    auto id = parse_int64(*id_value);
    auto from = parse_int64(*from_value);
    auto timestamp = parse_int64(*timestamp_value);
    auto conversation = parse_int64(*conversation_value);
    if (!conversation || *conversation <= 0 || !id || *id <= 0 || !from || *from <= 0 || !timestamp || *timestamp <= 0)
    {
        return false;
    }

    value.id = *id;
    value.conversation = *conversation;
    value.username = std::string(username_value->as_string());
    value.from = *from;
    value.timestamp = *timestamp;
    value.text = std::string(text_value->as_string());
    auto const* attachment = object.if_contains("attachment");
    if (attachment && !attachment->is_null())
    {
        if (!attachment->is_object())
        {
            return false;
        }
        auto const& fields = attachment->as_object();
        auto const* filename = fields.if_contains("filename");
        auto const* media_type = fields.if_contains("media_type");
        auto const* size_value = fields.if_contains("size");
        auto size = size_value ? parse_int64(*size_value) : std::nullopt;
        if (!filename || !filename->is_string() || filename->as_string().empty() || filename->as_string().size() > 255 ||
            !media_type || !media_type->is_string() || !size || *size < 0 ||
            *size > static_cast<std::int64_t>(max_attachment_size))
        {
            return false;
        }
        auto const type = std::string_view(media_type->as_string());
        if (type != "application/octet-stream" && type != "image/png" && type != "image/jpeg")
        {
            return false;
        }
        value.attachment = attachment_info{std::string(filename->as_string()), std::string(type), *size};
    }
    return parse_reply(object, value.reply) && parse_edited_at(object, value.edited_at) &&
           parse_deleted(object, value.deleted);
}

bool parse_presence(boost::json::object const& object, presence& value)
{
    auto const* user_value = object.if_contains("user");
    auto const* online_value = object.if_contains("online");
    auto const* last_seen_value = object.if_contains("last_seen");
    if (!user_value || !online_value || !online_value->is_bool() || !last_seen_value)
    {
        return false;
    }

    auto user = parse_int64(*user_value);
    auto last_seen = parse_int64(*last_seen_value);
    if (!user || *user <= 0 || !last_seen || *last_seen < 0)
    {
        return false;
    }

    value.user = *user;
    value.online = online_value->as_bool();
    value.last_seen = *last_seen;
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

    void notify_message(message value, bool updated)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        message_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = updated ? message_updated_handler_ : message_handler_;
        }
        if (handler)
        {
            handler(std::move(value));
        }
    }

    void notify_read(std::int64_t conversation, std::int64_t user, std::int64_t message)
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
            handler(conversation, user, message);
        }
    }

    void notify_presence(presence value)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        presence_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = presence_handler_;
        }
        if (handler)
        {
            handler(std::move(value));
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

            if (method->as_string() == "message" || method->as_string() == "message_updated")
            {
                message notification;
                if (!parse_message(params->as_object(), notification))
                {
                    report_error(make_error(error_kind::protocol, "Invalid message notification"));
                    return;
                }
                notify_message(std::move(notification), method->as_string() == "message_updated");
                return;
            }

            if (method->as_string() == "read")
            {
                auto const* conversation_value = params->as_object().if_contains("conversation");
                auto const* user_value = params->as_object().if_contains("user");
                auto const* message_value = params->as_object().if_contains("message");
                if (!conversation_value || !user_value || !message_value)
                {
                    report_error(make_error(error_kind::protocol, "Invalid read notification"));
                    return;
                }
                auto conversation = parse_int64(*conversation_value);
                auto user = parse_int64(*user_value);
                auto read_message = parse_int64(*message_value);
                if (!conversation || *conversation <= 0 || !user || *user <= 0 || !read_message || *read_message <= 0)
                {
                    report_error(make_error(error_kind::protocol, "Invalid read notification"));
                    return;
                }
                notify_read(*conversation, *user, *read_message);
                return;
            }

            if (method->as_string() == "typing")
            {
                auto const& fields = params->as_object();
                auto const* conversation_field = fields.if_contains("conversation");
                auto const* user_field = fields.if_contains("user");
                auto const* username = fields.if_contains("username");
                auto const* typing = fields.if_contains("typing");
                auto conversation = conversation_field ? parse_int64(*conversation_field) : std::nullopt;
                auto user = user_field ? parse_int64(*user_field) : std::nullopt;
                if (!conversation || *conversation <= 0 || !user || *user <= 0 || !username ||
                    !username->is_string() || username->as_string().empty() || !typing || !typing->is_bool())
                {
                    report_error(make_error(error_kind::protocol, "Invalid typing notification"));
                    return;
                }
                typing_handler handler;
                {
                    std::lock_guard lock(handler_mutex_);
                    handler = typing_handler_;
                }
                if (handler && !suppress_callbacks_.load())
                {
                    handler({*conversation, *user, std::string(username->as_string()), typing->as_bool()});
                }
                return;
            }

            if (method->as_string() == "presence")
            {
                presence notification;
                if (!parse_presence(params->as_object(), notification))
                {
                    report_error(make_error(error_kind::protocol, "Invalid presence notification"));
                    return;
                }
                notify_presence(std::move(notification));
                return;
            }

            if (method->as_string() == "conversation")
            {
                auto const* value = params->as_object().if_contains("conversation");
                auto id = value ? parse_int64(*value) : std::nullopt;
                if (!id || *id <= 0)
                {
                    report_error(make_error(error_kind::protocol, "Invalid conversation notification"));
                    return;
                }
                conversation_changed_handler handler;
                {
                    std::lock_guard lock(handler_mutex_);
                    handler = conversation_handler_;
                }
                if (handler && !suppress_callbacks_.load())
                {
                    handler(*id);
                }
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
            auto const* user_value = response->as_object().if_contains("user");
            auto user = user_value ? parse_int64(*user_value) : std::nullopt;
            if (!user || *user < 0 || (authenticated->as_bool() && *user == 0))
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid authenticate identity")));
                return;
            }
            handler(authentication_result{authenticated->as_bool(), *user});
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

    boost::capy::task<> get_conversations(std::optional<conversation_cursor> before, conversations_handler handler)
    {
        boost::json::object params;
        if (before)
        {
            params.emplace("before", boost::json::object{{"activity", before->activity}, {"id", before->id}});
        }
        send_request(
            "get_conversations", std::move(params),
            [handler = std::move(handler)](auto response) mutable
            {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversations")));
                return;
            }
                auto const* list = response->as_object().if_contains("conversations");
                auto const* next = response->as_object().if_contains("next");
                if (!list || !list->is_array() || !next)
            {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversations")));
                return;
            }
                conversations_result result;
                for (auto const& value : list->as_array())
            {
                if (!value.is_object())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation")));
                    return;
                }
                auto const& object = value.as_object();
                    auto const* id_value = object.if_contains("id");
                    auto const* kind = object.if_contains("kind");
                auto const* user_value = object.if_contains("user");
                    auto const* name = object.if_contains("username");
                    auto const* last = object.if_contains("last");
                auto const* unread_value = object.if_contains("unread");
                    auto const* count_value = object.if_contains("member_count");
                    auto id = id_value ? parse_int64(*id_value) : std::nullopt;
                    auto unread = unread_value ? parse_uint64(*unread_value) : std::nullopt;
                    auto count = count_value ? parse_uint64(*count_value) : std::nullopt;
                    if (!id || *id <= 0 || !kind || !kind->is_string() || !user_value || !name || !name->is_string() ||
                        !last || !unread || !count || *count == 0)
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation")));
                    return;
                }
                    conversation item;
                    item.id = *id;
                    item.username = std::string(name->as_string());
                    item.unread = *unread;
                    item.member_count = *count;
                    if (kind->as_string() == "direct")
                {
                        auto peer = parse_int64(*user_value);
                        if (!peer || *peer <= 0)
                        {
                            handler(std::unexpected(make_error(error_kind::protocol, "Invalid direct conversation")));
                    return;
                }
                        item.user = *peer;
                    }
                    else if (kind->as_string() == "group" && user_value->is_null())
                {
                        item.kind = conversation_kind::group;
                    }
                    else
                    {
                        handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation kind")));
                    return;
                }
                    if (!last->is_null() && (!last->is_object() || !parse_message(last->as_object(), item.last) ||
                                             item.last.conversation != item.id))
                    {
                        handler(std::unexpected(make_error(error_kind::protocol, "Invalid last message")));
                        return;
            }
                    result.conversations.push_back(std::move(item));
                }
                if (!next->is_null())
                {
                    if (!next->is_object())
                    {
                        handler(std::unexpected(make_error(error_kind::protocol, "Invalid cursor")));
                        return;
                    }
                    auto const* activity = next->as_object().if_contains("activity");
                    auto const* id = next->as_object().if_contains("id");
                    auto a = activity ? parse_int64(*activity) : std::nullopt;
                    auto i = id ? parse_int64(*id) : std::nullopt;
                    if (!a || *a <= 0 || !i || *i <= 0)
                    {
                        handler(std::unexpected(make_error(error_kind::protocol, "Invalid cursor")));
                        return;
                    }
                    result.next = conversation_cursor{*a, *i};
                }
                handler(std::move(result));
        });
        co_return;
    }

    boost::capy::task<> create_conversation(std::string method, boost::json::object params,
                                            conversation_handler handler)
    {
        send_request(std::move(method), std::move(params),
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto const* value =
                             response->is_object() ? response->as_object().if_contains("conversation") : nullptr;
                         auto id = value ? parse_int64(*value) : std::nullopt;
                         if (!id || *id <= 0)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation result")));
                             return;
                         }
                         handler(*id);
                     });
        co_return;
    }

    boost::capy::task<> get_members(std::int64_t conversation, members_handler handler)
    {
        send_request("get_members", {{"conversation", conversation}}, [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            auto const* values = response->is_object() ? response->as_object().if_contains("members") : nullptr;
            if (!values || !values->is_array())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid members result")));
                return;
            }
            std::vector<conversation_member> members;
            for (auto const& value : values->as_array())
            {
                user person;
                auto const* role = value.is_object() ? value.as_object().if_contains("role") : nullptr;
                if (!role || !role->is_string() || !parse_user(value.as_object(), person) ||
                    (role->as_string() != "owner" && role->as_string() != "admin" && role->as_string() != "member"))
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid member")));
                    return;
                }
                members.push_back({person.id, std::move(person.username), role->as_string() == "owner" ? member_role::owner :
                    role->as_string() == "admin" ? member_role::admin : member_role::member});
            }
            handler(std::move(members));
        });
        co_return;
    }

    boost::capy::task<> group_action(std::string method, boost::json::object params, group_action_handler handler)
    {
        send_request(std::move(method), std::move(params), [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            auto const* changed = response->is_object() ? response->as_object().if_contains("changed") : nullptr;
            if (!changed || !changed->is_bool())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid group action result")));
                return;
            }
            handler(changed->as_bool());
        });
        co_return;
    }

    boost::capy::task<> get_users(std::string method, boost::json::object params, users_handler handler)
    {
        send_request(std::move(method), std::move(params),
                     [handler = std::move(handler)](auto response) mutable
                     {
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

    boost::capy::task<> get_presence(presences_handler handler)
    {
        send_request("get_presence", {}, [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            if (!response->is_object())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_presence result")));
                return;
            }

            auto const* users_value = response->as_object().if_contains("users");
            if (!users_value || !users_value->is_array())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_presence result")));
                return;
            }

            std::vector<presence> users;
            users.reserve(users_value->as_array().size());
            for (auto const& value : users_value->as_array())
            {
                if (!value.is_object())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid presence")));
                    return;
                }

                presence item;
                if (!parse_presence(value.as_object(), item))
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid presence")));
                    return;
                }
                users.push_back(std::move(item));
            }

            handler(std::move(users));
        });

        co_return;
    }

    boost::capy::task<> get_messages(std::int64_t conversation, std::optional<std::int64_t> before,
                                     messages_handler handler, std::optional<std::int64_t> after,
                                     std::optional<std::string> query = {})
    {
        boost::json::object params;
        params.emplace("conversation", conversation);
        if (before)
        {
            params.emplace("before", *before);
        }

        if (after)
        {
            params.emplace("after", *after);
        }
        if (query)
        {
            params.emplace("query", std::move(*query));
        }
        send_request(query ? "search_messages" : "get_messages", std::move(params),
                     [conversation, handler = std::move(handler)](auto response) mutable
                     {
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
                         auto const* read_value = response->as_object().if_contains("read_positions");
                         auto const* more_value = response->as_object().if_contains("has_more");
                         if (!messages_value || !messages_value->is_array() || !read_value || !read_value->is_array() ||
                             !more_value || !more_value->is_bool())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_messages result")));
                return;
            }

            messages_result result;
                         result.has_more = more_value->as_bool();
                         for (auto const& position : read_value->as_array())
                         {
                             auto const* user_value =
                                 position.is_object() ? position.as_object().if_contains("user") : nullptr;
                             auto const* message_value =
                                 position.is_object() ? position.as_object().if_contains("message") : nullptr;
                             auto user = user_value ? parse_int64(*user_value) : std::nullopt;
                             auto message = message_value ? parse_int64(*message_value) : std::nullopt;
                             if (!user || *user <= 0 || !message || *message < 0)
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid read position")));
                                 return;
                             }
                             result.read_positions.push_back(read_position{*user, *message});
                         }
            result.messages.reserve(messages_value->as_array().size());
            for (auto const& value : messages_value->as_array())
            {
                if (!value.is_object())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid message")));
                    return;
                }

                message item;
                             if (!parse_message(value.as_object(), item) || item.conversation != conversation)
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid message")));
                    return;
                }
                result.messages.push_back(std::move(item));
            }
            handler(std::move(result));
        });

        co_return;
    }

    boost::capy::task<> send_message(std::int64_t conversation, std::string text, send_message_handler handler,
                                     std::optional<std::int64_t> reply_to)
    {
        boost::json::object params;
        params.emplace("conversation", conversation);
        params.emplace("text", std::move(text));
        if (reply_to)
        {
            params.emplace("reply_to", *reply_to);
        }

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
            if (!parse_reply(object, result.reply))
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid reply")));
                return;
            }
            handler(result);
        });

        co_return;
    }

    struct upload_job
    {
        std::int64_t conversation = 0;
        std::int64_t upload = 0;
        std::string filename;
        std::string data;
        std::size_t offset = 0;
        std::optional<std::int64_t> reply_to;
        message_result_handler handler;
    };

    void fail_upload(std::shared_ptr<upload_job> const& job, error value)
    {
        if (job->upload > 0)
        {
            send_request("cancel_attachment", {{"upload", job->upload}}, [](auto) {});
        }
        job->handler(std::unexpected(std::move(value)));
    }

    void upload_next(std::shared_ptr<upload_job> job)
    {
        if (job->offset == job->data.size())
        {
            boost::json::object params{{"upload", job->upload}};
            if (job->reply_to)
            {
                params.emplace("reply_to", *job->reply_to);
            }
            send_request("finish_attachment", std::move(params), [this, job](auto response) {
                if (!response)
                {
                    fail_upload(job, std::move(response.error()));
                    return;
                }
                message value;
                if (!response->is_object() || !parse_message(response->as_object(), value) ||
                    value.conversation != job->conversation || value.deleted || !value.attachment ||
                    value.attachment->filename != job->filename ||
                    value.attachment->size != static_cast<std::int64_t>(job->data.size()))
                {
                    fail_upload(job, make_error(error_kind::protocol, "Invalid attachment message"));
                    return;
                }
                job->handler(std::move(value));
            });
            return;
        }
        auto const count = std::min(attachment_chunk_size, job->data.size() - job->offset);
        auto chunk = detail::encode_base64(std::string_view(job->data).substr(job->offset, count));
        send_request("upload_attachment", {{"upload", job->upload}, {"offset", job->offset}, {"data", std::move(chunk)}},
            [this, job, count](auto response) {
                if (!response)
                {
                    fail_upload(job, std::move(response.error()));
                    return;
                }
                auto const* field = response->is_object() ? response->as_object().if_contains("offset") : nullptr;
                auto offset = field ? parse_int64(*field) : std::nullopt;
                if (!offset || *offset != static_cast<std::int64_t>(job->offset + count))
                {
                    fail_upload(job, make_error(error_kind::protocol, "Invalid attachment upload offset"));
                    return;
                }
                job->offset += count;
                upload_next(job);
            });
    }

    boost::capy::task<> send_attachment(std::int64_t conversation, std::string filename, std::string data,
                                       message_result_handler handler, std::optional<std::int64_t> reply_to)
    {
        if (data.size() > max_attachment_size)
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Attachment exceeds 10 MiB")));
            co_return;
        }
        auto job = std::make_shared<upload_job>();
        job->conversation = conversation;
        job->filename = std::move(filename);
        job->data = std::move(data);
        job->reply_to = reply_to;
        job->handler = std::move(handler);
        send_request("begin_attachment", {{"conversation", conversation}, {"filename", job->filename},
                                           {"size", job->data.size()}}, [this, job](auto response) {
            if (!response)
            {
                fail_upload(job, std::move(response.error()));
                return;
            }
            auto const* field = response->is_object() ? response->as_object().if_contains("upload") : nullptr;
            auto upload = field ? parse_int64(*field) : std::nullopt;
            if (!upload || *upload <= 0)
            {
                fail_upload(job, make_error(error_kind::protocol, "Invalid attachment upload id"));
                return;
            }
            job->upload = *upload;
            upload_next(job);
        });
        co_return;
    }

    struct download_job
    {
        std::int64_t conversation = 0;
        std::int64_t message = 0;
        std::optional<std::int64_t> size;
        std::string data;
        attachment_handler handler;
    };

    void download_next(std::shared_ptr<download_job> job)
    {
        send_request("get_attachment", {{"conversation", job->conversation}, {"message", job->message},
                                         {"offset", job->data.size()}}, [this, job](auto response) {
            if (!response)
            {
                job->handler(std::unexpected(std::move(response.error())));
                return;
            }
            auto const* fields = response->is_object() ? &response->as_object() : nullptr;
            auto const* size_field = fields ? fields->if_contains("size") : nullptr;
            auto const* offset_field = fields ? fields->if_contains("offset") : nullptr;
            auto const* data_field = fields ? fields->if_contains("data") : nullptr;
            auto const* more_field = fields ? fields->if_contains("has_more") : nullptr;
            auto size = size_field ? parse_int64(*size_field) : std::nullopt;
            auto offset = offset_field ? parse_int64(*offset_field) : std::nullopt;
            if (!size || *size < 0 || *size > static_cast<std::int64_t>(max_attachment_size) ||
                (job->size && *job->size != *size) || !offset || *offset != static_cast<std::int64_t>(job->data.size()) ||
                *offset > *size || !data_field || !data_field->is_string() || !more_field || !more_field->is_bool() ||
                data_field->as_string().size() > 4 * ((attachment_chunk_size + 2) / 3))
            {
                job->handler(std::unexpected(make_error(error_kind::protocol, "Invalid attachment chunk")));
                return;
            }
            auto bytes = detail::decode_base64(std::string_view(data_field->as_string()));
            auto const expected_size = std::min(attachment_chunk_size, static_cast<std::size_t>(*size - *offset));
            if (!bytes || bytes->size() != expected_size ||
                more_field->as_bool() != (*offset + static_cast<std::int64_t>(expected_size) < *size))
            {
                job->handler(std::unexpected(make_error(error_kind::protocol, "Invalid attachment chunk data")));
                return;
            }
            if (!job->size)
            {
                job->size = size;
                job->data.reserve(*size);
            }
            job->data.append(*bytes);
            if (more_field->as_bool())
            {
                download_next(job);
            }
            else
            {
                job->handler(std::move(job->data));
            }
        });
    }

    boost::capy::task<> get_attachment(std::int64_t conversation, std::int64_t message, attachment_handler handler)
    {
        auto job = std::make_shared<download_job>();
        job->conversation = conversation;
        job->message = message;
        job->handler = std::move(handler);
        download_next(job);
        co_return;
    }

    boost::capy::task<> update_message(std::string method, std::int64_t conversation, std::int64_t id,
                                       std::optional<std::string> text, message_result_handler handler)
    {
        boost::json::object params{{"conversation", conversation}, {"message", id}};
        if (text)
        {
            params.emplace("text", std::move(*text));
        }
        send_request(std::move(method), std::move(params),
                     [conversation, id, handler = std::move(handler)](auto result) mutable
                     {
                         if (!result)
                         {
                             handler(std::unexpected(std::move(result.error())));
                             return;
                         }
                         message value;
                         if (!result->is_object() || !parse_message(result->as_object(), value) ||
                             value.conversation != conversation || value.id != id)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid message result")));
                             return;
                         }
                         handler(std::move(value));
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

    boost::capy::task<> remove_contact(std::int64_t contact, remove_contact_handler handler)
    {
        boost::json::object params;
        params.emplace("user", contact);
        send_request("remove_contact", std::move(params), [handler = std::move(handler)](auto response) mutable {
            if (!response)
            {
                handler(std::unexpected(std::move(response.error())));
                return;
            }
            auto const* removed = response->is_object() ? response->as_object().if_contains("removed") : nullptr;
            if (!removed || !removed->is_bool())
            {
                handler(std::unexpected(make_error(error_kind::protocol, "Invalid remove_contact result")));
                return;
            }
            handler(removed->as_bool());
        });
        co_return;
    }

    boost::capy::task<> mark_read(std::int64_t conversation, std::int64_t message_id, mark_read_handler handler)
    {
        boost::json::object params;
        params.emplace("conversation", conversation);
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

    boost::capy::task<> set_typing(std::int64_t conversation, bool typing, typing_result_handler handler)
    {
        send_request("set_typing", {{"conversation", conversation}, {"typing", typing}},
            [handler = std::move(handler)](auto response) mutable {
                if (!response)
                {
                    handler(std::unexpected(std::move(response.error())));
                    return;
                }
                auto const* realtime = response->is_object() ? response->as_object().if_contains("realtime") : nullptr;
                if (!realtime || !realtime->is_bool())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid set_typing result")));
                    return;
                }
                handler(realtime->as_bool());
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
    message_handler message_updated_handler_;
    read_handler read_handler_;
    conversation_changed_handler conversation_handler_;
    presence_handler presence_handler_;
    typing_handler typing_handler_;
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

void client::set_message_updated_handler(message_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->message_updated_handler_ = std::move(handler);
}

void client::set_read_handler(read_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->read_handler_ = std::move(handler);
}

void client::set_presence_handler(presence_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->presence_handler_ = std::move(handler);
}

void client::set_typing_handler(typing_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->typing_handler_ = std::move(handler);
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

void client::get_conversations(std::optional<conversation_cursor> before, conversations_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_conversations(before, std::move(handler)));
}

void client::open_direct_conversation(std::int64_t user, conversation_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->create_conversation("open_direct_conversation", {{"user", user}}, std::move(handler)));
}

void client::create_group(std::string title, std::vector<std::int64_t> members, conversation_handler handler)
{
    boost::json::array values;
    for (auto id : members)
    {
        values.emplace_back(id);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->create_conversation(
        "create_group", {{"title", std::move(title)}, {"members", std::move(values)}}, std::move(handler)));
}

void client::get_members(std::int64_t conversation, members_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->get_members(conversation, std::move(handler)));
}

void client::set_group_admin(std::int64_t conversation, std::int64_t user, bool admin, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action(
        "set_group_admin", {{"conversation", conversation}, {"user", user}, {"admin", admin}}, std::move(handler)));
}

void client::set_conversation_handler(conversation_changed_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->conversation_handler_ = std::move(handler);
}

void client::rename_group(std::int64_t conversation, std::string title, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action(
        "rename_group", {{"conversation", conversation}, {"title", std::move(title)}}, std::move(handler)));
}

void client::invite_group_members(std::int64_t conversation, std::vector<std::int64_t> members, group_action_handler handler)
{
    boost::json::array values;
    for (auto id : members)
    {
        values.push_back(id);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action(
        "invite_group_members", {{"conversation", conversation}, {"members", std::move(values)}}, std::move(handler)));
}

void client::leave_group(std::int64_t conversation, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action(
        "leave_group", {{"conversation", conversation}}, std::move(handler)));
}

void client::get_contacts(users_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_users("get_contacts", {}, std::move(handler)));
}

void client::get_presence(presences_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_presence(std::move(handler)));
}

void client::get_messages(std::int64_t conversation, std::optional<std::int64_t> before, messages_handler handler,
                          std::optional<std::int64_t> after)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->get_messages(conversation, before, std::move(handler), after));
}

void client::search_messages(std::int64_t conversation, std::string query, std::optional<std::int64_t> before,
                             messages_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->get_messages(conversation, before, std::move(handler), {}, std::move(query)));
}

void client::send_message(std::int64_t user, std::string text, send_message_handler handler,
                          std::optional<std::int64_t> reply_to)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->send_message(user, std::move(text), std::move(handler), reply_to));
}

void client::send_attachment(std::int64_t conversation, std::string filename, std::string data,
                            message_result_handler handler, std::optional<std::int64_t> reply_to)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->send_attachment(conversation, std::move(filename), std::move(data), std::move(handler), reply_to));
}

void client::get_attachment(std::int64_t conversation, std::int64_t message, attachment_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->get_attachment(conversation, message, std::move(handler)));
}

void client::delete_message(std::int64_t conversation, std::int64_t message, message_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->update_message("delete_message", conversation, message, std::nullopt, std::move(handler)));
}

void client::edit_message(std::int64_t conversation, std::int64_t message, std::string text,
                          message_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->update_message("edit_message", conversation, message, std::move(text), std::move(handler)));
}

void client::search_users(std::string query, users_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->search_users(std::move(query), std::move(handler)));
}

void client::add_contact(std::int64_t user, user_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->add_contact(user, std::move(handler)));
}

void client::remove_contact(std::int64_t user, remove_contact_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->remove_contact(user, std::move(handler)));
}

void client::mark_read(std::int64_t user, std::int64_t message, mark_read_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->mark_read(user, message, std::move(handler)));
}

void client::set_typing(std::int64_t conversation, bool typing, typing_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->set_typing(conversation, typing, std::move(handler)));
}

}    // namespace chat
