#include <exception>
#include <iostream>
#include <string_view>
#include <system_error>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/http/bcrypt.hpp>

namespace
{

boost::capy::task<int> run_tests()
{
    namespace bcrypt = boost::http::bcrypt;

    constexpr auto password = "correct horse battery staple";

    try
    {
        auto hash = co_await bcrypt::hash_async(password, 4, bcrypt::version::v2b);
        std::string_view hash_view(hash.data(), hash.size());

        if (hash_view.size() != 60 || !hash_view.starts_with("$2b$04$"))
        {
            std::cerr << "FAIL bcrypt hash format\n";
            co_return 1;
        }
        std::cout << "PASS bcrypt hash format\n";

        if (!co_await bcrypt::compare_async(password, hash.str()))
        {
            std::cerr << "FAIL bcrypt password match\n";
            co_return 1;
        }
        std::cout << "PASS bcrypt password match\n";

        if (co_await bcrypt::compare_async("wrong password", hash.str()))
        {
            std::cerr << "FAIL bcrypt password mismatch\n";
            co_return 1;
        }
        std::cout << "PASS bcrypt password mismatch\n";

        std::error_code ec;
        if (bcrypt::compare(password, "invalid", ec) || ec != bcrypt::error::invalid_hash)
        {
            std::cerr << "FAIL bcrypt invalid hash\n";
            co_return 1;
        }
        std::cout << "PASS bcrypt invalid hash\n";

        ec.clear();
        if (bcrypt::get_rounds(hash.str(), ec) != 4 || ec)
        {
            std::cerr << "FAIL bcrypt cost factor\n";
            co_return 1;
        }
        std::cout << "PASS bcrypt cost factor\n";
    }
    catch (std::exception const& e)
    {
        std::cerr << "FAIL bcrypt exception: " << e.what() << '\n';
        co_return 1;
    }

    std::cout << "PASS bcrypt validation\n";
    co_return 0;
}

}

int main()
{
    boost::corosio::io_context io_context;
    int exit_code = 1;

    boost::capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(run_tests());

    io_context.run();
    return exit_code;
}
