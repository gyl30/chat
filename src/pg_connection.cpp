#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <variant>

#include <boost/capy/io_result.hpp>
#include <boost/capy/when_any.hpp>

#include "pg_connection.hpp"

namespace capy = boost::capy;
namespace corosio = boost::corosio;

namespace
{

std::error_code make_libpq_error() { return std::make_error_code(std::errc::io_error); }

}    // namespace

void pg_connection::pg_conn_deleter::operator()(PGconn* connection) const noexcept
{
    if (connection)
    {
        PQfinish(connection);
    }
}

pg_connection::pg_connection(corosio::io_context& io_context) : wait_socket_(io_context) {}

pg_connection::~pg_connection() { close(); }

void pg_connection::close() noexcept
{
    wait_socket_.close();
    connection_.reset();
}

bool pg_connection::is_open() const noexcept { return connection_ && PQstatus(connection_.get()) == CONNECTION_OK; }

std::string_view pg_connection::error_message() const noexcept { return error_message_; }

std::error_code pg_connection::set_libpq_error()
{
    if (connection_)
    {
        error_message_ = PQerrorMessage(connection_.get());
    }
    else
    {
        error_message_ = "PostgreSQL connection is not available";
    }

    return make_libpq_error();
}

std::error_code pg_connection::refresh_wait_socket()
{
    // Corosio owns whatever is assigned to tcp_socket, so never assign
    // libpq's actual descriptor. Close the previous duplicate and make
    // a fresh duplicate of libpq's current descriptor.
    wait_socket_.close();

    int const pg_fd = PQsocket(connection_.get());
    if (pg_fd < 0)
    {
        error_message_ = "PQsocket returned an invalid descriptor";
        return std::make_error_code(std::errc::not_connected);
    }

    // F_DUPFD_CLOEXEC gives us an independently owned descriptor while
    // preserving the same underlying socket/open-file-description.
    int const wait_fd = ::fcntl(pg_fd, F_DUPFD_CLOEXEC, 0);
    if (wait_fd < 0)
    {
        error_message_ = "failed to duplicate PostgreSQL socket";
        return {errno, std::generic_category()};
    }

    auto ec = wait_socket_.assign(wait_fd);
    if (ec)
    {
        // assign() transfers ownership only on success.
        ::close(wait_fd);
        error_message_ = ec.message();
        return ec;
    }

    return {};
}

capy::io_task<std::error_code> pg_connection::wait_event(corosio::wait_type type)
{
    auto [ec] = co_await wait_socket_.wait(type);

    // The outer io_result intentionally reports success. The actual wait
    // error is the payload. This makes when_any return on the first
    // completion even when that completion itself reports an I/O error.
    co_return capy::io_result<std::error_code>{
        std::error_code{},
        ec,
    };
}

capy::io_task<> pg_connection::connect(std::string conninfo)
{
    close();
    error_message_.clear();

    connection_.reset(PQconnectStart(conninfo.c_str()));

    if (!connection_)
    {
        error_message_ = "PQconnectStart failed to allocate PGconn";
        co_return std::make_error_code(std::errc::not_enough_memory);
    }

    if (PQstatus(connection_.get()) == CONNECTION_BAD)
    {
        co_return set_libpq_error();
    }

    // PostgreSQL documents the first iteration as though
    // PQconnectPoll() had returned PGRES_POLLING_WRITING.
    auto status = PGRES_POLLING_WRITING;

    for (;;)
    {
        switch (status)
        {
            case PGRES_POLLING_READING:
            {
                if (auto ec = refresh_wait_socket())
                {
                    co_return ec;
                }

                auto [ec] = co_await wait_socket_.wait(corosio::wait_type::read);

                if (ec)
                {
                    error_message_ = ec.message();
                    co_return ec;
                }

                break;
            }

            case PGRES_POLLING_WRITING:
            {
                if (auto ec = refresh_wait_socket())
                {
                    co_return ec;
                }

                auto [ec] = co_await wait_socket_.wait(corosio::wait_type::write);

                if (ec)
                {
                    error_message_ = ec.message();
                    co_return ec;
                }

                break;
            }

            case PGRES_POLLING_ACTIVE:
                break;

            case PGRES_POLLING_FAILED:
                co_return set_libpq_error();

            case PGRES_POLLING_OK:
            {
                // Keep query execution explicitly nonblocking.
                if (PQsetnonblocking(connection_.get(), 1) != 0)
                {
                    co_return set_libpq_error();
                }

                // PQconnectPoll is allowed to replace the socket during
                // connection setup. Refresh once more after connection
                // completion so this duplicate tracks the final socket.
                if (auto ec = refresh_wait_socket())
                {
                    co_return ec;
                }

                co_return std::error_code{};
            }
        }

        status = PQconnectPoll(connection_.get());
    }
}

