#ifndef CHAT_SRC_BLOCKING_WORKER_HPP
#define CHAT_SRC_BLOCKING_WORKER_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/task.hpp>

// One thread owns execution of a finite queue. A submitted operation completes
// on its caller's executor, which must outlive the operation.
class blocking_worker
{
   public:
    explicit blocking_worker(std::size_t capacity);
    blocking_worker(blocking_worker const&) = delete;
    blocking_worker& operator=(blocking_worker const&) = delete;
    ~blocking_worker();

    enum class unavailable { full, stopped };

    template <class F, class Translate>
    auto run(F job, Translate translate) -> boost::capy::task<std::invoke_result_t<F&>>
    {
        using result_type = std::invoke_result_t<F&>;
        struct operation
        {
            blocking_worker& owner;
            F& job;
            Translate& translate;
            std::optional<result_type> result;
            boost::capy::continuation continuation;
            boost::capy::executor_ref executor;

            bool await_ready() const noexcept { return false; }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle, boost::capy::io_env const* environment)
            {
                executor = environment->executor;
                continuation.h = handle;
                auto error = owner.enqueue([this] {
                    result.emplace(job());
                    executor.post(continuation);
                });
                if (error)
                {
                    result.emplace(std::unexpected(translate(*error)));
                    return handle;
                }
                return std::noop_coroutine();
            }

            result_type await_resume() { return std::move(*result); }
        };
        co_return co_await operation{*this, job, translate, {}, {}, {}};
    }

    // The owner calls this after ceasing submission. Accepted work is drained.
    void shutdown() noexcept;
    std::size_t pending_jobs() const;

   private:
    std::optional<unavailable> enqueue(std::move_only_function<void()> job);
    void execute();

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<std::move_only_function<void()>> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

#endif
