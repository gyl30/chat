#include "blocking_worker.hpp"

#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

blocking_worker::blocking_worker(std::size_t capacity, std::size_t parallelism) : capacity_(capacity)
{
    threads_.reserve(parallelism);
    for (std::size_t i = 0; i < parallelism; ++i)
    {
        threads_.emplace_back([this](std::stop_token stop) { execute(stop); });
    }
}

blocking_worker::~blocking_worker()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    available_.notify_all();
    for (auto& thread : threads_)
    {
        thread.join();
    }
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
#ifndef _WIN32
    // An interrupted TLS write (including close_notify) must return EPIPE to
    // the transfer job. Leave the embedding application's signal policy alone.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#endif
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
