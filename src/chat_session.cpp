#include <string>
#include <utility>
#include <string_view>

#include "chat_session.hpp"

namespace
{

constexpr std::string_view kEchoMethod = "echo";

}    // namespace

chat_session::chat_session(websocket_connection& connection) : connection_(connection) {}

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
            if (request.method == kEchoMethod)
            {
                rpc_error = co_await handle_echo(request, response);
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
}
