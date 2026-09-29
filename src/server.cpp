#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/capy/buffers.hpp>
#include <boost/capy/io/any_read_stream.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/http/config.hpp>
#include <boost/http/field.hpp>
#include <boost/http/io/any_buffer_sink.hpp>
#include <boost/http/io/any_buffer_source.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/http/serializer.hpp>
#include <boost/http/status.hpp>
#include <boost/http/version.hpp>
#include <boost/url/parse.hpp>

#include "server.hpp"
#include "websocket.hpp"

namespace capy = boost::capy;
namespace corosio = boost::corosio;
namespace http = boost::http;

namespace
{

class connection_worker final : public corosio::tcp_server::worker_base
{
   public:
    connection_worker(corosio::io_context& io_context,
                      http::router<http::route_params> router,
                      http::shared_parser_config parser_config,
                      http::shared_serializer_config serializer_config)
        : io_context_(io_context),
          socket_(io_context),
          router_(std::move(router)),
          parser_(std::move(parser_config)),
          serializer_(std::move(serializer_config))
    {
        serializer_.set_message(params_.res);
        params_.req_body = http::any_buffer_source(parser_.source_for(socket_));
        params_.res_body = http::any_buffer_sink(serializer_.sink_for(socket_));
    }

    corosio::tcp_socket& socket() override { return socket_; }

    void run(corosio::tcp_server::launcher launch) override { launch(io_context_.get_executor(), run_session()); }

   private:
    capy::io_task<> send_websocket_upgrade(std::string_view accept)
    {
        params_.res.clear();
        params_.res.set_start_line(http::status::switching_protocols, http::version::http_1_1);
        params_.res.set(http::field::upgrade, "websocket");
        params_.res.set(http::field::connection, "Upgrade");
        params_.res.set(http::field::sec_websocket_accept, accept);

        serializer_.reset();
        serializer_.start();
        while (!serializer_.is_done())
        {
            auto prepared = serializer_.prepare();
            if (prepared.has_error())
            {
                co_return std::error_code(prepared.error());
            }

            if (capy::buffer_empty(*prepared))
            {
                serializer_.consume(0);
                continue;
            }

            auto [ec, written] = co_await capy::write(socket_, *prepared);
            serializer_.consume(written);
            if (ec)
            {
                co_return ec;
            }
        }

        co_return {};
    }

    capy::task<void> run_websocket()
    {
        websocket_connection connection(socket_);
        if (!connection.valid())
        {
            co_return;
        }

        for (;;)
        {
            auto [ec, message] = co_await connection.receive();
            if (ec || message.message_type == websocket_message::type::close)
            {
                break;
            }
        }
    }

    capy::task<void> run_session()
    {
        parser_.reset();
        parser_.start();
        params_.session_data.clear();

        for (;;)
        {
            auto [read_ec] = co_await parser_.read_header(socket_);
            if (read_ec)
            {
                break;
            }

            params_.req = parser_.get();
            params_.route_data.clear();
            params_.res.clear();
            params_.res.set_start_line(http::status::ok, params_.req.version());
            params_.res.set_keep_alive(params_.req.keep_alive());
            serializer_.reset();

            auto parsed_url = boost::urls::parse_uri_reference(params_.req.target());
            if (parsed_url.has_error())
            {
                break;
            }
            params_.url = parsed_url.value();

            std::string accept;
            if (params_.url.encoded_path() == "/ws" && parser_.is_complete() && !parser_.has_buffered_data() &&
                websocket_upgrade_accept(params_.req, accept))
            {
                auto [upgrade_ec] = co_await send_websocket_upgrade(accept);
                if (!upgrade_ec)
                {
                    co_await run_websocket();
                }
                break;
            }

            auto route_result = co_await router_.dispatch(params_.req.method(), params_.url, params_);
            if (route_result.failed() || route_result.what() != http::route_what::done)
            {
                break;
            }

            if (!params_.res.keep_alive() || !parser_.is_complete())
            {
                break;
            }

            parser_.start();
        }

        params_.route_data.clear();
        params_.session_data.clear();
        socket_.close();
    }

    corosio::io_context& io_context_;
    corosio::tcp_socket socket_;
    http::router<http::route_params> router_;
    http::route_params params_;
    http::request_parser parser_;
    http::serializer serializer_;
};


}    // namespace

chat_server::chat_server(corosio::io_context& io_context, std::size_t worker_count, http::router<http::route_params> router)
    : server_(io_context, io_context.get_executor())
{
    auto parser_config = http::make_parser_config(http::parser_config{true});
    auto serializer_config = http::make_serializer_config(http::serializer_config{});
    std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> workers;
    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
        workers.push_back(std::make_unique<connection_worker>(io_context, router, parser_config, serializer_config));
    }
    server_.set_workers(std::move(workers));
}

std::error_code chat_server::bind(corosio::endpoint endpoint) { return server_.bind(endpoint); }

corosio::endpoint chat_server::local_endpoint(std::size_t index) const noexcept { return server_.local_endpoint(index); }

void chat_server::start() { server_.start(); }

void chat_server::stop() { server_.stop(); }

void chat_server::join() { server_.join(); }
