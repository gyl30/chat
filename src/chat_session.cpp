#include <string>

#include "json_rpc.hpp"
#include "chat_session.hpp"

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

        auto rpc_error = dispatch_json_rpc(message.payload, response);
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
