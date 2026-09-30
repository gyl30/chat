#ifndef CHAT_CLIENT_INCLUDE_CHAT_TRANSPORT_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_TRANSPORT_HPP

#include <string>

namespace chat
{

class transport_listener
{
   public:
    virtual ~transport_listener() = default;

    virtual void on_transport_open() = 0;
    virtual void on_transport_text(std::string message) = 0;
    virtual void on_transport_close() = 0;
    virtual void on_transport_error(std::string message) = 0;
};

class transport
{
   public:
    virtual ~transport() = default;

    virtual void set_listener(transport_listener* listener) = 0;
    virtual void open(std::string url) = 0;
    virtual void send(std::string message) = 0;
    virtual void close() = 0;
};

}    // namespace chat

#endif
