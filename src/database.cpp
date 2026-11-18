#include <utility>

#include "database.hpp"
#include "schema.hpp"

std::expected<database::pointer, sqlite_error> database::open(std::string path, std::size_t queue_capacity)
{
    if (queue_capacity == 0)
    {
        return std::unexpected(sqlite_error{1, "database queue capacity must be positive"});
    }

    auto opened = sqlite_database::open(path);
    if (!opened)
    {
        return std::unexpected(std::move(opened.error()));
    }
    if (auto error = initialize_schema(*opened); error)
    {
        return std::unexpected(std::move(error));
    }

    auto result = pointer(new database());
    result->path_ = std::move(path);
    result->queue_capacity_ = queue_capacity;
    result->database_ = std::move(*opened);
    result->thread_ = std::thread([owner = result.get()] { owner->worker(); });
    return result;
}

database::~database() { shutdown(); }

void database::shutdown() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
        {
            return;
        }
        stopping_ = true;
    }
    condition_.notify_all();

    if (thread_.joinable())
    {
        thread_.join();
    }
}

std::optional<sqlite_error> database::enqueue(std::function<void()> job)
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
        {
            return sqlite_error{1, "database is shut down"};
        }
        if (queue_.size() >= queue_capacity_)
        {
            return sqlite_error{2, "database queue is full"};
        }
        queue_.push_back(std::move(job));
    }
    condition_.notify_one();
    return std::nullopt;
}

void database::worker()
{
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty())
            {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job();
    }
}
