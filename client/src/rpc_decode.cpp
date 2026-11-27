#include "rpc_decode.hpp"
#include "rpc_payload.hpp"

#include <unordered_set>
#include <utility>

#include <chat/attachment.hpp>

namespace chat::detail
{

bool parse_avatar_state(dom::object object, avatar_state& value)
{
    auto parsed = integer_field(object, "avatar_revision");
    auto present = find_field(object, "has_avatar");
    if (!parsed || *parsed < 0 || !present || !present->is_bool() || (present->get_bool().value_unsafe() && *parsed == 0))
    {
        return false;
    }
    value = {*parsed, present->get_bool().value_unsafe()};
    return true;
}

std::string string_field(dom::object object, std::string_view key)
{
    auto value = find_field(object, key);
    return value && value->is_string() ? std::string(value->get_string().value_unsafe()) : std::string{};
}

bool parse_user(dom::object object, user& value)
{
    auto id_value = find_field(object, "id");
    auto username_value = find_field(object, "username");
    if (!id_value || !username_value || !username_value->is_string())
    {
        return false;
    }

    auto id = read_payload<std::int64_t>(*id_value);
    if (!id || *id <= 0)
    {
        return false;
    }

    value.id = *id;
    value.username = std::string(username_value->get_string().value_unsafe());
    value.nickname = string_field(object, "nickname");
    return parse_avatar_state(object, value.avatar);
}

std::optional<friendship_state> parse_friendship_state(std::optional<dom::element> value)
{
    if (!value || !value->is_string())
    {
        return {};
    }
    auto state = value->get_string().value_unsafe();
    if (state == "none")
    {
        return friendship_state::none;
    }
    if (state == "outgoing_pending")
    {
        return friendship_state::outgoing_pending;
    }
    if (state == "incoming_pending")
    {
        return friendship_state::incoming_pending;
    }
    if (state == "accepted")
    {
        return friendship_state::accepted;
    }
    return {};
}

static bool parse_deleted(dom::object object, bool& deleted)
{
    auto value = find_field(object, "deleted");
    if (!value)
    {
        return true;
    }
    if (!value->is_bool())
    {
        return false;
    }
    deleted = value->get_bool().value_unsafe();
    return true;
}

static bool parse_edited_at(dom::object object, std::optional<std::int64_t>& edited_at)
{
    auto value = find_field(object, "edited_at");
    if (!value || value->is_null())
    {
        return true;
    }
    edited_at = read_payload<std::int64_t>(*value);
    return edited_at && *edited_at > 0;
}

static bool parse_quote(dom::object object, std::string_view key, std::optional<quoted_message>& reply)
{
    auto value = find_field(object, key);
    if (!value || value->is_null())
    {
        return true;
    }
    if (!value->is_object())
    {
        return false;
    }
    auto fields = value->get_object().value_unsafe();
    auto id = find_field(fields, "id");
    auto from = find_field(fields, "from");
    auto username = find_field(fields, "username");
    auto text = find_field(fields, "text");
    if (!id || !from || !username || !username->is_string() || !text || !text->is_string())
    {
        return false;
    }
    auto parsed_id = read_payload<std::int64_t>(*id);
    auto parsed_from = read_payload<std::int64_t>(*from);
    if (!parsed_id || *parsed_id <= 0 || !parsed_from || *parsed_from <= 0)
    {
        return false;
    }
    reply = quoted_message{
        *parsed_id, *parsed_from, std::string(username->get_string().value_unsafe()), std::string(text->get_string().value_unsafe()), {}};
    reply->nickname = string_field(fields, "nickname");
    return parse_edited_at(fields, reply->edited_at) && parse_deleted(fields, reply->deleted);
}

static bool parse_reactions(dom::object object, std::int64_t& revision, std::vector<reaction>& reactions)
{
    auto parsed_revision = integer_field(object, "reaction_revision");
    auto list = find_field(object, "reactions");
    if (!parsed_revision || *parsed_revision < 0 || !list || !list->is_array() ||
        (*parsed_revision == 0 && list->get_array().value_unsafe().size() != 0))
    {
        return false;
    }
    revision = *parsed_revision;
    std::unordered_set<std::int64_t> seen_users;
    for (auto item : list->get_array().value_unsafe())
    {
        if (!item.is_object())
        {
            return false;
        }
        auto emoji_value = find_field(item, "emoji");
        auto users = find_field(item, "users");
        if (!emoji_value || !emoji_value->is_string() || !users || !users->is_array() || users->get_array().value_unsafe().size() == 0)
        {
            return false;
        }
        auto const emoji = std::string_view(emoji_value->get_string().value_unsafe());
        if (std::ranges::find(reaction_choices, emoji) == reaction_choices.end() ||
            std::ranges::any_of(reactions, [&](reaction const& value) { return value.emoji == emoji; }))
        {
            return false;
        }
        reaction value{std::string(emoji), {}};
        for (auto user_value : users->get_array().value_unsafe())
        {
            auto user = read_payload<std::int64_t>(user_value);
            if (!user || *user <= 0 || !seen_users.insert(*user).second)
            {
                return false;
            }
            value.users.push_back(*user);
        }
        reactions.push_back(std::move(value));
    }
    return true;
}

bool parse_reaction_update(dom::object object, reaction_update& value)
{
    auto conversation = integer_field(object, "conversation");
    auto message = integer_field(object, "message");
    if (!conversation || *conversation <= 0 || !message || *message <= 0 || !parse_reactions(object, value.revision, value.reactions))
    {
        return false;
    }
    value.conversation = *conversation;
    value.message = *message;
    return true;
}

static bool parse_mentions(dom::object object, std::vector<mention>& result)
{
    auto values = find_field(object, "mentions");
    if (!values || !values->is_array())
    {
        return false;
    }
    std::unordered_set<std::int64_t> seen;
    for (auto value : values->get_array().value_unsafe())
    {
        if (!value.is_object())
        {
            return false;
        }
        auto user = integer_field(value, "user");
        auto username = find_field(value, "username");
        if (!user || *user <= 0 || !username || !username->is_string() || username->get_string().value_unsafe().empty() || !seen.insert(*user).second)
        {
            return false;
        }
        result.push_back({*user, std::string(username->get_string().value_unsafe())});
    }
    return true;
}

bool parse_message(dom::object object, message& value)
{
    if (auto poll = find_field(object, "poll"); poll && !poll->is_null())
    {
        value.poll = read_payload<poll_summary>(*poll);
        if (!value.poll)
        {
            return false;
        }
    }
    value.content = string_field(object, "content");
    if (value.content.empty())
    {
        value.content = "message";
    }
    if (auto flag = find_field(object, "markdown"); flag && flag->get(value.markdown))
    {
        return false;
    }
    if (auto flag = find_field(object, "mention_all"); flag && flag->get(value.mention_all))
    {
        return false;
    }
    value.nickname = string_field(object, "nickname");
    if (auto sender = find_field(object, "forwarded_user"); sender && sender->is_int64())
    {
        value.forwarded_user = sender->get_int64().value_unsafe();
    }
    if (auto origin = find_field(object, "forwarded_from"); origin && origin->is_string())
    {
        value.forwarded_from = std::string(origin->get_string().value_unsafe());
    }
    auto id_value = find_field(object, "id");
    auto from_value = find_field(object, "from");
    auto timestamp_value = find_field(object, "timestamp");
    auto text_value = find_field(object, "text");
    auto conversation_value = find_field(object, "conversation");
    auto username_value = find_field(object, "username");
    if (!conversation_value || !username_value || !username_value->is_string() || !id_value || !from_value || !timestamp_value || !text_value ||
        !text_value->is_string())
    {
        return false;
    }

    auto id = read_payload<std::int64_t>(*id_value);
    auto from = read_payload<std::int64_t>(*from_value);
    auto timestamp = read_payload<std::int64_t>(*timestamp_value);
    auto conversation = read_payload<std::int64_t>(*conversation_value);
    if (!conversation || *conversation <= 0 || !id || *id <= 0 || !from || *from <= 0 || !timestamp || *timestamp <= 0)
    {
        return false;
    }

    value.id = *id;
    value.conversation = *conversation;
    value.username = std::string(username_value->get_string().value_unsafe());
    value.from = *from;
    value.timestamp = *timestamp;
    value.text = std::string(text_value->get_string().value_unsafe());
    auto attachment = find_field(object, "attachment");
    if (attachment && !attachment->is_null())
    {
        if (!attachment->is_object())
        {
            return false;
        }
        auto fields = attachment->get_object().value_unsafe();
        auto filename = find_field(fields, "filename");
        auto media_type = find_field(fields, "media_type");
        auto size = integer_field(fields, "size");
        if (!filename || !filename->is_string() || filename->get_string().value_unsafe().empty() ||
            filename->get_string().value_unsafe().size() > 255 || !media_type || !media_type->is_string() || !size || *size < 0 ||
            *size > static_cast<std::int64_t>(max_attachment_size))
        {
            return false;
        }
        auto const type = std::string_view(media_type->get_string().value_unsafe());
        if (type != "application/octet-stream" && type != "image/png" && type != "image/jpeg")
        {
            return false;
        }
        auto key = find_field(fields, "object_key");
        if (!key || !key->is_string())
        {
            return false;
        }
        value.attachment = attachment_info{
            std::string(filename->get_string().value_unsafe()), std::string(type), *size, std::string(key->get_string().value_unsafe())};
    }
    auto client_id = find_field(object, "client_id");
    if (client_id && !client_id->is_null())
    {
        value.client_id = read_payload<std::int64_t>(*client_id);
        if (!value.client_id || *value.client_id <= 0)
        {
            return false;
        }
    }
    return parse_avatar_state(object, value.avatar) && parse_quote(object, "reply", value.reply) && parse_edited_at(object, value.edited_at) &&
           parse_deleted(object, value.deleted) && parse_reactions(object, value.reaction_revision, value.reactions) &&
           parse_mentions(object, value.mentions) && (!value.deleted || (value.reactions.empty() && value.mentions.empty()));
}

std::expected<messages_result, error> parse_message_page(dom::element response, std::int64_t conversation)
{
    if (!response.is_object())
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid get_messages result"));
    }

