#ifndef CHAT_SRC_ONLINE_USERS_HPP
#define CHAT_SRC_ONLINE_USERS_HPP

#include <cstdint>
#include <unordered_map>
#include <span>
#include <vector>

class chat_session;

class online_users
{
   public:
    void add(std::int64_t user_id, chat_session& session);

    void remove(std::int64_t user_id, chat_session& session) noexcept;

    std::span<chat_session* const> sessions(std::int64_t user_id) const noexcept;

   private:
    std::unordered_map<std::int64_t, std::vector<chat_session*>> sessions_;
};

#endif
