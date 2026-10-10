#ifndef CHAT_CLIENT_SRC_WEBSOCKET_HPP
#define CHAT_CLIENT_SRC_WEBSOCKET_HPP

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <chat/detail/websocket_heartbeat.hpp>

#include <wslay/wslay.h>
#include <boost/capy/io_task.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace chat::detail
{

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

class websocket_client
{
   public:
    explicit websocket_client(boost::corosio::io_context& io_context, websocket_heartbeat_config heartbeat = {});

    websocket_client(websocket_client const&) = delete;
    websocket_client& operator=(websocket_client const&) = delete;
    websocket_client(websocket_client&&) = delete;
    websocket_client& operator=(websocket_client&&) = delete;

    boost::capy::io_task<> connect(std::string_view url);
    boost::capy::io_task<websocket_message> receive();
    boost::capy::io_task<> send_text(std::string_view payload);
    std::optional<websocket_message> take_pending_message();

    void interrupt_receive() noexcept;
    void cancel() noexcept;
    void close() noexcept;

   private:
    struct context_deleter
    {
        void operator()(wslay_event_context* context) const noexcept;
    };

    using context_ptr = std::unique_ptr<wslay_event_context, context_deleter>;

    boost::capy::io_task<> handshake(std::string_view target, std::string_view host_header);
    boost::capy::io_task<> flush();
    boost::capy::io_task<> read_input();
    boost::capy::io_task<> read_once(std::size_t& transferred);
    std::error_code poll_heartbeat();

    static ssize_t receive_callback(wslay_event_context_ptr context, std::uint8_t* buffer, std::size_t size, int flags, void* user_data);
    static int genmask_callback(wslay_event_context_ptr context, std::uint8_t* buffer, std::size_t size, void* user_data);
    static void message_callback(wslay_event_context_ptr context, wslay_event_on_msg_recv_arg const* message, void* user_data);

   private:
    boost::corosio::resolver resolver_;
    boost::corosio::tcp_socket socket_;
    std::unique_ptr<boost::corosio::openssl_stream> tls_;
    boost::capy::any_stream stream_;
    context_ptr context_;
    websocket_heartbeat heartbeat_;
    std::optional<std::error_code> deferred_read_error_;
    std::array<std::uint8_t, 4096> input_buffer_{};
    std::span<std::uint8_t const> input_;
    std::deque<websocket_message> messages_;
    bool reading_ = false;
    bool interrupt_requested_ = false;
};

}    // namespace chat::detail

#endif
