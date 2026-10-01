#include <tuple>
#include <utility>
#include <string>
#include <vector>
#include <cstdlib>
#include <iostream>

#include <boost/capy/task.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>

#include "pg_connection.hpp"
#include "pg_connection_pool.hpp"

namespace
{

constexpr auto connection_string =
    "hostaddr=172.20.54.83 "
    "port=5432 "
    "dbname=chat "
    "user=chat "
    "sslmode=disable";

boost::capy::task<int> run_tests(boost::corosio::io_context& io_context)
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

    auto first_result = co_await connection.execute_scalar("SELECT 1");
    auto& [first_ec, first_value] = first_result;
    if (first_ec || first_value != "1")
    {
        std::cerr << "FAIL first query: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS first query\n";
    }

    auto second_result = co_await connection.execute_scalar("SELECT 2");
    auto& [second_ec, second_value] = second_result;
    if (second_ec || second_value != "2")
    {
        std::cerr << "FAIL sequential query: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS sequential query\n";
    }

    std::string const parameter_value = "Robert'); SELECT pg_sleep(10); --";
    auto parameter_query_result = co_await connection.execute_scalar("SELECT $1::text", {parameter_value});
    auto& [parameter_ec, parameter_result] = parameter_query_result;
    if (parameter_ec || parameter_result != parameter_value)
    {
        std::cerr << "FAIL parameterized query: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS parameterized query\n";
    }

    auto multiple_parameters_query_result = co_await connection.execute_scalar("SELECT $1::text || ':' || $2::text", {"left", "right"});
    auto& [multiple_parameters_ec, multiple_parameters_result] = multiple_parameters_query_result;
    if (multiple_parameters_ec || multiple_parameters_result != "left:right")
    {
        std::cerr << "FAIL multiple query parameters: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS multiple query parameters\n";
    }

    auto empty_parameter_query_result = co_await connection.execute_scalar("SELECT length($1::text)::text", {""});
    auto& [empty_parameter_ec, empty_parameter_result] = empty_parameter_query_result;
    if (empty_parameter_ec || empty_parameter_result != "0")
    {
        std::cerr << "FAIL empty query parameter: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS empty query parameter\n";
    }

    auto row_result = co_await connection.execute_row("SELECT $1::text, $2::text", {"left", "right"});
    auto& [row_ec, row] = row_result;
    if (row_ec || !row || *row != std::vector<std::string>{"left", "right"})
    {
        std::cerr << "FAIL row query: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS row query\n";
    }

    auto missing_row_result = co_await connection.execute_row("SELECT 1::text, 2::text WHERE false");
    auto& [missing_row_ec, missing_row] = missing_row_result;
    if (missing_row_ec || missing_row)
    {
        std::cerr << "FAIL missing row query: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS missing row query\n";
    }

    auto multiple_rows_result = co_await connection.execute_row("SELECT generate_series(1, 2)::text");
    if (!std::get<0>(multiple_rows_result))
    {
        std::cerr << "FAIL multiple rows were accepted\n";
        ++failures;
    }
    else
    {
        std::cout << "PASS multiple rows rejected\n";
    }

