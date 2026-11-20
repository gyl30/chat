#include "online_users.hpp"

#include <algorithm>

void online_users::add(std::int64_t user_id, chat_session& session) { sessions_[user_id].push_back(&session); }

void online_users::remove(std::int64_t user_id, chat_session& session) noexcept
{
    auto it = sessions_.find(user_id);
    if (it != sessions_.end())
    {
        std::erase(it->second, &session);
        if (it->second.empty())
        {
            sessions_.erase(it);
        }
    }
}

std::span<chat_session* const> online_users::sessions(std::int64_t user_id) const noexcept
{
    auto it = sessions_.find(user_id);
    if (it == sessions_.end())
    {
        return {};
    }
    return it->second;
}
