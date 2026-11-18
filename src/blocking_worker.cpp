#include "blocking_worker.hpp"

blocking_worker::blocking_worker(std::size_t capacity, std::size_t parallelism) : capacity_(capacity)
{
    threads_.reserve(parallelism);
    for (std::size_t i = 0; i < parallelism; ++i)
    {
        threads_.emplace_back([this](std::stop_token stop) { execute(stop); });
    }
}

blocking_worker::~blocking_worker() { shutdown(); }

void blocking_worker::shutdown() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
        {
            return;
        }
        stopping_ = true;
    }
    available_.notify_all();
    for (auto& thread : threads_)
    {
        thread.join();
    }
}

std::size_t blocking_worker::pending_jobs() const
{
    std::lock_guard lock(mutex_);
    return queue_.size();
}

std::optional<blocking_worker::unavailable> blocking_worker::enqueue(std::move_only_function<void()> job)
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
        {
            return unavailable::stopped;
        }
        if (queue_.size() >= capacity_)
        {
            return unavailable::full;
        }
        queue_.push_back(std::move(job));
    }
    available_.notify_one();
    return std::nullopt;
}

void blocking_worker::execute(std::stop_token stop)
{
    for (;;)
    {
        std::move_only_function<void()> job;
        {
            std::unique_lock lock(mutex_);
            available_.wait(lock, stop, [this] { return stopping_ || !queue_.empty(); });
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
