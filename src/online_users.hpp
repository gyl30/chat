#ifndef CHAT_SRC_ONLINE_USERS_HPP
#define CHAT_SRC_ONLINE_USERS_HPP

#include <cstdint>
#include <unordered_map>

class chat_session;

class online_users
{
   public:
    bool add(std::int64_t user_id, chat_session& session);

    void remove(std::int64_t user_id, chat_session& session) noexcept;

    chat_session* find(std::int64_t user_id) const noexcept;

   private:
    std::unordered_map<std::int64_t, chat_session*> sessions_;
};

#endif
