#ifndef CHAT_SRC_PG_CONNECTION_HPP
#define CHAT_SRC_PG_CONNECTION_HPP

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <string_view>
#include <system_error>

#include <libpq-fe.h>
#include <boost/capy/io_task.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>

class pg_connection
{
   public:
    explicit pg_connection(boost::corosio::io_context& io_context);

    pg_connection(pg_connection const&) = delete;
    pg_connection& operator=(pg_connection const&) = delete;

    boost::capy::io_task<> connect(std::string conninfo);

    boost::capy::io_task<std::string> execute_scalar(std::string query, std::vector<std::string> parameters = {});

    boost::capy::io_task<std::optional<std::vector<std::string>>> execute_row(
        std::string query, std::vector<std::string> parameters = {});

    void close() noexcept;

    bool is_open() const noexcept;

    std::string_view error_message() const noexcept;

   private:
    struct pg_conn_deleter
    {
        void operator()(PGconn* connection) const noexcept;
    };

    using pg_conn_ptr = std::unique_ptr<PGconn, pg_conn_deleter>;

    std::error_code refresh_wait_socket();

    boost::capy::io_task<std::error_code> wait_event(boost::corosio::wait_type type);

    boost::capy::io_task<> flush_output();

    boost::capy::io_task<PGresult*> next_result();

    std::error_code set_libpq_error();

   private:
    pg_conn_ptr connection_;
    boost::corosio::tcp_socket wait_socket_;
    std::string error_message_;
};

#endif
