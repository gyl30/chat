#include "chat_session.hpp"

chat_session::chat_session(websocket_connection& connection) : connection_(connection) {}

boost::capy::task<void> chat_session::run()
{
    for (;;)
    {
        auto receive_result = co_await connection_.receive();
        auto& [ec, message] = receive_result;
        if (ec || message.message_type == websocket_message::type::close)
        {
            break;
        }

        if (message.message_type != websocket_message::type::text)
        {
            continue;
        }

        auto [send_ec] = co_await connection_.send_text(message.payload);
        if (send_ec)
        {
            break;
        }
    }
}
