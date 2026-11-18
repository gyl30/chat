#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <charconv>
#include <string_view>
#include <system_error>
#include <optional>
#include <chrono>
#include <stop_token>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <spdlog/spdlog.h>
#include <boost/capy/task.hpp>
#include <boost/http/field.hpp>
#include <boost/http/method.hpp>
#include <boost/http/status.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/http/server/router.hpp>
#include <boost/corosio/signal_set.hpp>

#include "server.hpp"
#include "tls_config.hpp"
#include "database.hpp"
#include "attachment_io.hpp"

namespace
{

constexpr std::string_view kHealthBody = R"({"status":"ok"})";
constexpr std::string_view kNotFoundBody = "not found";

struct database_lock
{
    int descriptor;
    ~database_lock()
    {
        if (descriptor >= 0)
        {
            close(descriptor);
        }
    }
};

std::string environment(char const* name)
{
    auto const* value = std::getenv(name);
    return value ? value : "";
}

boost::capy::task<void> recover_objects(database& db, s3_objects& objects)
{
    // A timed-out PUT may still finish remotely. The settling window applies
    // on shutdown too, not just during startup recovery.
    auto const cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(
        (std::chrono::system_clock::now() - std::chrono::minutes(1)).time_since_epoch()).count();
    auto const removed = co_await recover_attachment_objects(db, objects, cutoff);
    if (removed < 0)
    {
        spdlog::warn("attachment cleanup remains pending");
    }
    else if (removed > 0)
    {
        spdlog::info("removed {} abandoned attachment objects", removed);
    }
}

// The existing one-minute settling window is applied on every pass. This
// coroutine is cancelled with the server's shutdown signal, without a thread.
boost::capy::task<void> recover_objects_periodically(database& db, s3_objects& objects)
{
    for (;;)
    {
        auto [ec] = co_await boost::corosio::delay(std::chrono::minutes(5));
        if (ec)
        {
            co_return;
        }
        co_await recover_objects(db, objects);
    }
}

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

boost::capy::task<int> wait_for_shutdown(boost::corosio::signal_set& signals, chat_server& server,
                                          std::stop_source& cleanup_stop)
{
    auto [ec, signal_number] = co_await signals.wait();
    cleanup_stop.request_stop();
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
        spdlog::error("usage: {} <port> <max-workers> <sqlite-file> [--tls-cert fullchain.pem --tls-key key.pem]", argv[0]);
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

    s3_settings settings{environment("CHAT_S3_ENDPOINT"), environment("CHAT_S3_BUCKET"),
                         environment("CHAT_S3_ACCESS_KEY"), environment("CHAT_S3_SECRET_KEY"),
                         environment("CHAT_S3_REGION"), true};
    auto const https = environment("CHAT_S3_HTTPS");
    if (settings.endpoint.empty() || settings.bucket.empty() || settings.access_key.empty() ||
        settings.secret_key.empty() || (!https.empty() && https != "0" && https != "1"))
    {
        spdlog::error("set CHAT_S3_ENDPOINT, CHAT_S3_BUCKET, CHAT_S3_ACCESS_KEY, and CHAT_S3_SECRET_KEY. "
                      "CHAT_S3_HTTPS is optional and accepts 0 or 1");
        return EXIT_FAILURE;
    }
    settings.https = https != "0";
    if (settings.region.empty())
    {
        settings.region = "us-east-1";
    }
    database_lock lock{open(argv[3], O_CREAT | O_RDWR | O_CLOEXEC, 0600)};
    if (lock.descriptor < 0 || flock(lock.descriptor, LOCK_EX | LOCK_NB) != 0)
    {
        spdlog::error("cannot open or exclusively lock SQLite file {}", argv[3]);
        return EXIT_FAILURE;
    }
    auto storage = database::open(argv[3]);
    if (!storage)
    {
        spdlog::error("SQLite startup failed: {}", storage.error().message);
        return EXIT_FAILURE;
    }
    s3_objects objects(std::move(settings));
    boost::corosio::io_context io_context;
    boost::capy::run_async(io_context.get_executor())(recover_objects(**storage, objects));
    io_context.run();
    io_context.restart();

    boost::http::router<boost::http::route_params> router;
    router.add(boost::http::method::get, "/health", health_handler);
    router.use(not_found_handler);

    chat_server server(io_context, max_workers, std::move(router), **storage, objects, tls);
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

    spdlog::info("chat server listening on port {} ({}) with {} workers", port, tls ? "TLS" : "plain", max_workers);
    server.start();

    std::stop_source cleanup_stop;
    boost::capy::run_async(io_context.get_executor(), cleanup_stop.get_token())(
        recover_objects_periodically(**storage, objects));
    int exit_code = EXIT_FAILURE;
    boost::capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(
        wait_for_shutdown(signals, server, cleanup_stop));
    io_context.run();
    server.join();
    io_context.restart();
    boost::capy::run_async(io_context.get_executor())(recover_objects(**storage, objects));
    io_context.run();

    return exit_code;
}
