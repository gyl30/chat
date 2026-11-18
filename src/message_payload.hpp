#ifndef CHAT_SRC_MESSAGE_PAYLOAD_HPP
#define CHAT_SRC_MESSAGE_PAYLOAD_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <chat/message.hpp>

struct quoted_message_payload
{
    std::int64_t id = 0;
    std::int64_t from = 0;
    std::string username;
    std::string text;
    std::optional<std::int64_t> edited_at;
    bool deleted = false;
};

struct message_payload
{
    std::int64_t id = 0;
    std::int64_t conversation = 0;
    std::int64_t from = 0;
    std::string username;
    std::int64_t timestamp = 0;
    std::string text;
    std::optional<quoted_message_payload> reply;
    std::optional<std::int64_t> edited_at;
    bool deleted = false;
    std::optional<chat::attachment_info> attachment;
    std::int64_t avatar_revision = 0;
    bool has_avatar = false;
    std::int64_t reaction_revision = 0;
    std::vector<chat::reaction> reactions;
    std::vector<chat::mention> mentions;
    // Only in the sender's own acknowledgement, never in the shared notification.
    std::optional<std::int64_t> client_id;
};

inline message_payload make_message_payload(chat::message const& message, bool include_client_id = false)
{
    message_payload value;
    value.id = message.id;
    value.conversation = message.conversation;
    value.from = message.from;
    value.username = message.username;
    value.timestamp = message.timestamp;
    value.text = message.text;
    if (message.reply)
    {
        auto const& reply = *message.reply;
        value.reply = quoted_message_payload{reply.id, reply.from, reply.username, reply.text, reply.edited_at, reply.deleted};
    }
    value.edited_at = message.edited_at;
    value.deleted = message.deleted;
    value.attachment = message.attachment;
    value.avatar_revision = message.avatar.revision;
    value.has_avatar = message.avatar.present;
    value.reaction_revision = message.reaction_revision;
    value.reactions = message.reactions;
    value.mentions = message.mentions;
    if (include_client_id)
    {
        value.client_id = message.client_id;
    }
    return value;
}

#endif
