#ifndef CHAT_TUI_DEADLINE_HPP
#define CHAT_TUI_DEADLINE_HPP

#include "inbox.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace chat::tui
{

// One blocking worker serves the app's next deadline (typing or reconnect).
// Expiration only queues a task; the callback always runs in inbox::drain().
class deadline_timer
{
public:
    using clock = std::chrono::steady_clock;

    deadline_timer(std::shared_ptr<inbox> destination, inbox::task callback)
        : state_(std::make_shared<state>(std::move(destination), std::move(callback))),
          worker_([value = state_] { run(value); })
    {
    }

    ~deadline_timer() { stop(); }
    deadline_timer(deadline_timer const&) = delete;
    deadline_timer& operator=(deadline_timer const&) = delete;

    void schedule(std::optional<clock::time_point> deadline)
    {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopped) { return; }
            state_->deadline = deadline;
            ++state_->generation;
        }
        state_->changed.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard lock(state_->mutex);
            state_->stopped = true;
            state_->deadline.reset();
            ++state_->generation;
        }
        state_->changed.notify_one();
        if (worker_.joinable()) { worker_.join(); }
    }

private:
    struct state
    {
        state(std::shared_ptr<inbox> target, inbox::task action)
            : destination(std::move(target)), callback(std::move(action)) {}
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<clock::time_point> deadline;
        std::uint64_t generation = 0;
        bool stopped = false;
        std::weak_ptr<inbox> destination;
        inbox::task callback;
    };

    static void run(std::shared_ptr<state> const& value)
    {
        std::unique_lock lock(value->mutex);
        while (!value->stopped)
        {
            if (!value->deadline)
            {
                value->changed.wait(lock, [&] { return value->stopped || value->deadline.has_value(); });
                continue;
            }
            auto const generation = value->generation;
            auto const deadline = *value->deadline;
            if (value->changed.wait_until(lock, deadline,
                    [&] { return value->stopped || value->generation != generation; }))
            {
                continue;
            }
            value->deadline.reset();
            lock.unlock();
            if (auto destination = value->destination.lock())
            {
                destination->post([value, generation] {
                    inbox::task callback;
                    {
                        std::lock_guard guard(value->mutex);
                        if (value->stopped || value->generation != generation) { return; }
                        callback = value->callback;
                    }
                    callback();
                });
            }
            lock.lock();
        }
    }

    std::shared_ptr<state> state_;
    std::jthread worker_;
};

} // namespace chat::tui

#endif
