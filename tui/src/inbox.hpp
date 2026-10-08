#ifndef CHAT_TUI_INBOX_HPP
#define CHAT_TUI_INBOX_HPP

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace chat::tui
{

// Producers only enqueue values captured by tasks. The UI thread owns drain().
// The notifier must not reenter this inbox: it runs under the lock so stop()
// forms a lifetime barrier for an external screen wakeup target.
class inbox
{
public:
    using task = std::function<void()>;

    explicit inbox(task notify = {}) : notify_(std::move(notify)) {}
    inbox(inbox const&) = delete;
    inbox& operator=(inbox const&) = delete;

    bool post(task action)
    {
        std::lock_guard lock(mutex_);
        if (stopped_) { return false; }
        tasks_.push_back(std::move(action));
        if (notify_) { notify_(); }
        return true;
    }

    // A bounded batch lets the event loop render even when producers are busy.
    std::size_t drain()
    {
        std::deque<task> batch;
        {
            std::lock_guard lock(mutex_);
            if (stopped_) { return 0; }
            batch.swap(tasks_);
        }
        std::size_t count = 0;
        for (auto& action : batch)
        {
            {
                std::lock_guard lock(mutex_);
                if (stopped_) { break; }
            }
            action();
            ++count;
        }
        return count;
    }

    bool empty()
    {
        std::lock_guard lock(mutex_);
        return tasks_.empty();
    }

    // Call on the UI thread before destroying the screen or client. A task
    // already executing in drain() completes normally.
    void stop()
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        tasks_.clear();
        notify_ = {};
    }

private:
    std::mutex mutex_;
    std::deque<task> tasks_;
    task notify_;
    bool stopped_ = false;
};

} // namespace chat::tui

#endif