    auto null_row_result = co_await connection.execute_row("SELECT NULL::text");
    if (!std::get<0>(null_row_result))
    {
        std::cerr << "FAIL null row value was accepted\n";
        ++failures;
    }
    else
    {
        std::cout << "PASS null row value rejected\n";
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

    auto recovery_result = co_await connection.execute_scalar("SELECT 3");
    auto& [recovery_ec, recovery_value] = recovery_result;
    if (recovery_ec || recovery_value != "3")
    {
        std::cerr << "FAIL query after SQL error: " << connection.error_message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS query after SQL error\n";
    }

    for (auto const* command : {"BEGIN", "SELECT missing FROM __chat_poc_missing_table__", "ROLLBACK", "BEGIN", "COMMIT"})
    {
        auto result = co_await connection.execute_row(command);
        bool const sql_error = std::string_view(command).starts_with("SELECT");
        if (static_cast<bool>(std::get<0>(result)) != sql_error || std::get<1>(result))
        {
            std::cerr << "FAIL transaction command: " << command << '\n';
            ++failures;
        }
    }
    auto after_transaction = co_await connection.execute_scalar("SELECT 5");
    if (std::get<0>(after_transaction) || std::get<1>(after_transaction) != "5")
    {
        std::cerr << "FAIL query after transaction rollback\n";
        ++failures;
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
        auto pid_result = co_await terminated_connection.execute_scalar("SELECT pg_backend_pid()");
        auto& [pid_ec, pid] = pid_result;
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
                auto terminate_result = co_await terminator_connection.execute_scalar(std::move(terminate_query));
                auto& [terminate_ec, terminate_value] = terminate_result;
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

    pg_connection_pool pool(io_context, connection_string, 1);
    std::string pooled_backend_pid;
    {
        auto lease = co_await pool.acquire();
        if (lease.error())
        {
            std::cerr << "FAIL pooled connection acquire\n";
            ++failures;
        }
        else
        {
            auto pid_result = co_await lease.connection().execute_scalar("SELECT pg_backend_pid()::text");
            auto& [pid_ec, pid] = pid_result;
            if (pid_ec)
            {
                std::cerr << "FAIL pooled connection query: " << lease.connection().error_message() << '\n';
                ++failures;
            }
            else
            {
                pooled_backend_pid = std::move(pid);
                std::cout << "PASS pooled connection acquire\n";
            }
        }
    }

    {
        auto lease = co_await pool.acquire();
        if (lease.error())
        {
            std::cerr << "FAIL pooled connection reacquire\n";
            ++failures;
        }
        else
        {
            auto pid_result = co_await lease.connection().execute_scalar("SELECT pg_backend_pid()::text");
            auto& [pid_ec, pid] = pid_result;
            if (pid_ec || pid != pooled_backend_pid)
            {
                std::cerr << "FAIL pooled connection reuse: " << lease.connection().error_message() << '\n';
                ++failures;
            }
            else
            {
                std::cout << "PASS pooled connection reuse\n";
            }
        }
    }

    boost::capy::async_event holder_ready;
    boost::capy::async_event waiter_ready;
    boost::capy::async_event release_holder;
    bool waiter_acquired = false;

    auto holder = [&]() -> boost::capy::io_task<> {
        {
            auto lease = co_await pool.acquire();
            if (lease.error())
            {
                co_return lease.error();
            }
            holder_ready.set();
            auto release_result = co_await release_holder.wait();
            auto& [release_ec] = release_result;
            if (release_ec)
            {
                co_return release_ec;
            }
        }
        co_return std::error_code{};
    };

    auto waiter = [&]() -> boost::capy::io_task<> {
        auto ready_result = co_await holder_ready.wait();
        auto& [ready_ec] = ready_result;
        if (ready_ec)
        {
            co_return ready_ec;
        }
        waiter_ready.set();
        auto lease = co_await pool.acquire();
        if (lease.error())
        {
            co_return lease.error();
        }
        waiter_acquired = true;
        co_return std::error_code{};
    };

    auto releaser = [&]() -> boost::capy::io_task<> {
        auto ready_result = co_await waiter_ready.wait();
        auto& [ready_ec] = ready_result;
        if (ready_ec)
        {
            co_return ready_ec;
        }
        release_holder.set();
        co_return std::error_code{};
    };

    auto wait_result = co_await boost::capy::when_all(holder(), waiter(), releaser());
    if (std::get<0>(wait_result) || !waiter_acquired)
    {
        std::cerr << "FAIL pooled connection wait\n";
        ++failures;
    }
    else
    {
        std::cout << "PASS pooled connection wait\n";
    }

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

    boost::corosio::io_context io_context;
    int exit_code = 1;

    boost::capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(run_tests(io_context));

    io_context.run();
    return exit_code;
}
