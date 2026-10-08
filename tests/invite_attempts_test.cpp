#include <iostream>

#include "invite_attempts.hpp"

namespace
{

bool check(bool value, char const* text)
{
    if (!value) { std::cerr << "FAIL " << text << '\n'; return false; }
    std::cout << "PASS " << text << '\n';
    return true;
}

}    // namespace

int main()
{
    using namespace std::chrono_literals;
    invite_attempts attempts;
    auto const start = invite_attempts::clock::time_point{} + 1h;
    bool ok = check(!attempts.blocked(1, start), "an account without failures may join");
    for (int i = 1; i < invite_attempts::max_failures; ++i) { attempts.record_failure(1, start + i * 1s); }
    ok &= check(!attempts.blocked(1, start + 30s), "failures below the limit still allow joining");
    attempts.record_failure(1, start + 40s);
    ok &= check(attempts.blocked(1, start + 41s), "the limit blocks further joins");
    ok &= check(!attempts.blocked(2, start + 41s), "one account's failures never block another");
    // The window starts at the first failure, one second after start.
    auto const first = start + 1s;
    ok &= check(attempts.blocked(1, first + invite_attempts::window - 1s), "the block lasts for the rest of the window");
    ok &= check(!attempts.blocked(1, first + invite_attempts::window), "the window's end lifts the block");
    attempts.record_failure(1, first + invite_attempts::window + 1s);
    ok &= check(!attempts.blocked(1, first + invite_attempts::window + 2s), "a new window counts from zero");
    for (int user = 100; user < 1200; ++user) { attempts.record_failure(user, start); }
    attempts.record_failure(5000, start + invite_attempts::window + 5s);
    ok &= check(!attempts.blocked(100, start + invite_attempts::window + 6s), "expired accounts are swept as the table grows");
    return ok ? 0 : 1;
}