    auto messages_value = find_field(response, "messages");
    auto read_value = find_field(response, "read_positions");
    auto more_value = find_field(response, "has_more");
    if (!messages_value || !messages_value->is_array() || !read_value || !read_value->is_array() || !more_value || !more_value->is_bool())
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid get_messages result"));
    }

    messages_result result;
    result.has_more = more_value->get_bool().value_unsafe();
    auto newer = find_field(response, "has_newer");
    if (!newer || !newer->is_bool())
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid history boundary"));
    }
    result.has_newer = newer->get_bool().value_unsafe();
    auto floor = integer_field(response, "visible_after");
    auto hidden = find_field(response, "hidden");
    if (!floor || !hidden || !hidden->is_bool())
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid history visibility"));
    }
    result.visible_after = *floor;
    result.hidden = hidden->get_bool().value_unsafe();
    if (auto first = find_field(response, "first_unread"))
    {
        result.first_unread = read_payload<std::int64_t>(*first);
    }
    for (auto position : read_value->get_array().value_unsafe())
    {
        auto user = integer_field(position, "user");
        auto message = integer_field(position, "message");
        if (!user || *user <= 0 || !message || *message < 0)
        {
            return std::unexpected(make_error(error_kind::protocol, "Invalid read position"));
        }
        result.read_positions.push_back(read_position{*user, *message});
    }
    result.messages.reserve(messages_value->get_array().value_unsafe().size());
    for (auto value : messages_value->get_array().value_unsafe())
    {
        if (!value.is_object())
        {
            return std::unexpected(make_error(error_kind::protocol, "Invalid message"));
        }

        message item;
        if (!parse_message(value.get_object().value_unsafe(), item) || item.conversation != conversation)
        {
            return std::unexpected(make_error(error_kind::protocol, "Invalid message"));
        }
        result.messages.push_back(std::move(item));
    }
    return result;
}

