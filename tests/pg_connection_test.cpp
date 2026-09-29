#include "pg_connection.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

namespace
{

constexpr auto connection_string =
    "hostaddr=172.20.54.83 "
    "port=5432 "
    "dbname=chat "
    "user=chat "
    "sslmode=disable";

capy::task<int> run_tests(corosio::io_context& io_context)
{
    int failures = 0;

    pg_connection connection(io_context);
    auto [connect_ec] = co_await connection.connect(connection_string);
    if (connect_ec)
    {
        std::cerr << "FAIL connect: " << connection.error_message() << '\n';
        co_return 1;
    }
    std::cout << "PASS connect\n";

    auto [first_ec, first_value] = co_await connection.execute_scalar("SELECT 1");
    if (first_ec || first_value != "1")
    {
        std::cerr << "FAIL first query: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS first query\n";
    }

    auto [second_ec, second_value] = co_await connection.execute_scalar("SELECT 2");
    if (second_ec || second_value != "2")
    {
        std::cerr << "FAIL sequential query: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS sequential query\n";
    }

    auto sql_result = co_await connection.execute_scalar("SELECT * FROM __chat_poc_missing_table__");
    if (!std::get<0>(sql_result))
    {
        std::cerr << "FAIL SQL error was not reported\n";
        ++failures;
    }
    else
    {
        std::cout << "PASS SQL error\n";
    }

    auto [recovery_ec, recovery_value] = co_await connection.execute_scalar("SELECT 3");
    if (recovery_ec || recovery_value != "3")
    {
        std::cerr << "FAIL query after SQL error: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS query after SQL error\n";
    }

    pg_connection bad_password_connection(io_context);
    auto [bad_password_ec] = co_await bad_password_connection.connect(
        "hostaddr=172.20.54.83 "
        "port=5432 "
        "dbname=chat "
        "user=chat "
        "password=__chat_invalid_password__ "
        "sslmode=disable");
    if (!bad_password_ec)
    {
        std::cerr << "FAIL invalid password was accepted\n";
        ++failures;
    }
    else
    {
        std::cout << "PASS invalid password\n";
    }

    pg_connection missing_database_connection(io_context);
    auto [missing_database_ec] = co_await missing_database_connection.connect(
        "hostaddr=172.20.54.83 "
        "port=5432 "
        "dbname=__chat_poc_missing_database__ "
        "user=chat "
        "sslmode=disable");
    if (!missing_database_ec)
    {
        std::cerr << "FAIL missing database was accepted\n";
        ++failures;
    }
    else
    {
        std::cout << "PASS missing database\n";
    }

    pg_connection terminated_connection(io_context);
    auto [terminated_connect_ec] = co_await terminated_connection.connect(connection_string);
    if (terminated_connect_ec)
    {
        std::cerr << "FAIL disconnect test connect: " << terminated_connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        auto [pid_ec, pid] = co_await terminated_connection.execute_scalar("SELECT pg_backend_pid()");
        if (pid_ec)
        {
            std::cerr << "FAIL backend pid query: " << terminated_connection.error_message() << '\n';
            ++failures;
        }
        else
        {
            pg_connection terminator_connection(io_context);
            auto [terminator_connect_ec] = co_await terminator_connection.connect(connection_string);
            if (terminator_connect_ec)
            {
                std::cerr << "FAIL terminator connect: " << terminator_connection.error_message() << '\n';
                ++failures;
            }
            else
            {
                std::string terminate_query = "SELECT pg_terminate_backend(" + pid + ", 5000)";
                auto [terminate_ec, terminate_value] = co_await terminator_connection.execute_scalar(std::move(terminate_query));
                if (terminate_ec || terminate_value != "t")
                {
                    std::cerr << "FAIL terminate backend: " << terminator_connection.error_message() << '\n';
                    ++failures;
                }
                else
                {
                    std::cout << "PASS terminate backend\n";

                    auto disconnected_result = co_await terminated_connection.execute_scalar("SELECT 4");
                    if (!std::get<0>(disconnected_result))
                    {
                        std::cerr << "FAIL query after server disconnect succeeded\n";
                        ++failures;
                    }
                    else
                    {
                        std::cout << "PASS server disconnect\n";
                    }

                    if (terminated_connection.is_open())
                    {
                        std::cerr << "FAIL disconnected connection remains open\n";
                        ++failures;
                    }
                    else
                    {
                        std::cout << "PASS disconnected connection state\n";
                    }

                    auto retry_result = co_await terminated_connection.execute_scalar("SELECT 5");
                    if (!std::get<0>(retry_result))
                    {
                        std::cerr << "FAIL second query after server disconnect succeeded\n";
                        ++failures;
                    }
                    else
                    {
                        std::cout << "PASS query after disconnected state\n";
                    }
                }
            }
        }
    }

    connection.close();

    if (failures != 0)
    {
        std::cerr << failures << " PostgreSQL validation test(s) failed\n";
        co_return 1;
    }

    std::cout << "PASS PostgreSQL validation\n";
    co_return 0;
}

}    // namespace

int main()
{
    if (std::getenv("PGPASSWORD") == nullptr)
    {
        std::cerr << "PGPASSWORD is required\n";
        return 1;
    }

    corosio::io_context io_context;
    int exit_code = 1;

    capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(run_tests(io_context));

    io_context.run();
    return exit_code;
}
