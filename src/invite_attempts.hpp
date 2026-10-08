#ifndef CHAT_SRC_INVITE_ATTEMPTS_HPP
#define CHAT_SRC_INVITE_ATTEMPTS_HPP

#include <chrono>
#include <cstdint>
#include <unordered_map>

// Limits guessing of invite codes per account. A failure is a well-formed code that matches no group;
// after max_failures within window every join of that account is refused until the window ends.
// Shared by every session of the server; like online_users it is only touched from the server's executor.
class invite_attempts
{
   public:
    using clock = std::chrono::steady_clock;
    static constexpr int max_failures = 10;
    static constexpr std::chrono::minutes window{10};

    bool blocked(std::int64_t user, clock::time_point now = clock::now());

    void record_failure(std::int64_t user, clock::time_point now = clock::now());

   private:
    struct entry
    {
        clock::time_point started;
        int failures = 0;
    };

    void prune(clock::time_point now);

    std::unordered_map<std::int64_t, entry> users_;
};

#endif
