#include <string>
#include <utility>
#include <string_view>

#include "chat_session.hpp"

namespace
{

constexpr std::string_view kAuthenticateMethod = "authenticate";
constexpr std::string_view kEchoMethod = "echo";
constexpr std::string_view kRegisterMethod = "register";

}    // namespace

chat_session::chat_session(boost::corosio::io_context& io_context,
                           websocket_connection& connection,
                           online_users& users,
                           std::string const& database_connection_string)
    : io_context_(io_context), connection_(connection), users_(users), database_connection_string_(database_connection_string)
{
}

boost::capy::task<void> chat_session::run()
{
    std::string response;

    for (;;)
    {
        auto receive_result = co_await connection_.receive();
        auto& [ec, message] = receive_result;
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
            else if (request.method == kRegisterMethod)
            {
                rpc_error = co_await handle_register(request, response);
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
