#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <charconv>
#include <string_view>
#include <system_error>

#include <spdlog/spdlog.h>
#include <boost/capy/task.hpp>
#include <boost/http/field.hpp>
#include <boost/http/method.hpp>
#include <boost/http/status.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/http/server/router.hpp>
#include <boost/corosio/signal_set.hpp>

#include "server.hpp"

namespace
{

constexpr std::string_view kHealthBody = R"({"status":"ok"})";
constexpr std::string_view kNotFoundBody = "not found";

template <class T>
bool parse_positive(std::string_view input, T& value)
{
    auto const [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), value);
    return ec == std::errc{} && ptr == input.data() + input.size() && value != 0;
}

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

boost::capy::task<int> wait_for_shutdown(boost::corosio::signal_set& signals, chat_server& server)
{
    auto [ec, signal_number] = co_await signals.wait();
    if (ec)
    {
        spdlog::error("signal wait failed: {}", ec.message());
        server.stop();
        co_return EXIT_FAILURE;
    }

    spdlog::info("received signal {}, stopping", signal_number);
    server.stop();
    co_return EXIT_SUCCESS;
}

}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        spdlog::error("usage: {} <port> <max-workers>", argv[0]);
        return EXIT_FAILURE;
    }

    std::uint16_t port = 0;
    std::size_t max_workers = 0;
    if (!parse_positive(argv[1], port))
    {
        spdlog::error("invalid port: {}", argv[1]);
        return EXIT_FAILURE;
    }
    if (!parse_positive(argv[2], max_workers))
    {
        spdlog::error("invalid max-workers: {}", argv[2]);
        return EXIT_FAILURE;
    }

    boost::corosio::io_context io_context;

    boost::http::router<boost::http::route_params> router;
    router.add(boost::http::method::get, "/health", health_handler);
    router.use(not_found_handler);

    chat_server server(io_context, max_workers, std::move(router), {});
    if (auto ec = server.bind(boost::corosio::endpoint(port)))
    {
        spdlog::error("bind failed: {}", ec.message());
        return EXIT_FAILURE;
    }

    boost::corosio::signal_set signals(io_context);
    if (auto ec = signals.add(SIGINT))
    {
        spdlog::error("failed to register SIGINT: {}", ec.message());
        return EXIT_FAILURE;
    }
    if (auto ec = signals.add(SIGTERM))
    {
        spdlog::error("failed to register SIGTERM: {}", ec.message());
        return EXIT_FAILURE;
    }

    spdlog::info("chat server listening on port {} with {} workers", port, max_workers);
    server.start();

    int exit_code = EXIT_FAILURE;
    boost::capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(wait_for_shutdown(signals, server));
    io_context.run();
    server.join();

    return exit_code;
}