bool parse_receipt(dom::object fields, message& value)
{
    auto hidden = find_field(fields, "hidden");
    if (!hidden || !hidden->is_bool() || !hidden->get_bool().value_unsafe())
    {
        return parse_message(fields, value);
    }
    auto number = [&](char const* key) { return integer_field(fields, key).value_or(0); };
    value.id = number("id");
    value.conversation = number("conversation");
    value.from = number("from");
    value.client_id = number("client_id");
    value.acknowledgement_only = true;
    return value.id > 0 && value.from > 0 && value.conversation > 0 && *value.client_id > 0;
}

bool parse_presence(dom::object object, presence& value)
{
    auto user_value = find_field(object, "user");
    auto online_value = find_field(object, "online");
    auto last_seen_value = find_field(object, "last_seen");
    if (!user_value || !online_value || !online_value->is_bool() || !last_seen_value)
    {
        return false;
    }

    auto user = read_payload<std::int64_t>(*user_value);
    auto last_seen = read_payload<std::int64_t>(*last_seen_value);
    if (!user || *user <= 0 || !last_seen || *last_seen < 0)
    {
        return false;
    }

    value.user = *user;
    value.online = online_value->get_bool().value_unsafe();
    value.last_seen = *last_seen;
    return true;
}

std::expected<conversation, error> parse_conversation(dom::element const& value)
{
    if (!value.is_object())
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid conversation"));
    }
    auto object = value.get_object().value_unsafe();
    auto id = integer_field(object, "id");
    auto kind = find_field(object, "kind");
    auto user_value = find_field(object, "user");
    auto name = find_field(object, "username");
    auto last = find_field(object, "last");
    auto unread_value = find_field(object, "unread");
    auto count_value = find_field(object, "member_count");
    auto level = find_field(object, "notification_level");
    auto until = find_field(object, "mute_until");
    auto marked = find_field(object, "marked_unread");
    auto pinned = find_field(object, "pinned");
    auto pinned_message = find_field(object, "pinned_message");
    auto announcement = find_field(object, "announcement");
    auto approval = find_field(object, "join_approval");
    auto unread = unread_value ? read_payload<std::uint64_t>(*unread_value) : std::nullopt;
    auto count = count_value ? read_payload<std::uint64_t>(*count_value) : std::nullopt;
    if (!id || *id <= 0 || !kind || !kind->is_string() || !user_value || !name || !name->is_string() || !last || !unread || !count || *count == 0 ||
        !level || !level->is_int64() || level->get_int64().value_unsafe() < 0 || level->get_int64().value_unsafe() > 2 || !until ||
        !until->is_int64() || !marked || !marked->is_bool() || !pinned || !pinned->is_bool() || !pinned_message || !announcement ||
        !announcement->is_string() || !approval || !approval->is_bool() ||
        (kind->get_string().value_unsafe() == "direct" && approval->get_bool().value_unsafe()))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid conversation"));
    }
    conversation item;
    item.id = *id;
    if (auto folder = find_field(object, "folder"); folder && folder->is_int64())
    {
        item.folder = folder->get_int64().value_unsafe();
    }
    item.username = std::string(name->get_string().value_unsafe());
    item.nickname = string_field(object, "nickname");
    if (auto remark = find_field(object, "remark"); remark && remark->is_string())
    {
        item.remark = std::string(remark->get_string().value_unsafe());
    }
    if (auto hidden = find_field(object, "hidden"); hidden && hidden->get(item.hidden))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid conversation"));
    }
    item.join_requests = integer_field(object, "join_requests").value_or(0);
    item.unread = *unread;
    if (auto flag = find_field(object, "unread_mention"); flag && flag->get(item.unread_mention))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid conversation"));
    }
    item.member_count = *count;
    item.notifications = static_cast<notification_level>(level->get_int64().value_unsafe());
    item.mute_until = until->get_int64().value_unsafe();
    item.marked_unread = marked->get_bool().value_unsafe();
    item.pinned = pinned->get_bool().value_unsafe();
    item.announcement = std::string(announcement->get_string().value_unsafe());
    item.description = string_field(object, "description");
    item.join_approval = approval->get_bool().value_unsafe();
    auto speech_role = find_field(object, "speech_role");
    auto group_muted = find_field(object, "group_muted");
    auto speech_until = find_field(object, "speech_until");
    auto server_time = find_field(object, "server_time");
    if (!speech_role || !speech_role->is_int64() || speech_role->get_int64().value_unsafe() < 0 || speech_role->get_int64().value_unsafe() > 2 ||
        !group_muted || !group_muted->is_bool() || !speech_until || !speech_until->is_int64() || !server_time || !server_time->is_int64())
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid group speech state"));
    }
    item.speech_role = static_cast<member_role>(speech_role->get_int64().value_unsafe());
    item.group_muted = group_muted->get_bool().value_unsafe();
    item.speech_until = speech_until->get_int64().value_unsafe();
    item.server_time = server_time->get_int64().value_unsafe();
    if (item.announcement.size() > 4096 || item.announcement.find('\0') != std::string::npos ||
        (kind->get_string().value_unsafe() == "direct" && !item.announcement.empty()))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid group announcement"));
    }
    if (kind->get_string().value_unsafe() == "direct")
    {
        auto peer = read_payload<std::int64_t>(*user_value);
        if (!peer || *peer <= 0)
        {
            return std::unexpected(make_error(error_kind::protocol, "Invalid direct conversation"));
        }
        item.user = *peer;
    }
    else if (kind->get_string().value_unsafe() == "group" && user_value->is_null())
    {
        item.kind = conversation_kind::group;
    }
    else
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid conversation kind"));
    }
    if (!parse_avatar_state(object, item.avatar))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid conversation avatar"));
    }
    if (!parse_quote(object, "pinned_message", item.pinned_message) ||
        (item.pinned_message && (item.kind != conversation_kind::group || item.pinned_message->deleted)))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid pinned message"));
    }
    if (!last->is_null() && (!last->is_object() || !parse_message(last->get_object().value_unsafe(), item.last) || item.last.conversation != item.id))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid last message"));
    }
    return item;
}

}    // namespace chat::detail
