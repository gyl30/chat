#ifndef CHAT_SRC_MESSAGE_PAYLOAD_HPP
#define CHAT_SRC_MESSAGE_PAYLOAD_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <chat/attachment.hpp>

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
};

#endif
