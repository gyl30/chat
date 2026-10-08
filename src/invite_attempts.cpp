#include "invite_attempts.hpp"

#include <cstddef>
#include <iterator>

namespace
{

// Expired entries are only swept once the table grows, so the common case stays a single lookup.
constexpr std::size_t prune_threshold = 1024;

}    // namespace

bool invite_attempts::blocked(std::int64_t user, clock::time_point now)
{
    auto const found = users_.find(user);
    if (found == users_.end()) { return false; }
    if (now - found->second.started >= window)
    {
        users_.erase(found);
        return false;
    }
    return found->second.failures >= max_failures;
}

void invite_attempts::record_failure(std::int64_t user, clock::time_point now)
{
    if (users_.size() >= prune_threshold) { prune(now); }
    auto& value = users_[user];
    if (value.failures == 0 || now - value.started >= window)
    {
        value = {now, 0};
    }
    ++value.failures;
}

void invite_attempts::prune(clock::time_point now)
{
    for (auto it = users_.begin(); it != users_.end();)
    {
        it = now - it->second.started >= window ? users_.erase(it) : std::next(it);
    }
}
