#include "online_users.hpp"


bool online_users::add(std::int64_t user_id, chat_session& session)
{
    return sessions_.emplace(user_id, &session).second;
}

void online_users::remove(std::int64_t user_id, chat_session& session) noexcept
{
    auto const it = sessions_.find(user_id);
    if (it != sessions_.end() && it->second == &session)
    {
        sessions_.erase(it);
    }
}
