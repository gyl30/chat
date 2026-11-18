#ifndef CHAT_SRC_DATABASE_HPP
#define CHAT_SRC_DATABASE_HPP

#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/task.hpp>

#include "sqlite_database.hpp"

// Owns the single database thread, its bounded job queue, and the one
// SQLite connection used by that thread. Coroutines submit closures and
// resume on their own executor; the network event loop never runs SQLite.
class database
{
   public:
    using pointer = std::unique_ptr<database>;

    static std::expected<pointer, sqlite_error> open(std::string path, std::size_t queue_capacity = 1024);

    database(database const&) = delete;
    database& operator=(database const&) = delete;
    ~database();

    // The callable runs on the database thread and must return
    // std::expected<T, sqlite_error>. A full or stopped queue is reported
    // through the same error channel.
    template <class F>
        requires std::same_as<typename std::invoke_result_t<F, sqlite_database&>::error_type, sqlite_error>
    auto run(F&& job) -> boost::capy::task<std::invoke_result_t<F, sqlite_database&>>
    {
        using result_type = std::invoke_result_t<F, sqlite_database&>;
        auto state = std::make_shared<run_state<result_type>>();
        state->job = std::forward<F>(job);
        auto result = co_await run_awaiter<result_type>{this, state};
        co_return std::move(result);
    }

    // Stops accepting jobs and drains the queue. The destructor calls it.
    void shutdown() noexcept;

    [[nodiscard]] std::string const& path() const noexcept { return path_; }

    // Number of jobs waiting for the database thread. A running job is not
    // included. Intended for diagnostics and deterministic tests.
    [[nodiscard]] std::size_t pending_jobs() const
    {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

   private:
    database() = default;

    template <class R> struct run_state;

    template <class R> class run_awaiter;

    template <class R> friend class run_awaiter;

    std::optional<sqlite_error> enqueue(std::function<void()> job);

    void worker();

    std::string path_;
    std::size_t queue_capacity_ = 0;
    sqlite_database database_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
};

template <class R> struct database::run_state
{
    std::function<R(sqlite_database&)> job;
    std::optional<R> result;
    boost::capy::continuation continuation;
    boost::capy::executor_ref executor;
};

template <class R> class database::run_awaiter
{
   public:
    run_awaiter(database* owner, std::shared_ptr<run_state<R>> state) noexcept
        : owner_(owner), state_(std::move(state))
    {
    }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle, boost::capy::io_env const* environment)
    {
        state_->executor = environment->executor;
        state_->continuation.h = handle;

        auto error = owner_->enqueue(
            [owner = owner_, state = state_]
            {
                // The job must never throw; all operations report through
                // std::expected.
                state->result.emplace(state->job(owner->database_));
                state->executor.post(state->continuation);
            });
        if (error)
        {
            state_->result.emplace(std::unexpected(std::move(*error)));
            return handle;
        }
        return std::noop_coroutine();
    }

    R await_resume() { return std::move(*state_->result); }

   private:
    database* owner_;
    std::shared_ptr<run_state<R>> state_;
};

#endif
