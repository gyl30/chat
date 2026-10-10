#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <charconv>
#include <string_view>
#include <system_error>
#include <optional>

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
#include "tls_config.hpp"

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
    if (argc < 4)
    {
        spdlog::error("usage: {} <port> <max-workers> <database-connections> [--tls-cert fullchain.pem --tls-key key.pem]", argv[0]);
        return EXIT_FAILURE;
    }

    std::uint16_t port = 0;
    std::size_t max_workers = 0;
    std::size_t database_connections = 0;
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
    if (!parse_positive(argv[3], database_connections))
    {
        spdlog::error("invalid database-connections: {}", argv[3]);
        return EXIT_FAILURE;
    }

    std::optional<std::string_view> certificate_file;
    std::optional<std::string_view> private_key_file;
    for (int i = 4; i < argc; ++i)
    {
        std::string_view const option = argv[i];
        if (i + 1 >= argc || (option != "--tls-cert" && option != "--tls-key"))
        {
            spdlog::error("TLS options must be --tls-cert fullchain.pem and --tls-key key.pem");
            return EXIT_FAILURE;
        }
        auto& destination = option == "--tls-cert" ? certificate_file : private_key_file;
        if (destination)
        {
            spdlog::error("TLS options may only be specified once");
            return EXIT_FAILURE;
        }
        destination = argv[++i];
    }
    if (certificate_file.has_value() != private_key_file.has_value())
    {
        spdlog::error("--tls-cert and --tls-key must be specified together");
        return EXIT_FAILURE;
    }
    std::optional<boost::corosio::tls_context> tls;
    if (certificate_file)
    {
        auto context = make_server_tls_context(*certificate_file, *private_key_file);
        if (!context)
        {
            spdlog::error("TLS configuration failed: {}", context.error());
            return EXIT_FAILURE;
        }
        tls.emplace(std::move(*context));
    }

    boost::corosio::io_context io_context;

    boost::http::router<boost::http::route_params> router;
    router.add(boost::http::method::get, "/health", health_handler);
    router.use(not_found_handler);

    chat_server server(io_context, max_workers, std::move(router), {}, database_connections, tls);
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

    spdlog::info("chat server listening on port {} ({}) with {} workers and {} database connections", port, tls ? "TLS" : "plain", max_workers, database_connections);
    server.start();

    int exit_code = EXIT_FAILURE;
    boost::capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(wait_for_shutdown(signals, server));
    io_context.run();
    server.join();

    return exit_code;
}
