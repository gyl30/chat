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
    std::string nickname{};
};

struct mention
{
    std::int64_t user = 0;
    std::string username;
};

struct poll_option
{
    std::int64_t position, votes;
    std::string label;
};
struct poll_summary
{
    std::int64_t revision = 0;
    bool closed = false;
    std::vector<poll_option> options;
};
struct poll_state
{
    poll_summary summary;
    std::int64_t selection = 0;
    bool can_close = false;
    std::string question;
};
struct poll_voter
{
    std::int64_t id = 0;
    std::string username, nickname;
};
struct poll_voters
{
    std::vector<poll_voter> members;
    std::int64_t next = 0;
};
struct poll_update
{
    std::int64_t conversation = 0, message = 0;
    poll_summary summary;
};
struct forwarded_item
{
    std::int64_t position, created;
    std::string author, body;
    bool markdown;
    std::string filename, media_type;
};
struct forwarded_page
{
    std::vector<forwarded_item> items;
    std::int64_t next = 0;
};

struct text_options
{
    bool markdown = false;
    bool mention_all = false;
};

struct message
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
    std::optional<std::int64_t> edited_at;
    bool deleted = false;
    std::optional<quoted_message> reply;
    std::optional<attachment_info> attachment;
    avatar_state avatar;
    std::int64_t reaction_revision = 0;
    std::vector<reaction> reactions;
    std::vector<mention> mentions;
    bool markdown = false;
    bool mention_all = false;
    std::string content = "message";
    std::optional<poll_summary> poll;
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
    bool mentions_me = false;
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

struct bookmark_state
{
    std::int64_t message = 0;
    bool bookmarked = false;
};

struct bookmarked_message
{
    std::int64_t id = 0;
    std::int64_t message = 0;
    std::int64_t created_at = 0;
    std::optional<message_search_hit> source;
    std::string attachment_type;
};

struct bookmark_page
{
    std::vector<bookmarked_message> bookmarks;
    std::optional<std::int64_t> next_before;
};

struct messages_result
{
    std::vector<message> messages;
    std::vector<read_position> read_positions;
    std::int64_t visible_after = 0;
    bool hidden = false;
    bool has_more = false;
    std::optional<std::int64_t> first_unread;
};

}    // namespace chat

#endif
