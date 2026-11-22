#ifndef CHAT_CLIENT_INCLUDE_CHAT_MESSAGE_HPP
#define CHAT_CLIENT_INCLUDE_CHAT_MESSAGE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "attachment.hpp"
#include "avatar.hpp"
#include "reaction.hpp"

namespace chat
{

enum class message_category
{
    all,
    images,
    files,
    links,
};

inline constexpr std::string_view category_name(message_category kind)
{
    switch (kind)
    {
        case message_category::images:
            return "images";
        case message_category::files:
            return "files";
        case message_category::links:
            return "links";
        case message_category::all:
            return "all";
    }
    return "all";
}

inline constexpr std::optional<message_category> parse_message_category(std::string_view name)
{
    for (auto kind : {message_category::all, message_category::images, message_category::files, message_category::links})
    {
        if (category_name(kind) == name)
        {
            return kind;
        }
    }
    return std::nullopt;
}

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
    std::string forwarded_from{};
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
    bool acknowledgement_only = false;
    // Present only on the caller's own messages: the ID it sent them with.
    std::optional<std::int64_t> client_id;
};

struct read_position
{
    std::int64_t user = 0;
    std::int64_t message = 0;
};

struct message_search_filter
{
    std::string query;
    std::optional<std::int64_t> conversation;
    message_category category = message_category::all;
    std::optional<std::int64_t> sender;
    std::optional<std::int64_t> start;
    std::optional<std::int64_t> end;
};

struct message_search_scope
{
    std::int64_t id;
    std::string name;
};

struct message_search_hit
{
    std::int64_t conversation;
    std::int64_t id;
    std::int64_t from;
    std::int64_t timestamp;
    std::string title;
    std::string username;
    std::string snippet;
};

struct message_search_page
{
    std::vector<message_search_hit> messages;
    std::optional<std::int64_t> next_before;
};

struct messages_result
{
    std::vector<message> messages;
    std::vector<read_position> read_positions;
    std::int64_t visible_after = 0;
    bool hidden = false;
    bool has_more = false;
};

}    // namespace chat

#endif
