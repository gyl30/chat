#ifndef CHAT_CLIENT_INCLUDE_CHAT_MESSAGE_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_MESSAGE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "attachment.hpp"
#include "avatar.hpp"
#include "reaction.hpp"

namespace chat
{

struct quoted_message
{
    std::int64_t id = 0;
    std::int64_t from = 0;
    std::string username;
    std::string text;
    std::optional<std::int64_t> edited_at;
    bool deleted = false;
};

struct mention
{
    std::int64_t user = 0;
    std::string username;
};

struct message
{
    std::int64_t id = 0;
    std::int64_t conversation = 0;
    std::int64_t from = 0;
    std::string username;
    std::int64_t timestamp = 0;
    std::string text;
    std::optional<std::int64_t> edited_at;
    bool deleted = false;
    std::optional<quoted_message> reply;
    std::optional<attachment_info> attachment;
    avatar_state avatar;
    std::int64_t reaction_revision = 0;
    std::vector<reaction> reactions;
    std::vector<mention> mentions;
    // Present only on the caller's own messages: the ID it sent them with.
    std::optional<std::int64_t> client_id;
};

struct read_position
{
    std::int64_t user = 0;
    std::int64_t message = 0;
};

struct messages_result
{
    std::vector<message> messages;
    std::vector<read_position> read_positions;
    bool has_more = false;
};

}    // namespace chat

#endif
