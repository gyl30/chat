#ifndef CHAT_SRC_WEBSOCKET_HPP
#define CHAT_SRC_WEBSOCKET_HPP

#include <span>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

#include <wslay/wslay.h>
#include <boost/capy/io_task.hpp>
#include <boost/http/request_base.hpp>
#include <boost/corosio/tcp_socket.hpp>

struct websocket_message
{
    enum class type
    {
        text,
        close,
    };

    type message_type = type::text;
    std::string payload;
};

bool websocket_upgrade_accept(boost::http::request_base const& request, std::string& accept);

class websocket_connection
{
   public:
    explicit websocket_connection(boost::corosio::tcp_socket& socket);

    websocket_connection(websocket_connection const&) = delete;
    websocket_connection& operator=(websocket_connection const&) = delete;
    websocket_connection(websocket_connection&&) = delete;
    websocket_connection& operator=(websocket_connection&&) = delete;

    boost::capy::io_task<websocket_message> receive();

    void interrupt_receive() noexcept;

    boost::capy::io_task<> send_text(std::string_view payload);

   private:
    struct context_deleter
    {
        void operator()(wslay_event_context* context) const noexcept;
    };

    using context_ptr = std::unique_ptr<wslay_event_context, context_deleter>;

    boost::capy::io_task<> flush();

    static ssize_t receive_callback(wslay_event_context_ptr context, std::uint8_t* buffer, std::size_t size, int flags, void* user_data);

    static void message_callback(wslay_event_context_ptr context, wslay_event_on_msg_recv_arg const* message, void* user_data);

   private:
    boost::corosio::tcp_socket& socket_;
    context_ptr context_;
    std::array<std::uint8_t, 4096> input_buffer_{};
    std::span<std::uint8_t const> input_;
    std::deque<websocket_message> messages_;
    bool reading_ = false;
    bool interrupt_requested_ = false;
};

#endif
