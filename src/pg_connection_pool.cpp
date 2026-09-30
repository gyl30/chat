#include <tuple>
#include <utility>

#include "pg_connection_pool.hpp"

pg_connection_pool::lease::lease(pg_connection_pool& pool, std::size_t index, std::error_code error) noexcept
    : pool_(&pool), index_(index), error_(error)
{
}

pg_connection_pool::lease::lease(std::error_code error) noexcept : error_(error) {}

pg_connection_pool::lease::lease(lease&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_), error_(other.error_)
{
}

pg_connection_pool::lease& pg_connection_pool::lease::operator=(lease&& other) noexcept
{
    if (this != &other)
    {
        if (pool_)
        {
            pool_->release(index_);
        }
        pool_ = std::exchange(other.pool_, nullptr);
        index_ = other.index_;
        error_ = other.error_;
    }
    return *this;
}

pg_connection_pool::lease::~lease()
{
    if (pool_)
    {
        pool_->release(index_);
    }
}

std::error_code pg_connection_pool::lease::error() const noexcept { return error_; }

pg_connection& pg_connection_pool::lease::connection() noexcept { return *pool_->connections_[index_]; }

pg_connection_pool::pg_connection_pool(boost::corosio::io_context& io_context,
                                       std::string connection_string,
                                       std::size_t connection_count)
    : connection_string_(std::move(connection_string))
{
    connections_.reserve(connection_count);
    for (std::size_t index = 0; index < connection_count; ++index)
    {
        connections_.push_back(std::make_unique<pg_connection>(io_context));
        available_.push_back(index);
    }
    if (!available_.empty())
    {
        available_event_.set();
    }
}

boost::capy::task<pg_connection_pool::lease> pg_connection_pool::acquire()
{
    if (connections_.empty())
    {
        co_return lease(std::make_error_code(std::errc::invalid_argument));
    }

    while (available_.empty())
    {
        available_event_.clear();
        auto wait_result = co_await available_event_.wait();
        auto& [wait_ec] = wait_result;
        if (wait_ec)
        {
            co_return lease(wait_ec);
        }
    }

    auto const index = available_.front();
    available_.pop_front();
    if (available_.empty())
    {
        available_event_.clear();
    }

    auto& connection = *connections_[index];
    std::error_code connect_ec;
    if (!connection.is_open())
    {
        auto connect_result = co_await connection.connect(connection_string_);
        connect_ec = std::get<0>(connect_result);
    }

    co_return lease(*this, index, connect_ec);
}

void pg_connection_pool::release(std::size_t index) noexcept
{
    available_.push_back(index);
    available_event_.set();
}
