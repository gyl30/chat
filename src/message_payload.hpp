#ifndef CHAT_SRC_MESSAGE_PAYLOAD_HPP
#define CHAT_SRC_MESSAGE_PAYLOAD_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <chat/message.hpp>
#include <simdjson.h>

struct message_payload
{
    std::string nickname{};
    std::int64_t forwarded_user = 0;
    std::string forwarded_from{};
    std::int64_t id = 0;
    std::int64_t conversation = 0;
    std::int64_t from = 0;
    std::string username;
    std::int64_t timestamp = 0;
    std::string text;
    std::optional<chat::quoted_message> reply;
    std::optional<std::int64_t> edited_at;
    bool deleted = false;
    bool markdown = false;
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
    value.forwarded_user = message.forwarded_user;
    value.forwarded_from = message.forwarded_from;
    value.id = message.id;
    value.conversation = message.conversation;
    value.from = message.from;
    value.username = message.username;
    value.nickname = message.nickname;
    value.timestamp = message.timestamp;
    value.text = message.text;
    value.markdown = message.markdown;
    value.reply = message.reply;
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

struct quoted_notification
{
    std::int64_t id{};
    std::string without_quote;
};

struct message_broadcast
{
    std::string json;
    std::int64_t id{};
    quoted_notification quote;

    simdjson::error_code serialize(std::string_view method, message_payload value)
    {
        struct envelope
        {
            std::string_view jsonrpc, method;
            message_payload params;
        };
        id = value.id;
        value.client_id.reset();
        envelope event{"2.0", method, std::move(value)};
        if (auto error = simdjson::builder::to_json_string(event).get(json))
        {
            return error;
        }
        if (event.params.reply)
        {
            quote.id = event.params.reply->id;
            event.params.reply.reset();
            return simdjson::builder::to_json_string(event).get(quote.without_quote);
        }
        return simdjson::SUCCESS;
    }
};

#endif
