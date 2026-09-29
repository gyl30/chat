#include <memory>
#include <vector>
#include <utility>
#include <iostream>
#include <string_view>

#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>
#include <boost/url/parse.hpp>
#include <boost/capy/write.hpp>
#include <boost/http/field.hpp>
#include <boost/http/config.hpp>
#include <boost/http/method.hpp>
#include <boost/http/status.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/http/serializer.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/http/server/router.hpp>
#include <boost/http/request_parser.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/http/response_parser.hpp>

namespace
{

constexpr std::string_view kHealthBody = R"({"status":"ok"})";
constexpr std::string_view kNotFoundBody = "not found";

boost::http::route_task health_handler(boost::http::route_params& params)
{
    params.status(boost::http::status::ok);
    params.res.set(boost::http::field::content_type, "application/json");

    auto [ec] = co_await params.send(kHealthBody);
    if (ec)
    {
        co_return boost::http::route_error(ec);
    }

    co_return boost::http::route_done;
}

boost::http::route_task not_found_handler(boost::http::route_params& params)
{
    params.status(boost::http::status::not_found);

    auto [ec] = co_await params.send(kNotFoundBody);
    if (ec)
    {
        co_return boost::http::route_error(ec);
    }

    co_return boost::http::route_done;
}

class http_test_worker final : public boost::corosio::tcp_server::worker_base
{
   public:
    http_test_worker(boost::corosio::io_context& io_context,
                     boost::http::router<boost::http::route_params> router,
                     boost::http::shared_parser_config parser_config,
                     boost::http::shared_serializer_config serializer_config)
        : io_context_(io_context),
          socket_(io_context),
          router_(std::move(router)),
          parser_(std::move(parser_config)),
          serializer_(std::move(serializer_config))
    {
        serializer_.set_message(params_.res);
        params_.req_body = boost::http::any_buffer_source(parser_.source_for(socket_));
        params_.res_body = boost::http::any_buffer_sink(serializer_.sink_for(socket_));
    }

    boost::corosio::tcp_socket& socket() override { return socket_; }

    void run(boost::corosio::tcp_server::launcher launch) override { launch(io_context_.get_executor(), run_session()); }

   private:
    boost::capy::task<void> run_session()
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
            params_.res.set_start_line(boost::http::status::ok, params_.req.version());
            params_.res.set_keep_alive(params_.req.keep_alive());
            serializer_.reset();

            auto parsed_url = boost::urls::parse_uri_reference(params_.req.target());
            if (parsed_url.has_error())
            {
                break;
            }
            params_.url = parsed_url.value();

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
    boost::http::router<boost::http::route_params> router_;
    boost::http::route_params params_;
    boost::http::request_parser parser_;
    boost::http::serializer serializer_;
};

std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> make_workers(boost::corosio::io_context& io_context,
                                                                            boost::http::router<boost::http::route_params> const& router,
                                                                            boost::http::shared_parser_config const& parser_config,
                                                                            boost::http::shared_serializer_config const& serializer_config)
{
    std::vector<std::unique_ptr<boost::corosio::tcp_server::worker_base>> workers;
    workers.push_back(std::make_unique<http_test_worker>(io_context, router, parser_config, serializer_config));
    return workers;
}

struct server_stop_guard
{
    boost::corosio::tcp_server& server;

    ~server_stop_guard() { server.stop(); }
};

boost::capy::task<int> run_client(boost::corosio::io_context& io_context, boost::corosio::tcp_server& server, unsigned short port)
{
    server_stop_guard stop_guard{server};
    boost::corosio::tcp_socket socket(io_context);

    auto [connect_ec] = co_await socket.connect(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), port));
    if (connect_ec)
    {
        std::cerr << "FAIL HTTP connect: " << connect_ec.message() << '\n';
        co_return 1;
    }
    std::cout << "PASS HTTP connect\n";

    auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
    boost::http::response_parser response_parser(parser_config);

    constexpr std::string_view health_request =
        "GET /health HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    auto [health_write_ec, health_written] =
        co_await boost::capy::write(socket, boost::capy::const_buffer(health_request.data(), health_request.size()));
    if (health_write_ec || health_written != health_request.size())
    {
        std::cerr << "FAIL health request write: " << health_write_ec.message() << '\n';
        co_return 1;
    }

    response_parser.reset();
    response_parser.start();
    auto [health_read_ec] = co_await response_parser.read(socket);
    if (health_read_ec)
    {
        std::cerr << "FAIL health response read: " << health_read_ec.message() << '\n';
        co_return 1;
    }

    auto const& health_response = response_parser.get();
    if (health_response.status() != boost::http::status::ok || response_parser.body() != kHealthBody)
    {
        std::cerr << "FAIL health response\n";
        co_return 1;
    }
    std::cout << "PASS GET /health\n";

    constexpr std::string_view missing_request =
        "GET /missing HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    auto [missing_write_ec, missing_written] =
        co_await boost::capy::write(socket, boost::capy::const_buffer(missing_request.data(), missing_request.size()));
    if (missing_write_ec || missing_written != missing_request.size())
    {
        std::cerr << "FAIL missing request write: " << missing_write_ec.message() << '\n';
        co_return 1;
    }

    response_parser.start();
    auto [missing_read_ec] = co_await response_parser.read(socket);
    if (missing_read_ec)
    {
        std::cerr << "FAIL missing response read: " << missing_read_ec.message() << '\n';
        co_return 1;
    }

    auto const& missing_response = response_parser.get();
    if (missing_response.status() != boost::http::status::not_found || response_parser.body() != kNotFoundBody)
    {
        std::cerr << "FAIL missing response\n";
        co_return 1;
    }
    std::cout << "PASS same connection second request\n";
    std::cout << "PASS unknown path 404\n";

    socket.close();
    co_return 0;
}

}    // namespace

int main()
{
    boost::corosio::io_context io_context;

    boost::http::router<boost::http::route_params> router;
    router.add(boost::http::method::get, "/health", health_handler);
    router.use(not_found_handler);

    auto parser_config = boost::http::make_parser_config(boost::http::parser_config{true});
    auto serializer_config = boost::http::make_serializer_config(boost::http::serializer_config{});

    boost::corosio::tcp_server server(io_context, io_context.get_executor());
    server.set_workers(make_workers(io_context, router, parser_config, serializer_config));

    if (auto ec = server.bind(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "FAIL HTTP bind: " << ec.message() << '\n';
        return 1;
    }

    auto const port = server.local_endpoint().port();
    server.start();

    int exit_code = 1;
    boost::capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(run_client(io_context, server, port));

    io_context.run();
    server.join();

    if (exit_code != 0)
    {
        return exit_code;
    }

    std::cout << "PASS HTTP server shutdown\n";
    std::cout << "PASS Boost.HTTP validation\n";
    return 0;
}
