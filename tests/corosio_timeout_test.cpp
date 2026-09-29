#include <chrono>
#include <iostream>

#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/timeout.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_stream_socket.hpp>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

using namespace std::chrono_literals;

namespace
{

capy::task<> cancel_after_delay(corosio::local_stream_socket& socket)
{
    auto [ec] = co_await corosio::delay(20ms);
    if (!ec)
    {
        socket.cancel();
    }
}

capy::task<int> run_tests(corosio::io_context& io_context)
{
    int failures = 0;

    corosio::local_stream_socket timeout_reader(io_context);
    corosio::local_stream_socket timeout_writer(io_context);
    if (auto ec = corosio::connect_pair(timeout_reader, timeout_writer))
    {
        std::cerr << "FAIL timeout socket pair: " << ec.message() << '\n';
        co_return 1;
    }

    auto [timeout_ec] = co_await corosio::timeout(timeout_reader.wait(corosio::wait_type::read), 50ms);
    if (timeout_ec != capy::cond::timeout)
    {
        std::cerr << "FAIL wait timeout: " << timeout_ec.message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS wait timeout\n";
    }

    auto [timeout_write_ec, timeout_written] = co_await timeout_writer.write_some(capy::const_buffer("x", 1));
    if (timeout_write_ec || timeout_written != 1)
    {
        std::cerr << "FAIL write after timeout: " << timeout_write_ec.message() << '\n';
        ++failures;
    }
    else
    {
        auto [ready_ec] = co_await timeout_reader.wait(corosio::wait_type::read);
        if (ready_ec)
        {
            std::cerr << "FAIL wait after timeout: " << ready_ec.message() << '\n';
            ++failures;
        }
        else
        {
            char value = 0;
            auto [read_ec, read_size] = co_await timeout_reader.read_some(capy::mutable_buffer(&value, 1));
            if (read_ec || read_size != 1 || value != 'x')
            {
                std::cerr << "FAIL socket reuse after timeout\n";
                ++failures;
            }
            else
            {
                std::cout << "PASS socket reuse after timeout\n";
            }
        }
    }

    corosio::local_stream_socket cancel_reader(io_context);
    corosio::local_stream_socket cancel_writer(io_context);
    if (auto ec = corosio::connect_pair(cancel_reader, cancel_writer))
    {
        std::cerr << "FAIL cancel socket pair: " << ec.message() << '\n';
        co_return 1;
    }

    capy::run_async(io_context.get_executor())(cancel_after_delay(cancel_reader));

    auto [cancel_ec] = co_await cancel_reader.wait(corosio::wait_type::read);
    if (cancel_ec != capy::cond::canceled)
    {
        std::cerr << "FAIL wait cancel: " << cancel_ec.message() << '\n';
        ++failures;
    }
    else
    {
        std::cout << "PASS wait cancel\n";
    }

    auto [cancel_write_ec, cancel_written] = co_await cancel_writer.write_some(capy::const_buffer("y", 1));
    if (cancel_write_ec || cancel_written != 1)
    {
        std::cerr << "FAIL write after cancel: " << cancel_write_ec.message() << '\n';
        ++failures;
    }
    else
    {
        auto [ready_ec] = co_await cancel_reader.wait(corosio::wait_type::read);
        if (ready_ec)
        {
            std::cerr << "FAIL wait after cancel: " << ready_ec.message() << '\n';
            ++failures;
        }
        else
        {
            char value = 0;
            auto [read_ec, read_size] = co_await cancel_reader.read_some(capy::mutable_buffer(&value, 1));
            if (read_ec || read_size != 1 || value != 'y')
            {
                std::cerr << "FAIL socket reuse after cancel\n";
                ++failures;
            }
            else
            {
                std::cout << "PASS socket reuse after cancel\n";
            }
        }
    }

    if (failures != 0)
    {
        std::cerr << failures << " Corosio timeout validation test(s) failed\n";
        co_return 1;
    }

    std::cout << "PASS Corosio timeout validation\n";
    co_return 0;
}

}    // namespace

int main()
{
    corosio::io_context io_context;
    int exit_code = 1;

    capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(run_tests(io_context));

    io_context.run();
    return exit_code;
}
