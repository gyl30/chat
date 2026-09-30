#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <chat/client.hpp>

namespace
{

class fake_transport : public chat::transport
{
   public:
    void set_listener(chat::transport_listener* listener) override
    {
        listener_ = listener;
    }

    void open(std::string url) override
    {
        url_ = std::move(url);
    }

    void send(std::string message) override
    {
        sent_.push_back(std::move(message));
    }

    void close() override
    {
        closed_ = true;
    }

    void emit_open()
    {
        listener_->on_transport_open();
    }

    void emit_text(std::string message)
    {
        listener_->on_transport_text(std::move(message));
    }

    void emit_close()
    {
        listener_->on_transport_close();
    }

    std::string url_;
    std::vector<std::string> sent_;
    bool closed_ = false;

   private:
    chat::transport_listener* listener_ = nullptr;
};

}    // namespace

int main()
{
    auto transport = std::make_unique<fake_transport>();
    auto* transport_ptr = transport.get();
    chat::client client(std::move(transport));

    bool connected = false;
    bool disconnected = false;
    std::vector<chat::error> errors;
    client.set_connected_handler([&connected] { connected = true; });
    client.set_disconnected_handler([&disconnected] { disconnected = true; });
    client.set_error_handler([&errors](chat::error const& error) { errors.push_back(error); });

    client.open("ws://127.0.0.1:18080/ws");
    transport_ptr->emit_open();
    if (transport_ptr->url_ != "ws://127.0.0.1:18080/ws" || !connected)
    {
        std::cerr << "FAIL client connection events\n";
        return 1;
    }
    std::cout << "PASS client connection events\n";

    bool authenticated_called = false;
    bool authenticated = false;
    client.authenticate("alice", "secret", [&](std::expected<bool, chat::error> result) {
        authenticated_called = true;
        if (result)
        {
            authenticated = *result;
        }
    });
    if (transport_ptr->sent_.size() != 1 ||
        transport_ptr->sent_[0] !=
            R"({"jsonrpc":"2.0","method":"authenticate","params":{"username":"alice","password":"secret"},"id":1})")
    {
        std::cerr << "FAIL client authenticate request\n";
        return 1;
    }
    std::cout << "PASS client authenticate request\n";

    transport_ptr->emit_text(R"({"jsonrpc":"2.0","result":{"authenticated":true},"id":1})");
    if (!authenticated_called || !authenticated)
    {
        std::cerr << "FAIL client authenticate response\n";
        return 1;
    }
    std::cout << "PASS client authenticate response\n";

    bool rejected_called = false;
    bool rejected = false;
    client.authenticate("alice", "wrong", [&](std::expected<bool, chat::error> result) {
        rejected_called = true;
        rejected = result && !*result;
    });
    transport_ptr->emit_text(R"({"jsonrpc":"2.0","result":{"authenticated":false},"id":2})");
    if (!rejected_called || !rejected)
    {
        std::cerr << "FAIL client authenticate rejection\n";
        return 1;
    }
    std::cout << "PASS client authenticate rejection\n";

    bool rpc_error_called = false;
    chat::error rpc_error;
    client.authenticate("alice", "secret", [&](std::expected<bool, chat::error> result) {
        if (!result)
        {
            rpc_error_called = true;
            rpc_error = std::move(result.error());
        }
    });
    transport_ptr->emit_text(
        R"({"jsonrpc":"2.0","error":{"code":-32003,"message":"Already authenticated"},"id":3})");
    if (!rpc_error_called || rpc_error.kind != chat::error_kind::rpc || rpc_error.code != -32003 ||
        rpc_error.message != "Already authenticated")
    {
        std::cerr << "FAIL client rpc error\n";
        return 1;
    }
    std::cout << "PASS client rpc error\n";

    transport_ptr->emit_text("invalid");
    if (errors.size() != 1 || errors.back().kind != chat::error_kind::protocol)
    {
        std::cerr << "FAIL client protocol error\n";
        return 1;
    }
    std::cout << "PASS client protocol error\n";

    bool close_error_called = false;
    client.authenticate("alice", "secret", [&](std::expected<bool, chat::error> result) {
        close_error_called = !result && result.error().kind == chat::error_kind::transport;
    });
    transport_ptr->emit_close();
    if (!close_error_called || !disconnected)
    {
        std::cerr << "FAIL client pending request close\n";
        return 1;
    }
    std::cout << "PASS client pending request close\n";

    client.close();
    if (!transport_ptr->closed_)
    {
        std::cerr << "FAIL client close\n";
        return 1;
    }
    std::cout << "PASS client close\n";

    return 0;
}
