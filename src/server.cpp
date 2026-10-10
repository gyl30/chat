#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <string_view>

#include <boost/url/parse.hpp>
#include <boost/capy/write.hpp>
#include <boost/capy/ex/work_guard.hpp>
#include <boost/http/field.hpp>
#include <boost/http/config.hpp>
#include <boost/http/status.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/io_task.hpp>
#include <boost/http/version.hpp>
#include <boost/http/serializer.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/capy/io/any_read_stream.hpp>
#include <boost/http/io/any_buffer_sink.hpp>
#include <boost/http/io/any_buffer_source.hpp>

#include "server.hpp"
#include "websocket.hpp"
#include "chat_session.hpp"

namespace
{

class connection_worker final : public boost::corosio::tcp_server::worker_base
{
   public:
    connection_worker(boost::corosio::io_context& io_context,
                      boost::http::router<boost::http::route_params> router,
                      boost::http::shared_parser_config parser_config,
                      boost::http::shared_serializer_config serializer_config,
                      online_users& users,
                      invite_attempts& invite_attempts,
                      pg_connection_pool& database,
                      std::optional<boost::corosio::tls_context> const& tls)
        : io_context_(io_context),
          socket_(io_context),
          stream_(&socket_),
          router_(std::move(router)),
          users_(users),
          invite_attempts_(invite_attempts),
          database_(database),
          parser_(std::move(parser_config)),
          serializer_(std::move(serializer_config))
    {
        if (tls)
        {
            tls_.emplace(&socket_, *tls);
            stream_ = boost::capy::any_stream(&*tls_);
        }
        serializer_.set_message(params_.res);
        params_.req_body = boost::http::any_buffer_source(parser_.source_for(stream_));
        params_.res_body = boost::http::any_buffer_sink(serializer_.sink_for(stream_));
    }

    boost::corosio::tcp_socket& socket() override { return socket_; }

    void run(boost::corosio::tcp_server::launcher launch) override { launch(io_context_.get_executor(), run_session()); }

   private:
    boost::capy::io_task<> send_websocket_upgrade(std::string_view accept)
    {
        params_.res.clear();
        params_.res.set_start_line(boost::http::status::switching_protocols, boost::http::version::http_1_1);
        params_.res.set(boost::http::field::upgrade, "websocket");
        params_.res.set(boost::http::field::connection, "Upgrade");
        params_.res.set(boost::http::field::sec_websocket_accept, accept);

        serializer_.reset();
        serializer_.start();
        while (!serializer_.is_done())
        {
            auto prepared = serializer_.prepare();
            if (prepared.has_error())
            {
                co_return std::error_code(prepared.error());
            }

            if (boost::capy::buffer_empty(*prepared))
            {
                serializer_.consume(0);
                continue;
            }

            auto [ec, written] = co_await boost::capy::write(stream_, *prepared);
            serializer_.consume(written);
            if (ec)
            {
                co_return ec;
            }
        }

        co_return {};
    }

    boost::capy::task<void> run_session()
    {
        // Workers are reused only after the previous session has returned; no TLS operation
        // is still in flight when the next connection starts.
        if (tls_)
        {
            tls_->reset();
            auto [handshake_ec] = co_await tls_->handshake(boost::corosio::tls_role::server);
            if (handshake_ec)
            {
                socket_.close();
                co_return;
            }
        }
        // Offloaded bcrypt work must finish before the owning io_context drains.
        auto work = boost::capy::make_work_guard(io_context_.get_executor());
        parser_.reset();
        parser_.start();
        params_.session_data.clear();

        for (;;)
        {
            auto [read_ec] = co_await parser_.read_header(stream_);
            if (read_ec)
            {
                break;
            }

            params_.req = parser_.get();
            params_.route_data.clear();
            params_.res.clear();
            params_.res.set_start_line(boost::http::status::ok, params_.req.version());
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
                    websocket_connection connection(socket_, boost::capy::any_stream(&stream_));
                    chat_session session(connection, users_, invite_attempts_, database_);
                    co_await session.run();
                }
                break;
            }

            auto route_result = co_await router_.dispatch(params_.req.method(), params_.url, params_);
            if (route_result.failed() || route_result.what() != boost::http::route_what::done)
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

    boost::corosio::io_context& io_context_;
    boost::corosio::tcp_socket socket_;
    std::optional<boost::corosio::openssl_stream> tls_;
    boost::capy::any_stream stream_;
    boost::http::router<boost::http::route_params> router_;
    online_users& users_;
    invite_attempts& invite_attempts_;
    pg_connection_pool& database_;
    boost::http::route_params params_;
    boost::http::request_parser parser_;
    boost::http::serializer serializer_;
};


}    // namespace

chat_server::chat_server(boost::corosio::io_context& io_context,
                         std::size_t worker_count,
                         boost::http::router<boost::http::route_params> router,
                         std::string database_connection_string,
                         std::size_t database_connection_count,
                         std::optional<boost::corosio::tls_context> tls)
    : database_(io_context, std::move(database_connection_string), database_connection_count),
      server_(io_context, io_context.get_executor())
{
    auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
    auto serializer_config = boost::http::make_serializer_config(boost::http::serializer_config{});
    std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> workers;
    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
        workers.push_back(std::make_unique<connection_worker>(
            io_context, router, parser_config, serializer_config, users_, invite_attempts_, database_, tls));
    }
    server_.set_workers(std::move(workers));
}

std::error_code chat_server::bind(boost::corosio::endpoint endpoint) { return server_.bind(endpoint); }

boost::corosio::endpoint chat_server::local_endpoint(std::size_t index) const noexcept { return server_.local_endpoint(index); }

void chat_server::start() { server_.start(); }

void chat_server::stop() { server_.stop(); }

void chat_server::join() { server_.join(); }
