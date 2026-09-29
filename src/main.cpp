#include "pg_connection.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>

#include <exception>
#include <iostream>
#include <string>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

capy::task<int> run_poc(corosio::io_context& io_context)
{
    pg_connection connection(io_context);

    auto [connect_ec] = co_await connection.connect(
        "hostaddr=172.20.54.83 "
        "port=5432 "
        "dbname=chat "
        "user=chat "
        "sslmode=disable");

    if (connect_ec)
    {
        std::cerr << "connect failed: " << connect_ec.message() << ": " << connection.error_message() << '\n';

        co_return 1;
    }

    std::cout << "connected to 172.20.54.83:5432/chat\n";

    auto [query_ec, value] = co_await connection.execute_scalar("SELECT 1");

    if (query_ec)
    {
        std::cerr << "query failed: " << query_ec.message() << ": " << connection.error_message() << '\n';

        co_return 1;
    }

    std::cout << "SELECT 1 -> " << value << '\n';

    connection.close();

    co_return 0;
}

int main()
{
    corosio::io_context io_context;

    int exit_code = 1;
    std::exception_ptr exception;

    capy::run_async(
        io_context.get_executor(), [&exit_code](int result) { exit_code = result; }, [&exception](std::exception_ptr error) { exception = error; })(
        run_poc(io_context));

    io_context.run();

    if (exception)
    {
        try
        {
            std::rethrow_exception(exception);
        }
        catch (std::exception const& error)
        {
            std::cerr << "unexpected exception: " << error.what() << '\n';
        }
        catch (...)
        {
            std::cerr << "unexpected non-standard exception\n";
        }

        return 1;
    }

    return exit_code;
}