capy::io_task<> pg_connection::flush_output()
{
    for (;;)
    {
        int const result = PQflush(connection_.get());

        if (result == 0)
        {
            co_return std::error_code{};
        }

        if (result < 0)
        {
            co_return set_libpq_error();
        }

        // PQflush()==1 means output remains buffered.
        //
        // PostgreSQL requires us to react to both readability and
        // writability here. Reading matters because the server can be
        // blocked sending NOTICE or other data while we are waiting to
        // finish sending.
        auto ready = co_await capy::when_any(wait_event(corosio::wait_type::read), wait_event(corosio::wait_type::write));

        if (ready.index() == 0)
        {
            co_return std::get<0>(ready);
        }

        std::error_code ec;

        if (ready.index() == 1)
        {
            ec = std::get<1>(ready);
        }
        else
        {
            ec = std::get<2>(ready);
        }

        if (ec)
        {
            error_message_ = ec.message();
            co_return ec;
        }

        if (ready.index() == 1)
        {
            if (PQconsumeInput(connection_.get()) == 0)
            {
                co_return set_libpq_error();
            }
        }
    }
}

capy::io_task<PGresult*> pg_connection::next_result()
{
    while (PQisBusy(connection_.get()) != 0)
    {
        auto [ec] = co_await wait_socket_.wait(corosio::wait_type::read);

        if (ec)
        {
            error_message_ = ec.message();
            co_return capy::io_result<PGresult*>{
                ec,
                nullptr,
            };
        }

        if (PQconsumeInput(connection_.get()) == 0)
        {
            co_return capy::io_result<PGresult*>{
                set_libpq_error(),
                nullptr,
            };
        }
    }

    co_return capy::io_result<PGresult*>{
        std::error_code{},
        PQgetResult(connection_.get()),
    };
}

capy::io_task<std::string> pg_connection::execute_scalar(std::string query)
{
    error_message_.clear();

    if (!is_open())
    {
        error_message_ = "PostgreSQL connection is not open";

        co_return capy::io_result<std::string>{
            std::make_error_code(std::errc::not_connected),
            {},
        };
    }

    if (PQsendQueryParams(connection_.get(), query.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0) == 0)
    {
        co_return capy::io_result<std::string>{
            set_libpq_error(),
            {},
        };
    }

    {
        auto [ec] = co_await flush_output();

        if (ec)
        {
            co_return capy::io_result<std::string>{
                ec,
                {},
            };
        }
    }

    auto [ec, raw_result] = co_await next_result();
    if (ec)
    {
        co_return capy::io_result<std::string>{
            ec,
            {},
        };
    }

    if (!raw_result)
    {
        error_message_ = "PostgreSQL command returned no result";

        co_return capy::io_result<std::string>{
            std::make_error_code(std::errc::protocol_error),
            {},
        };
    }

    auto result = std::unique_ptr<PGresult, decltype(&PQclear)>(raw_result, &PQclear);

    std::error_code result_ec;
    std::string result_error_message;
    std::string value;

    if (PQresultStatus(result.get()) != PGRES_TUPLES_OK)
    {
        result_ec = make_libpq_error();
        result_error_message = PQresultErrorMessage(result.get());
    }
    else if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1 || PQgetisnull(result.get(), 0, 0))
    {
        result_ec = std::make_error_code(std::errc::protocol_error);
        result_error_message = "query did not return exactly one non-null value";
    }
    else
    {
        value = PQgetvalue(result.get(), 0, 0);
    }

    result.reset();

    bool unexpected_result = false;
    for (;;)
    {
        auto [next_ec, extra_result] = co_await next_result();
        if (next_ec)
        {
            co_return capy::io_result<std::string>{
                next_ec,
                {},
            };
        }

        if (!extra_result)
        {
            break;
        }

        unexpected_result = true;
        PQclear(extra_result);
    }

    if (result_ec)
    {
        error_message_ = std::move(result_error_message);
        co_return capy::io_result<std::string>{
            result_ec,
            {},
        };
    }

    if (unexpected_result)
    {
        error_message_ = "PostgreSQL command returned unexpected extra results";
        co_return capy::io_result<std::string>{
            std::make_error_code(std::errc::protocol_error),
            {},
        };
    }

    co_return capy::io_result<std::string>{
        std::error_code{},
        std::move(value),
    };
}
