#include "deadline.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace
{
void check(bool value, char const* description)
{
    if (!value) { std::cerr << "FAIL " << description << '\n'; std::exit(1); }
}

struct wakeup
{
    std::mutex mutex;
    std::condition_variable changed;
    int count = 0;
    void notify()
    {
        { std::lock_guard lock(mutex); ++count; }
        changed.notify_all();
    }
    void wait(int expected)
    {
        std::unique_lock lock(mutex);
        check(changed.wait_for(lock, 2s, [&] { return count >= expected; }), "deadline woke UI");
    }
};
}

int main()
{
    auto const ui_thread = std::this_thread::get_id();
    std::thread::id producer_thread;
    std::thread::id wake_thread;
    std::thread::id action_thread;
    auto queue = std::make_shared<chat::tui::inbox>([&] { wake_thread = std::this_thread::get_id(); });
    std::jthread producer([&] {
        producer_thread = std::this_thread::get_id();
        check(queue->post([&] { action_thread = std::this_thread::get_id(); }), "producer post accepted");
    });
    producer.join();
    check(action_thread == std::thread::id{}, "producer never executes action");
    check(wake_thread == producer_thread, "notifier runs on producer");
    check(queue->drain() == 1 && action_thread == ui_thread, "action executes on UI");
    check(queue->drain() == 0, "task executes once");

    int batches = 0;
    queue->post([&] { ++batches; queue->post([&] { ++batches; }); });
    check(queue->drain() == 1 && batches == 1, "drain bounds producer batch");
    check(queue->drain() == 1 && batches == 2, "next event drains new batch");

    for (int iteration = 0; iteration < 100; ++iteration)
    {
        std::atomic_int notified = 0;
        std::atomic_int executed = 0;
        auto racing = std::make_shared<chat::tui::inbox>([&] { ++notified; });
        std::atomic_bool begin = false;
        std::jthread writer([&] {
            while (!begin.load()) { std::this_thread::yield(); }
            for (int i = 0; i < 100; ++i) { racing->post([&] { ++executed; }); }
        });
        begin = true;
        racing->stop();
        int const at_stop = notified.load();
        writer.join();
        check(notified == at_stop, "stop is notifier barrier");
        check(!racing->post([] {}), "post after stop rejected");
        check(racing->drain() == 0 && executed == 0, "stop drops queued tasks");
    }

    wakeup wake;
    auto timer_queue = std::make_shared<chat::tui::inbox>([&] { wake.notify(); });
    int ticks = 0;
    {
        chat::tui::deadline_timer timer(timer_queue, [&] {
            check(std::this_thread::get_id() == ui_thread, "timer callback executes on UI");
            ++ticks;
        });
        timer.schedule(chat::tui::deadline_timer::clock::now());
        wake.wait(1);
        timer.schedule(std::nullopt);
        timer_queue->drain();
        check(ticks == 0, "cancel invalidates queued expiration");

        timer.schedule(chat::tui::deadline_timer::clock::now() + 1h);
        timer.schedule(chat::tui::deadline_timer::clock::now());
        wake.wait(2);
        check(ticks == 0, "timer worker does not mutate UI");
        timer_queue->drain();
        check(ticks == 1, "reschedule wakes deadline wait");

        timer.schedule(chat::tui::deadline_timer::clock::now());
        wake.wait(3);
        timer.schedule(chat::tui::deadline_timer::clock::now() + 1h);
        timer_queue->drain();
        check(ticks == 1, "reschedule invalidates queued old expiration");

        auto const before = std::chrono::steady_clock::now();
        timer.stop();
        check(std::chrono::steady_clock::now() - before < 1s, "stop wakes and joins sleeping worker");
        timer.schedule(chat::tui::deadline_timer::clock::now());
    }
    {
        chat::tui::deadline_timer timer(timer_queue, [&] { ++ticks; });
        timer.schedule(chat::tui::deadline_timer::clock::now());
        wake.wait(4);
    }
    timer_queue->drain();
    check(ticks == 1, "destroyed timer invalidates queued callback");
    timer_queue->stop();
    std::cout << "PASS inbox thread handoff, stop race and deadline lifecycle\n";
}
