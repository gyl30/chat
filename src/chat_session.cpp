#include <string>
#include <utility>
#include <string_view>
#include <system_error>

#include "chat_session.hpp"

namespace
{

constexpr std::string_view kAuthenticateMethod = "authenticate";
constexpr std::string_view kEchoMethod = "echo";
constexpr std::string_view kGetMessagesMethod = "get_messages";
constexpr std::string_view kGetUnreadCountMethod = "get_unread_count";
constexpr std::string_view kMarkReadMethod = "mark_read";
constexpr std::string_view kRegisterMethod = "register";
constexpr std::string_view kSendMessageMethod = "send_message";
constexpr std::size_t kMaxQueuedMessages = 64;

}    // namespace

chat_session::chat_session(websocket_connection& connection, online_users& users, pg_connection_pool& database)
    : connection_(connection), users_(users), database_(database)
{
}

bool chat_session::enqueue_message(std::string message)
{
    if (outgoing_messages_.size() >= kMaxQueuedMessages)
    {
        return false;
    }

    outgoing_messages_.push_back(std::move(message));
    connection_.interrupt_receive();
    return true;
}

boost::capy::task<void> chat_session::run()
{
    std::string response;
    bool running = true;

    while (running)
    {
        while (!outgoing_messages_.empty())
        {
            auto message = std::move(outgoing_messages_.front());
            outgoing_messages_.pop_front();

            auto [send_ec] = co_await connection_.send_text(message);
            if (send_ec)
            {
                running = false;
                break;
            }
        }
        if (!running)
        {
            break;
        }

        auto receive_result = co_await connection_.receive();
        auto& [ec, message] = receive_result;
        if (ec == std::errc::interrupted)
        {
            continue;
        }
        if (ec || message.message_type == websocket_message::type::close)
        {
            break;
        }

        json_rpc_request request;
        auto rpc_error = parse_json_rpc_request(message.payload, request, response);
        if (rpc_error)
        {
            break;
        }

        if (response.empty())
        {
            if (request.method == kAuthenticateMethod)
            {
                rpc_error = co_await handle_authenticate(request, response);
            }
            else if (request.method == kEchoMethod)
            {
                rpc_error = co_await handle_echo(request, response);
            }
            else if (request.method == kGetMessagesMethod)
            {
                rpc_error = co_await handle_get_messages(request, response);
            }
            else if (request.method == kGetUnreadCountMethod)
            {
                rpc_error = co_await handle_get_unread_count(request, response);
            }
            else if (request.method == kMarkReadMethod)
            {
                rpc_error = co_await handle_mark_read(request, response);
            }
            else if (request.method == kRegisterMethod)
            {
                rpc_error = co_await handle_register(request, response);
            }
            else if (request.method == kSendMessageMethod)
            {
                rpc_error = co_await handle_send_message(request, response);
            }
            else if (request.id.present)
            {
                rpc_error = serialize_json_rpc_method_not_found(std::move(request.id), response);
            }
        }

        if (rpc_error)
        {
            break;
        }
        if (response.empty())
        {
            continue;
        }

        auto [send_ec] = co_await connection_.send_text(response);
        if (send_ec)
        {
            break;
        }
    }

    if (user_id_)
    {
        users_.remove(*user_id_, *this);
    }
}
