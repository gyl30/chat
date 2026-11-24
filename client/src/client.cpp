#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <stop_token>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/work_guard.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/json.hpp>
#include <boost/describe/class.hpp>

#include <chat/client.hpp>
#include <chat/invite.hpp>
#include <chat/detail/base64.hpp>
#include <chat/detail/websocket_limits.hpp>

#include "websocket.hpp"
#include "attachment_files.hpp"
#include "blocking_worker.hpp"
#include <filesystem>

namespace chat
{
struct login_session_list
{
    std::vector<login_session> sessions;
};
BOOST_DESCRIBE_STRUCT(login_session, (), (id, device_name, client_type, created_at, last_seen, current))
BOOST_DESCRIBE_STRUCT(login_session_list, (), (sessions))
BOOST_DESCRIBE_STRUCT(conversation_folder, (), (id, name))
BOOST_DESCRIBE_STRUCT(conversation_view_event, (), (conversation, action, through, level, until))
BOOST_DESCRIBE_STRUCT(user_profile, (), (id, username, nickname, signature))
BOOST_DESCRIBE_STRUCT(contact_entry, (), (id, username, avatar_revision, has_avatar, remark, group, nickname))
BOOST_DESCRIBE_STRUCT(contact_book, (), (users, groups))
BOOST_DESCRIBE_STRUCT(message_search_scope, (), (id, name))
BOOST_DESCRIBE_STRUCT(message_search_hit, (), (conversation, id, from, timestamp, title, username, snippet))
BOOST_DESCRIBE_STRUCT(message_search_page, (), (messages, next_before))
BOOST_DESCRIBE_STRUCT(bookmark_state, (), (message, bookmarked))
BOOST_DESCRIBE_STRUCT(bookmarked_message, (), (id, message, created_at, source, attachment_type))
BOOST_DESCRIBE_STRUCT(bookmark_page, (), (bookmarks, next_before))
BOOST_DESCRIBE_STRUCT(conversation_file, (), (object_key, filename, media_type, size, uploader_id, username, created_at, category))
BOOST_DESCRIBE_STRUCT(conversation_file_page, (), (files, has_more))

namespace
{

using response_handler = std::function<void(std::expected<boost::json::value, error>)>;

error make_error(error_kind kind, std::string message, int code = 0)
{
    error value;
    value.kind = kind;
    value.code = code;
    value.message = std::move(message);
    return value;
}

std::optional<std::int64_t> parse_response_id(boost::json::value const& value)
{
    if (value.is_int64())
    {
        return value.as_int64();
    }
    if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return static_cast<std::int64_t>(value.as_uint64());
    }
    return std::nullopt;
}

std::optional<std::int64_t> parse_int64(boost::json::value const& value)
{
    if (value.is_int64())
    {
        return value.as_int64();
    }
    if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return static_cast<std::int64_t>(value.as_uint64());
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parse_uint64(boost::json::value const& value)
{
    if (value.is_uint64())
    {
        return value.as_uint64();
    }
    if (value.is_int64() && value.as_int64() >= 0)
    {
        return static_cast<std::uint64_t>(value.as_int64());
    }
    return std::nullopt;
}

bool parse_avatar_state(boost::json::object const& object, avatar_state& value)
{
    auto const* revision = object.if_contains("avatar_revision");
    auto const* present = object.if_contains("has_avatar");
    auto parsed = revision ? parse_int64(*revision) : std::nullopt;
    if (!parsed || *parsed < 0 || !present || !present->is_bool() || (present->as_bool() && *parsed == 0))
    {
        return false;
    }
    value = {*parsed, present->as_bool()};
    return true;
}

std::string string_field(boost::json::object const& object, std::string_view key)
{
    auto* value = object.if_contains(key);
    return value && value->is_string() ? std::string(value->as_string()) : std::string{};
}

bool parse_user(boost::json::object const& object, user& value)
{
    auto const* id_value = object.if_contains("id");
    auto const* username_value = object.if_contains("username");
    if (!id_value || !username_value || !username_value->is_string())
    {
        return false;
    }

    auto id = parse_int64(*id_value);
    if (!id || *id <= 0)
    {
        return false;
    }

    value.id = *id;
    value.username = std::string(username_value->as_string());
    value.nickname = string_field(object, "nickname");
    return parse_avatar_state(object, value.avatar);
}

std::optional<friendship_state> parse_friendship_state(boost::json::value const* value)
{
    if (!value || !value->is_string())
    {
        return {};
    }
    auto const& state = value->as_string();
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

bool parse_deleted(boost::json::object const& object, bool& deleted)
{
    auto const* value = object.if_contains("deleted");
    if (!value)
    {
        return true;
    }
    if (!value->is_bool())
    {
        return false;
    }
    deleted = value->as_bool();
    return true;
}

bool parse_edited_at(boost::json::object const& object, std::optional<std::int64_t>& edited_at)
{
    auto const* value = object.if_contains("edited_at");
    if (!value || value->is_null())
    {
        return true;
    }
    edited_at = parse_int64(*value);
    return edited_at && *edited_at > 0;
}

bool parse_quote(boost::json::object const& object, std::string_view key, std::optional<quoted_message>& reply)
{
    auto const* value = object.if_contains(key);
    if (!value || value->is_null())
    {
        return true;
    }
    if (!value->is_object())
    {
        return false;
    }
    auto const& fields = value->as_object();
    auto const* id = fields.if_contains("id");
    auto const* from = fields.if_contains("from");
    auto const* username = fields.if_contains("username");
    auto const* text = fields.if_contains("text");
    if (!id || !from || !username || !username->is_string() || !text || !text->is_string())
    {
        return false;
    }
    auto parsed_id = parse_int64(*id);
    auto parsed_from = parse_int64(*from);
    if (!parsed_id || *parsed_id <= 0 || !parsed_from || *parsed_from <= 0)
    {
        return false;
    }
    reply = quoted_message{*parsed_id, *parsed_from, std::string(username->as_string()), std::string(text->as_string()), {}};
    reply->nickname = string_field(fields, "nickname");
    return parse_edited_at(fields, reply->edited_at) && parse_deleted(fields, reply->deleted);
}

bool parse_reactions(boost::json::object const& object, std::int64_t& revision, std::vector<reaction>& reactions)
{
    auto const* revision_value = object.if_contains("reaction_revision");
    auto const* list = object.if_contains("reactions");
    auto parsed_revision = revision_value ? parse_int64(*revision_value) : std::nullopt;
    if (!parsed_revision || *parsed_revision < 0 || !list || !list->is_array() || (*parsed_revision == 0 && !list->as_array().empty()))
    {
        return false;
    }
    revision = *parsed_revision;
    std::unordered_set<std::int64_t> seen_users;
    for (auto const& item : list->as_array())
    {
        if (!item.is_object())
        {
            return false;
        }
        auto const* emoji_value = item.as_object().if_contains("emoji");
        auto const* users = item.as_object().if_contains("users");
        if (!emoji_value || !emoji_value->is_string() || !users || !users->is_array() || users->as_array().empty())
        {
            return false;
        }
        auto const emoji = std::string_view(emoji_value->as_string());
        if (std::ranges::find(reaction_choices, emoji) == reaction_choices.end() ||
            std::ranges::any_of(reactions, [&](reaction const& value) { return value.emoji == emoji; }))
        {
            return false;
        }
        reaction value{std::string(emoji), {}};
        for (auto const& user_value : users->as_array())
        {
            auto user = parse_int64(user_value);
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

bool parse_reaction_update(boost::json::object const& object, reaction_update& value)
{
    auto const* conversation_value = object.if_contains("conversation");
    auto const* message_value = object.if_contains("message");
    auto conversation = conversation_value ? parse_int64(*conversation_value) : std::nullopt;
    auto message = message_value ? parse_int64(*message_value) : std::nullopt;
    if (!conversation || *conversation <= 0 || !message || *message <= 0 || !parse_reactions(object, value.revision, value.reactions))
    {
        return false;
    }
    value.conversation = *conversation;
    value.message = *message;
    return true;
}

bool parse_mentions(boost::json::object const& object, std::vector<mention>& result)
{
    auto const* values = object.if_contains("mentions");
    if (!values || !values->is_array())
    {
        return false;
    }
    std::unordered_set<std::int64_t> seen;
    for (auto const& value : values->as_array())
    {
        if (!value.is_object())
        {
            return false;
        }
        auto const* user_value = value.as_object().if_contains("user");
        auto const* username = value.as_object().if_contains("username");
        auto user = user_value ? parse_int64(*user_value) : std::nullopt;
        if (!user || *user <= 0 || !username || !username->is_string() || username->as_string().empty() || !seen.insert(*user).second)
        {
            return false;
        }
        result.push_back({*user, std::string(username->as_string())});
    }
    return true;
}

bool parse_message(boost::json::object const& object, message& value)
{
    value.nickname = string_field(object, "nickname");
    if (auto* origin = object.if_contains("forwarded_from"); origin && origin->is_string())
    {
        value.forwarded_from = std::string(origin->as_string());
    }
    auto const* id_value = object.if_contains("id");
    auto const* from_value = object.if_contains("from");
    auto const* timestamp_value = object.if_contains("timestamp");
    auto const* text_value = object.if_contains("text");
    auto const* conversation_value = object.if_contains("conversation");
    auto const* username_value = object.if_contains("username");
    if (!conversation_value || !username_value || !username_value->is_string() || !id_value || !from_value || !timestamp_value || !text_value ||
        !text_value->is_string())
    {
        return false;
    }

    auto id = parse_int64(*id_value);
    auto from = parse_int64(*from_value);
    auto timestamp = parse_int64(*timestamp_value);
    auto conversation = parse_int64(*conversation_value);
    if (!conversation || *conversation <= 0 || !id || *id <= 0 || !from || *from <= 0 || !timestamp || *timestamp <= 0)
    {
        return false;
    }

    value.id = *id;
    value.conversation = *conversation;
    value.username = std::string(username_value->as_string());
    value.from = *from;
    value.timestamp = *timestamp;
    value.text = std::string(text_value->as_string());
    auto const* attachment = object.if_contains("attachment");
    if (attachment && !attachment->is_null())
    {
        if (!attachment->is_object())
        {
            return false;
        }
        auto const& fields = attachment->as_object();
        auto const* filename = fields.if_contains("filename");
        auto const* media_type = fields.if_contains("media_type");
        auto const* size_value = fields.if_contains("size");
        auto size = size_value ? parse_int64(*size_value) : std::nullopt;
        if (!filename || !filename->is_string() || filename->as_string().empty() || filename->as_string().size() > 255 || !media_type ||
            !media_type->is_string() || !size || *size < 0 || *size > static_cast<std::int64_t>(max_attachment_size))
        {
            return false;
        }
        auto const type = std::string_view(media_type->as_string());
        if (type != "application/octet-stream" && type != "image/png" && type != "image/jpeg")
        {
            return false;
        }
        auto* key = fields.if_contains("object_key");
        if (!key || !key->is_string())
        {
            return false;
        }
        value.attachment = attachment_info{std::string(filename->as_string()), std::string(type), *size, std::string(key->as_string())};
    }
    auto const* client_id = object.if_contains("client_id");
    if (client_id && !client_id->is_null())
    {
        value.client_id = parse_int64(*client_id);
        if (!value.client_id || *value.client_id <= 0)
        {
            return false;
        }
    }
    return parse_avatar_state(object, value.avatar) && parse_quote(object, "reply", value.reply) && parse_edited_at(object, value.edited_at) &&
           parse_deleted(object, value.deleted) && parse_reactions(object, value.reaction_revision, value.reactions) &&
           parse_mentions(object, value.mentions) && (!value.deleted || (value.reactions.empty() && value.mentions.empty()));
}

bool parse_receipt(boost::json::object const& fields, message& value)
{
    auto hidden = fields.if_contains("hidden");
    if (!hidden || !hidden->is_bool() || !hidden->as_bool())
    {
        return parse_message(fields, value);
    }
    auto number = [&](char const* key)
    {
        auto v = fields.if_contains(key);
        return v ? parse_int64(*v).value_or(0) : 0;
    };
    value.id = number("id");
    value.conversation = number("conversation");
    value.from = number("from");
    value.client_id = number("client_id");
    value.acknowledgement_only = true;
    return value.id > 0 && value.from > 0 && value.conversation > 0 && *value.client_id > 0;
}

bool parse_presence(boost::json::object const& object, presence& value)
{
    auto const* user_value = object.if_contains("user");
    auto const* online_value = object.if_contains("online");
    auto const* last_seen_value = object.if_contains("last_seen");
    if (!user_value || !online_value || !online_value->is_bool() || !last_seen_value)
    {
        return false;
    }

    auto user = parse_int64(*user_value);
    auto last_seen = parse_int64(*last_seen_value);
    if (!user || *user <= 0 || !last_seen || *last_seen < 0)
    {
        return false;
    }

    value.user = *user;
    value.online = online_value->as_bool();
    value.last_seen = *last_seen;
    return true;
}

std::expected<conversation, error> parse_conversation(boost::json::value const& value)
{
    if (!value.is_object())
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid conversation"));
    }
    auto const& object = value.as_object();
    auto const* id_value = object.if_contains("id");
    auto const* kind = object.if_contains("kind");
    auto const* user_value = object.if_contains("user");
    auto const* name = object.if_contains("username");
    auto const* last = object.if_contains("last");
    auto const* unread_value = object.if_contains("unread");
    auto const* count_value = object.if_contains("member_count");
    auto const* level = object.if_contains("notification_level");
    auto const* until = object.if_contains("mute_until");
    auto const* marked = object.if_contains("marked_unread");
    auto const* pinned = object.if_contains("pinned");
    auto const* pinned_message = object.if_contains("pinned_message");
    auto const* announcement = object.if_contains("announcement");
    auto const* approval = object.if_contains("join_approval");
    auto id = id_value ? parse_int64(*id_value) : std::nullopt;
    auto unread = unread_value ? parse_uint64(*unread_value) : std::nullopt;
    auto count = count_value ? parse_uint64(*count_value) : std::nullopt;
    if (!id || *id <= 0 || !kind || !kind->is_string() || !user_value || !name || !name->is_string() || !last || !unread || !count || *count == 0 ||
        !level || !level->is_int64() || level->as_int64() < 0 || level->as_int64() > 2 || !until || !until->is_int64() || !marked ||
        !marked->is_bool() || !pinned || !pinned->is_bool() || !pinned_message || !announcement || !announcement->is_string() || !approval ||
        !approval->is_bool() || (kind->as_string() == "direct" && approval->as_bool()))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid conversation"));
    }
    conversation item;
    item.id = *id;
    if (auto* folder = object.if_contains("folder"); folder && folder->is_int64())
    {
        item.folder = folder->as_int64();
    }
    item.username = std::string(name->as_string());
    item.nickname = string_field(object, "nickname");
    if (auto* remark = object.if_contains("remark"); remark && remark->is_string())
    {
        item.remark = std::string(remark->as_string());
    }
    item.unread = *unread;
    item.member_count = *count;
    item.notifications = static_cast<notification_level>(level->as_int64());
    item.mute_until = until->as_int64();
    item.marked_unread = marked->as_bool();
    item.pinned = pinned->as_bool();
    item.announcement = std::string(announcement->as_string());
    item.description = string_field(object, "description");
    item.join_approval = approval->as_bool();
    auto const* speech_role = object.if_contains("speech_role");
    auto const* group_muted = object.if_contains("group_muted");
    auto const* speech_until = object.if_contains("speech_until");
    auto const* server_time = object.if_contains("server_time");
    if (!speech_role || !speech_role->is_int64() || speech_role->as_int64() < 0 || speech_role->as_int64() > 2 || !group_muted ||
        !group_muted->is_bool() || !speech_until || !speech_until->is_int64() || !server_time || !server_time->is_int64())
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid group speech state"));
    }
    item.speech_role = static_cast<member_role>(speech_role->as_int64());
    item.group_muted = group_muted->as_bool();
    item.speech_until = speech_until->as_int64();
    item.server_time = server_time->as_int64();
    if (item.announcement.size() > 4096 || item.announcement.find('\0') != std::string::npos ||
        (kind->as_string() == "direct" && !item.announcement.empty()))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid group announcement"));
    }
    if (kind->as_string() == "direct")
    {
        auto peer = parse_int64(*user_value);
        if (!peer || *peer <= 0)
        {
            return std::unexpected(make_error(error_kind::protocol, "Invalid direct conversation"));
        }
        item.user = *peer;
    }
    else if (kind->as_string() == "group" && user_value->is_null())
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
    if (!last->is_null() && (!last->is_object() || !parse_message(last->as_object(), item.last) || item.last.conversation != item.id))
    {
        return std::unexpected(make_error(error_kind::protocol, "Invalid last message"));
    }
    return item;
}

}    // namespace

struct client::impl
{
    enum class connection_state
    {
        disconnected,
        connecting,
        connected,
    };

    explicit impl(std::string directory)
        : transfer_directory_(std::move(directory)),
          websocket_(io_context_),
          work_(boost::capy::make_work_guard(io_context_.get_executor())),
          network_thread_([this] { io_context_.run(); })
    {
    }

    ~impl()
    {
        suppress_callbacks_.store(true);

        std::promise<void> stopped;
        auto stopped_future = stopped.get_future();
        boost::capy::run_async(io_context_.get_executor())(shutdown(&stopped));
        stopped_future.wait();

        work_.reset();
        network_thread_.join();
    }

    void report_error(error value)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        error_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = error_handler_;
        }
        if (handler)
        {
            handler(value);
        }
    }

    void notify_connected()
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        connection_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = connected_handler_;
        }
        if (handler)
        {
            handler();
        }
    }

    void notify_disconnected()
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        connection_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = disconnected_handler_;
        }
        if (handler)
        {
            handler();
        }
    }

    void notify_message(message value, bool updated)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        message_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = updated ? message_updated_handler_ : message_handler_;
        }
        if (handler)
        {
            handler(std::move(value));
        }
    }

    void notify_read(std::int64_t conversation, std::int64_t user, std::int64_t message)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        read_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = read_handler_;
        }
        if (handler)
        {
            handler(conversation, user, message);
        }
    }

    void notify_presence(presence value)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }

        presence_handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = presence_handler_;
        }
        if (handler)
        {
            handler(std::move(value));
        }
    }

    void fail_pending(error value)
    {
        auto pending = std::move(pending_);
        pending_.clear();
        for (auto& [id, handler] : pending)
        {
            if (suppress_callbacks_.load())
            {
                return;
            }
            (void)id;
            handler(std::unexpected(value));
        }
    }

    void send_request(std::string method, boost::json::object params, response_handler handler)
    {
        if (suppress_callbacks_.load())
        {
            return;
        }
        if (state_ != connection_state::connected || closing_)
        {
            handler(std::unexpected(make_error(error_kind::transport, "Not connected")));
            return;
        }

        auto const id = next_request_id_++;
        boost::json::object request;
        request.emplace("jsonrpc", "2.0");
        request.emplace("method", std::move(method));
        request.emplace("params", std::move(params));
        request.emplace("id", id);
        auto payload = boost::json::serialize(request);
        if (payload.size() > detail::max_websocket_request_size)
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Request exceeds 64 KiB")));
            return;
        }
        pending_.emplace(id, std::move(handler));
        outgoing_.push_back(std::move(payload));
        websocket_.interrupt_receive();
    }

    void receive(std::string const& payload)
    {
        boost::system::error_code ec;
        auto value = boost::json::parse(payload, ec);
        if (ec || !value.is_object())
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC message"));
            return;
        }

        auto const& object = value.as_object();
        auto const* version = object.if_contains("jsonrpc");
        if (!version || !version->is_string() || version->as_string() != "2.0")
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC message"));
            return;
        }

        auto const* id_value = object.if_contains("id");
        if (!id_value)
        {
            receive_notification(object);
            return;
        }

        receive_response(object, *id_value);
    }

    void receive_notification(boost::json::object const& object)
    {
        auto const* method = object.if_contains("method");
        auto const* params = object.if_contains("params");
        if (!method || !method->is_string() || !params || !params->is_object())
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC notification"));
            return;
        }

        if (method->as_string() == "login_session_revoked" || method->as_string() == "login_sessions_changed")
        {
            auto const revoked = method->as_string() == "login_session_revoked";
            if (revoked)
            {
                forget_login();
                closing_ = true;
                outgoing_.clear();
                websocket_.cancel();
                stop_file_transfers();
            }
            connection_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = revoked ? login_revoked_handler_ : login_sessions_changed_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler();
            }
            return;
        }

        if (method->as_string() == "message" || method->as_string() == "message_updated")
        {
            message notification;
            if (!parse_message(params->as_object(), notification))
            {
                report_error(make_error(error_kind::protocol, "Invalid message notification"));
                return;
            }
            notify_message(std::move(notification), method->as_string() == "message_updated");
            return;
        }

        if (method->as_string() == "message_bookmark_changed")
        {
            auto const* value = params->as_object().if_contains("message");
            if (!value || !value->is_int64() || value->as_int64() <= 0)
            {
                report_error(make_error(error_kind::protocol, "Invalid bookmark notification"));
                return;
            }
            bookmark_changed_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = bookmark_changed_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler(value->as_int64());
            }
            return;
        }

        if (method->as_string() == "reaction")
        {
            reaction_update notification;
            if (!parse_reaction_update(params->as_object(), notification))
            {
                report_error(make_error(error_kind::protocol, "Invalid reaction notification"));
                return;
            }
            reaction_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = reaction_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler(std::move(notification));
            }
            return;
        }

        if (method->as_string() == "read")
        {
            auto const* conversation_value = params->as_object().if_contains("conversation");
            auto const* user_value = params->as_object().if_contains("user");
            auto const* message_value = params->as_object().if_contains("message");
            if (!conversation_value || !user_value || !message_value)
            {
                report_error(make_error(error_kind::protocol, "Invalid read notification"));
                return;
            }
            auto conversation = parse_int64(*conversation_value);
            auto user = parse_int64(*user_value);
            auto read_message = parse_int64(*message_value);
            if (!conversation || *conversation <= 0 || !user || *user <= 0 || !read_message || *read_message <= 0)
            {
                report_error(make_error(error_kind::protocol, "Invalid read notification"));
                return;
            }
            notify_read(*conversation, *user, *read_message);
            return;
        }

        if (method->as_string() == "typing")
        {
            auto const& fields = params->as_object();
            auto const* conversation_field = fields.if_contains("conversation");
            auto const* user_field = fields.if_contains("user");
            auto const* username = fields.if_contains("username");
            auto const* typing = fields.if_contains("typing");
            auto conversation = conversation_field ? parse_int64(*conversation_field) : std::nullopt;
            auto user = user_field ? parse_int64(*user_field) : std::nullopt;
            if (!conversation || *conversation <= 0 || !user || *user <= 0 || !username || !username->is_string() || username->as_string().empty() ||
                !typing || !typing->is_bool())
            {
                report_error(make_error(error_kind::protocol, "Invalid typing notification"));
                return;
            }
            typing_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = typing_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler({*conversation, *user, std::string(username->as_string()), typing->as_bool()});
            }
            return;
        }

        if (method->as_string() == "presence")
        {
            presence notification;
            if (!parse_presence(params->as_object(), notification))
            {
                report_error(make_error(error_kind::protocol, "Invalid presence notification"));
                return;
            }
            notify_presence(std::move(notification));
            return;
        }

        if (method->as_string() == "contacts_changed")
        {
            connection_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = contacts_changed_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler();
            }
            return;
        }
        if (method->as_string() == "friendship" || method->as_string() == "profile_changed")
        {
            auto const* field = params->as_object().if_contains("user");
            auto user = field ? parse_int64(*field) : std::nullopt;
            if (!user || *user <= 0)
            {
                report_error(make_error(error_kind::protocol, "Invalid friendship notification"));
                return;
            }
            friendship_changed_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = method->as_string() == "friendship" ? friendship_handler_ : profile_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler(*user);
            }
            return;
        }

        if (method->as_string() == "conversation_view")
        {
            auto view = boost::json::try_value_to<conversation_view_event>(*params);
            if (!view || (view->action == "clear" && view->through.value_or(-1) < 0) ||
                (view->action == "notifications" && (!view->level || *view->level < 0 || *view->level > 2 || !view->until)))
            {
                report_error(make_error(error_kind::protocol, "Invalid personal conversation state"));
                return;
            }
            conversation_view_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = view_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler(std::move(*view));
            }
            return;
        }
        if (method->as_string() == "conversation")
        {
            auto const* value = params->as_object().if_contains("conversation");
            auto const* removed = params->as_object().if_contains("removed");
            auto id = value ? parse_int64(*value) : std::nullopt;
            if (!id || *id <= 0 || (removed && !removed->is_bool()))
            {
                report_error(make_error(error_kind::protocol, "Invalid conversation notification"));
                return;
            }
            auto const unavailable = removed && removed->as_bool();
            if (unavailable && outbox_account_ && journal_.is_open())
            {
                if (auto e = journal_.exec("UPDATE text_outbox SET rejection=?,reply=NULL WHERE user=? AND conversation=?",
                                           conversation_unavailable,
                                           outbox_account_,
                                           *id);
                    e)
                {
                    report_error(outbox_error("无法更新不可用会话的待发送消息"));
                }
                if (auto items = read_pending_texts())
                {
                    for (auto& item : *items)
                    {
                        if (item.conversation == *id)
                        {
                            notify_outbox(std::move(item));
                        }
                    }
                }
            }
            conversation_changed_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = conversation_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler(*id, unavailable);
            }
            return;
        }

        if (method->as_string() == "join_request")
        {
            auto const& object = params->as_object();
            auto const* conversation = object.if_contains("conversation");
            auto const* user = object.if_contains("user");
            auto const* state = object.if_contains("state");
            auto id = conversation ? parse_int64(*conversation) : std::nullopt;
            auto applicant = user ? parse_int64(*user) : std::nullopt;
            if (!id || *id <= 0 || !applicant || *applicant <= 0 || !state || !state->is_string() ||
                (state->as_string() != "pending" && state->as_string() != "accepted" && state->as_string() != "rejected"))
            {
                report_error(make_error(error_kind::protocol, "Invalid join request notification"));
                return;
            }
            group_join_request_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = join_request_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler({*id,
                         *applicant,
                         state->as_string() == "pending"    ? group_join_request_state::pending
                         : state->as_string() == "accepted" ? group_join_request_state::accepted
                                                            : group_join_request_state::rejected});
            }
            return;
        }

        if (method->as_string() == "avatar")
        {
            auto const* field = params->as_object().if_contains("user");
            auto user = field ? parse_int64(*field) : std::nullopt;
            avatar_state state;
            if (!user || *user <= 0 || !parse_avatar_state(params->as_object(), state))
            {
                report_error(make_error(error_kind::protocol, "Invalid avatar notification"));
                return;
            }
            avatar_changed_handler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = avatar_handler_;
            }
            if (handler && !suppress_callbacks_.load())
            {
                handler(*user, state);
            }
            return;
        }
        report_error(make_error(error_kind::protocol, "Invalid JSON-RPC notification"));
    }

    void receive_response(boost::json::object const& object, boost::json::value const& id_value)
    {
        auto id = parse_response_id(id_value);
        if (!id)
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC response id"));
            return;
        }

        auto pending = pending_.find(*id);
        if (pending == pending_.end())
        {
            report_error(make_error(error_kind::protocol, "Unknown JSON-RPC response id"));
            return;
        }

        auto handler = std::move(pending->second);
        pending_.erase(pending);
        if (suppress_callbacks_.load())
        {
            return;
        }

        auto const* result = object.if_contains("result");
        auto const* error_value = object.if_contains("error");
        if ((result == nullptr) == (error_value == nullptr))
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Invalid JSON-RPC response")));
            return;
        }

        if (result)
        {
            handler(*result);
            return;
        }

        if (!error_value->is_object())
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Invalid JSON-RPC error")));
            return;
        }

        auto const& error_object = error_value->as_object();
        auto const* code = error_object.if_contains("code");
        auto const* error_message = error_object.if_contains("message");
        if (!code || !code->is_int64() || !error_message || !error_message->is_string())
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Invalid JSON-RPC error")));
            return;
        }

        handler(std::unexpected(make_error(error_kind::rpc, std::string(error_message->as_string()), static_cast<int>(code->as_int64()))));
    }

    boost::capy::task<> open(std::string url)
    {
        if (state_ != connection_state::disconnected)
        {
            report_error(make_error(error_kind::transport, "Connection already open"));
            co_return;
        }

        if (endpoint_ != url)
        {
            outbox_account_ = 0;
            login_username_.clear();
            login_token_.clear();
        }
        endpoint_ = url;
        transfer_stop_ = std::stop_source{};
        state_ = connection_state::connecting;
        closing_ = false;

        auto [connect_ec] = co_await websocket_.connect(url);
        if (connect_ec)
        {
            websocket_.close();
            state_ = connection_state::disconnected;
            if (!closing_)
            {
                report_error(make_error(error_kind::transport, connect_ec.message()));
            }
            closing_ = false;
            co_return;
        }

        if (closing_)
        {
            websocket_.close();
            state_ = connection_state::disconnected;
            closing_ = false;
            co_return;
        }

        state_ = connection_state::connected;
        notify_connected();

        std::optional<std::error_code> connection_error;
        for (;;)
        {
            while (!outgoing_.empty() && !closing_)
            {
                auto message = std::move(outgoing_.front());
                outgoing_.pop_front();
                auto [send_ec] = co_await websocket_.send_text(message);
                if (send_ec)
                {
                    connection_error = send_ec;
                    break;
                }
            }

            if (closing_ || connection_error)
            {
                break;
            }

            auto receive_result = co_await websocket_.receive();
            auto& [receive_ec, message] = receive_result;
            if (receive_ec)
            {
                if (receive_ec == std::errc::interrupted)
                {
                    continue;
                }
                connection_error = receive_ec;
                break;
            }
            if (message.message_type == detail::websocket_message::type::close)
            {
                break;
            }
            receive(message.payload);
        }

        if (connection_error && !closing_)
        {
            // The fair control-frame pump may have received complete RPC replies while
            // sending. Deliver those before failing requests whose replies never arrived.
            while (!closing_ && !suppress_callbacks_.load())
            {
                auto message = websocket_.take_pending_message();
                if (!message)
                {
                    break;
                }
                if (message->message_type == detail::websocket_message::type::text)
                {
                    receive(message->payload);
                }
            }
        }
        websocket_.close();
        outgoing_.clear();
        state_ = connection_state::disconnected;
        stop_file_transfers();
        fail_pending(make_error(error_kind::transport, "Connection closed"));
        deferred_text_conversations_.clear();

        if (connection_error && !closing_)
        {
            report_error(make_error(error_kind::transport, connection_error->message()));
        }
        closing_ = false;
        notify_disconnected();
    }

    boost::capy::task<> close(bool keep_local_account)
    {
        if (!keep_local_account)
        {
            outbox_account_ = 0;
        }
        if (state_ != connection_state::disconnected)
        {
            closing_ = true;
            websocket_.cancel();
        }
        co_return;
    }

    boost::capy::task<> shutdown(std::promise<void>* stopped)
    {
        closing_ = true;
        websocket_.cancel();
        stop_file_transfers();
        stopped->set_value();
        co_return;
    }

    void forget_login()
    {
        if (journal_.is_open() && !login_token_.empty())
        {
            if (auto error = journal_.exec("DELETE FROM login_credentials WHERE username=? AND token=?", login_username_, login_token_); error)
            {
                report_error(outbox_error("无法清理本地登录凭证"));
            }
        }
        login_username_.clear();
        login_token_.clear();
    }

    boost::capy::task<> authenticate(
        std::string username, std::optional<std::string> password, std::string device_name, std::string client_type, authenticate_handler handler)
    {
        boost::json::object params;
        if (!account_)
        {
            if (username != outbox_username_)
            {
                outbox_account_ = 0;
            }
            // An unavailable upload directory must not prevent a password login.
            journal_ = {};
            if (auto db = detail::open_transfer_journal(transfer_directory_, endpoint_))
            {
                if (auto secret = detail::installation_secret(*db))
                {
                    params.emplace("device_secret", std::move(*secret));
                    journal_ = std::move(*db);
                }
            }
        }
        auto const resuming = !password;
        if (resuming)
        {
            if ((login_username_ != username || login_token_.empty()) && journal_.is_open())
            {
                auto saved = journal_.prepare("SELECT token FROM login_credentials WHERE username=?", username);
                if (saved)
                {
                    if (auto row = saved->step(); row && *row)
                    {
                        login_token_ = saved->column_text(0);
                        login_username_ = username;
                    }
                }
            }
            if (login_username_ != username || login_token_.empty())
            {
                handler(std::unexpected(make_error(error_kind::rpc, "No saved login session", login_session_invalid)));
                co_return;
            }
            params.emplace("token", login_token_);
        }
        else
        {
            params.emplace("username", username);
            params.emplace("password", std::move(*password));
            if (!device_name.empty())
            {
                params.emplace("device_name", std::move(device_name));
            }
            if (!client_type.empty())
            {
                params.emplace("client_type", std::move(client_type));
            }
        }
        send_request(resuming ? "resume_session" : "authenticate",
                     std::move(params),
                     [this, resuming, username = std::move(username), handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             if (resuming && response.error().kind == error_kind::rpc && response.error().code == login_session_invalid)
                             {
                                 forget_login();
                             }
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto const* object = response->if_object();
                         auto const* authenticated = object ? object->if_contains("authenticated") : nullptr;
                         auto const* user_value = object ? object->if_contains("user") : nullptr;
                         auto user = user_value ? parse_int64(*user_value) : std::nullopt;
                         if (!authenticated || !authenticated->is_bool() || !user)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid authenticate result")));
                             return;
                         }

                         authentication_result value{authenticated->as_bool(), *user, {}};
                         if (!parse_avatar_state(*object, value.avatar))
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid authenticate avatar")));
                             return;
                         }
                         if (value.authenticated)
                         {
                             auto const* token = object->if_contains("token");
                             if (!resuming)
                             {
                                 if (!token || !token->is_string() || token->as_string().size() != 64)
                                 {
                                     handler(std::unexpected(make_error(error_kind::protocol, "Missing login token")));
                                     return;
                                 }
                                 login_token_ = token->as_string();
                                 login_username_ = username;
                                 if (journal_.is_open())
                                 {
                                     if (auto error = journal_.exec(R"SQL(
                                         INSERT INTO login_credentials(username,token) VALUES(?,?)
                                         ON CONFLICT(username) DO UPDATE SET token=excluded.token
                                     )SQL",
                                                                    login_username_,
                                                                    login_token_);
                                         error)
                                     {
                                         report_error(outbox_error("无法保存登录凭证，仅在本次运行中保留"));
                                     }
                                 }
                             }
                             account_ = value.user;
                             outbox_account_ = value.user;
                             outbox_username_ = std::move(username);
                         }
                         handler(value);
                         if (value.authenticated)
                         {
                             send_next_text();
                         }
                     });
        co_return;
    }

    boost::capy::task<> list_login_sessions(login_sessions_handler handler)
    {
        co_await request_page<login_session_list>("list_login_sessions",
                                                  {},
                                                  "Invalid login session list",
                                                  [handler = std::move(handler)](std::expected<login_session_list, error> result)
                                                  {
                                                      if (!result)
                                                      {
                                                          handler(std::unexpected(std::move(result.error())));
                                                          return;
                                                      }
                                                      handler(std::move(result->sessions));
                                                  });
    }

    boost::capy::task<> logout_session(group_action_handler handler)
    {
        // Clear only the credential. The UI flushes the draft before retiring the journal owner.
        send_request("logout_session",
                     {},
                     [this, handler = std::move(handler)](auto reply) mutable
                     {
                         forget_login();
                         if (!reply)
                         {
                             handler(std::unexpected(std::move(reply.error())));
                         }
                         else
                         {
                             handler(true);
                         }
                     });
        co_return;
    }

    boost::capy::task<> register_user(std::string username, std::string password, register_handler handler)
    {
        boost::json::object params;
        params.emplace("username", std::move(username));
        params.emplace("password", std::move(password));

        send_request("register",
                     std::move(params),
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         if (!response->is_object())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid register result")));
                             return;
                         }

                         auto const* user_value = response->as_object().if_contains("user");
                         if (!user_value)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid register result")));
                             return;
                         }

                         auto user = parse_int64(*user_value);
                         if (!user || *user <= 0)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid register result")));
                             return;
                         }
                         handler(*user);
                     });

        co_return;
    }

    boost::capy::task<> get_conversations(std::optional<conversation_cursor> before, conversations_handler handler)
    {
        boost::json::object params;
        if (before)
        {
            params.emplace("before", boost::json::object{{"activity", before->activity}, {"id", before->id}, {"pinned", before->pinned}});
        }
        send_request("get_conversations",
                     std::move(params),
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         if (!response->is_object())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversations")));
                             return;
                         }
                         auto const* list = response->as_object().if_contains("conversations");
                         auto const* next = response->as_object().if_contains("next");
                         if (!list || !list->is_array() || !next)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversations")));
                             return;
                         }
                         conversations_result result;
                         auto const* folders = response->as_object().if_contains("folders");
                         if (!folders)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Missing conversation folders")));
                             return;
                         }
                         auto values = boost::json::try_value_to<std::vector<conversation_folder>>(*folders);
                         if (!values)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation folders")));
                             return;
                         }
                         result.folders = std::move(*values);
                         for (auto const& value : list->as_array())
                         {
                             auto item = parse_conversation(value);
                             if (!item)
                             {
                                 handler(std::unexpected(std::move(item.error())));
                                 return;
                             }
                             result.conversations.push_back(std::move(*item));
                         }
                         if (!next->is_null())
                         {
                             if (!next->is_object())
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid cursor")));
                                 return;
                             }
                             auto const* activity = next->as_object().if_contains("activity");
                             auto const* id = next->as_object().if_contains("id");
                             auto const* pinned = next->as_object().if_contains("pinned");
                             auto a = activity ? parse_int64(*activity) : std::nullopt;
                             auto i = id ? parse_int64(*id) : std::nullopt;
                             if (!a || *a <= 0 || !i || *i <= 0 || !pinned || !pinned->is_bool())
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid cursor")));
                                 return;
                             }
                             result.next = conversation_cursor{*a, *i, pinned->as_bool()};
                         }
                         handler(std::move(result));
                     });
        co_return;
    }

    boost::capy::task<> open_direct_conversation(std::int64_t user, conversation_handler handler)
    {
        send_request("open_direct_conversation",
                     {{"user", user}},
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto const* id_value = response->is_object() ? response->as_object().if_contains("conversation") : nullptr;
                         auto id = id_value ? parse_int64(*id_value) : std::nullopt;
                         if (!id || *id <= 0)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid direct conversation result")));
                             return;
                         }
                         handler(*id);
                     });
        co_return;
    }

    boost::capy::task<> create_group(std::string title, std::vector<std::int64_t> members, conversation_handler handler)
    {
        boost::json::array values;
        for (auto id : members)
        {
            values.emplace_back(id);
        }
        send_request("create_group",
                     {{"title", std::move(title)}, {"members", std::move(values)}},
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto const* value = response->is_object() ? response->as_object().if_contains("conversation") : nullptr;
                         auto id = value ? parse_int64(*value) : std::nullopt;
                         if (!id || *id <= 0)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation result")));
                             return;
                         }
                         handler(*id);
                     });
        co_return;
    }

    boost::capy::task<> get_members(std::int64_t conversation, members_handler handler)
    {
        send_request("get_members",
                     {{"conversation", conversation}},
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto const* values = response->is_object() ? response->as_object().if_contains("members") : nullptr;
                         if (!values || !values->is_array())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid members result")));
                             return;
                         }
                         std::vector<conversation_member> members;
                         for (auto const& value : values->as_array())
                         {
                             user person;
                             auto const* role = value.is_object() ? value.as_object().if_contains("role") : nullptr;
                             auto const* speech_until = value.is_object() ? value.as_object().if_contains("speech_until") : nullptr;
                             if (!speech_until || !speech_until->is_int64() || !role || !role->is_string() ||
                                 !parse_user(value.as_object(), person) ||
                                 (role->as_string() != "owner" && role->as_string() != "admin" && role->as_string() != "member"))
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid member")));
                                 return;
                             }
                             members.push_back({person.id,
                                                std::move(person.username),
                                                role->as_string() == "owner"   ? member_role::owner
                                                : role->as_string() == "admin" ? member_role::admin
                                                                               : member_role::member,
                                                person.avatar,
                                                speech_until->as_int64(),
                                                std::move(person.nickname),
                                                string_field(value.as_object(), "remark")});
                         }
                         handler(std::move(members));
                     });
        co_return;
    }

    boost::capy::task<> group_invite(std::string method, std::int64_t conversation, group_invite_handler handler)
    {
        auto const creating = method == "create_group_invite";
        auto const revoking = method == "revoke_group_invite";
        send_request(
            std::move(method),
            {{"conversation", conversation}},
            [creating, revoking, handler = std::move(handler)](auto response) mutable
            {
                if (!response)
                {
                    handler(std::unexpected(std::move(response.error())));
                    return;
                }
                auto const* token = response->is_object() ? response->as_object().if_contains("token") : nullptr;
                if (!token || (!token->is_null() && !token->is_string()) || (creating && token->is_null()) || (revoking && !token->is_null()))
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid group invite")));
                    return;
                }
                std::optional<std::string> value;
                if (token->is_string())
                {
                    value = std::string(token->as_string());
                    if (!valid_invite_token(*value))
                    {
                        handler(std::unexpected(make_error(error_kind::protocol, "Invalid invite token")));
                        return;
                    }
                }
                handler(std::move(value));
            });
        co_return;
    }

    boost::capy::task<> join_group(std::string token, group_join_handler handler)
    {
        send_request("join_group",
                     {{"token", std::move(token)}},
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         if (!response->is_object())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid group join result")));
                             return;
                         }
                         auto const& object = response->as_object();
                         auto const* conversation = object.if_contains("conversation");
                         auto const* title = object.if_contains("title");
                         auto const* count = object.if_contains("member_count");
                         auto const* state = object.if_contains("state");
                         auto id = conversation ? parse_int64(*conversation) : std::nullopt;
                         auto members = count ? parse_uint64(*count) : std::nullopt;
                         if (!id || *id <= 0 || !title || !title->is_string() || title->as_string().empty() || title->as_string().size() > 256 ||
                             title->as_string().find('\0') != boost::json::string::npos || !members || *members == 0 || !state ||
                             !state->is_string() ||
                             (state->as_string() != "joined" && state->as_string() != "member" && state->as_string() != "pending"))
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid group join result")));
                             return;
                         }
                         handler(group_join_result{*id,
                                                   std::string(title->as_string()),
                                                   *members,
                                                   state->as_string() == "joined"   ? group_join_state::joined
                                                   : state->as_string() == "member" ? group_join_state::member
                                                                                    : group_join_state::pending});
                     });
        co_return;
    }

    boost::capy::task<> get_group_join_requests(std::int64_t conversation, std::optional<std::int64_t> before, group_join_requests_handler handler)
    {
        boost::json::object params{{"conversation", conversation}};
        if (before)
        {
            params.emplace("before", *before);
        }
        send_request("get_group_join_requests",
                     std::move(params),
                     [before, handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto const* requests = response->is_object() ? response->as_object().if_contains("requests") : nullptr;
                         auto const* next = response->is_object() ? response->as_object().if_contains("next") : nullptr;
                         if (!requests || !requests->is_array() || requests->as_array().size() > 50 || !next)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid join requests")));
                             return;
                         }
                         group_join_requests_result result;
                         for (auto const& value : requests->as_array())
                         {
                             group_join_request item;
                             auto const* created = value.is_object() ? value.as_object().if_contains("created_at") : nullptr;
                             auto time = created ? parse_int64(*created) : std::nullopt;
                             if (!value.is_object() || !parse_user(value.as_object(), item.applicant) || !time || *time <= 0 ||
                                 (before && item.applicant.id >= *before) ||
                                 (!result.requests.empty() && item.applicant.id >= result.requests.back().applicant.id))
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid join request")));
                                 return;
                             }
                             item.created_at = *time;
                             result.requests.push_back(std::move(item));
                         }
                         if (!next->is_null())
                         {
                             auto cursor = parse_int64(*next);
                             if (!cursor || *cursor <= 0 || result.requests.size() != 50 || *cursor != result.requests.back().applicant.id)
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid join request cursor")));
                                 return;
                             }
                             result.next = cursor;
                         }
                         handler(std::move(result));
                     });
        co_return;
    }

    boost::capy::task<> request_bool(std::string method,
                                     boost::json::object params,
                                     char const* field,
                                     char const* invalid_result,
                                     std::function<void(std::expected<bool, error>)> handler)
    {
        send_request(std::move(method),
                     std::move(params),
                     [field, invalid_result, handler = std::move(handler)](auto response)
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto const* value = response->is_object() ? response->as_object().if_contains(field) : nullptr;
                         if (!value || !value->is_bool())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, invalid_result)));
                             return;
                         }
                         handler(value->as_bool());
                     });
        co_return;
    }

    boost::capy::task<> group_action(std::string method, boost::json::object params, group_action_handler handler)
    {
        return request_bool(std::move(method), std::move(params), "changed", "Invalid group action result", std::move(handler));
    }

    template <class Page, class Handler>
    boost::capy::task<> request_page(std::string method, boost::json::object params, char const* invalid_result, Handler handler)
    {
        send_request(std::move(method),
                     std::move(params),
                     [invalid_result, handler = std::move(handler)](auto r)
                     {
                         if (!r)
                         {
                             handler(std::unexpected(r.error()));
                             return;
                         }
                         auto page = boost::json::try_value_to<Page>(*r);
                         if (!page)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, invalid_result)));
                             return;
                         }
                         handler(std::move(*page));
                     });
        co_return;
    }
    boost::capy::task<> get_presence(presences_handler handler)
    {
        send_request("get_presence",
                     {},
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         if (!response->is_object())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_presence result")));
                             return;
                         }

                         auto const* users_value = response->as_object().if_contains("users");
                         if (!users_value || !users_value->is_array())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_presence result")));
                             return;
                         }

                         std::vector<presence> users;
                         users.reserve(users_value->as_array().size());
                         for (auto const& value : users_value->as_array())
                         {
                             if (!value.is_object())
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid presence")));
                                 return;
                             }

                             presence item;
                             if (!parse_presence(value.as_object(), item))
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid presence")));
                                 return;
                             }
                             users.push_back(std::move(item));
                         }

                         handler(std::move(users));
                     });

        co_return;
    }

    boost::capy::task<> get_messages(std::int64_t conversation,
                                     std::optional<std::int64_t> before,
                                     messages_handler handler,
                                     std::optional<std::int64_t> after,
                                     std::optional<std::string> query = {},
                                     std::optional<std::int64_t> around = {},
                                     std::optional<message_category> category = {},
                                     bool capture_unread = false)
    {
        boost::json::object params;
        params.emplace("conversation", conversation);
        if (before)
        {
            params.emplace("before", *before);
        }

        if (after)
        {
            params.emplace("after", *after);
        }
        if (around)
        {
            params.emplace("around", *around);
        }
        if (query)
        {
            params.emplace("query", std::move(*query));
        }
        if (category)
        {
            params.emplace("category", category_name(*category));
        }
        if (capture_unread)
        {
            params.emplace("capture_unread", true);
        }
        send_request(
            query && !category ? "search_messages" : "get_messages",
            std::move(params),
            [conversation, handler = std::move(handler)](auto response) mutable
            {
                if (!response)
                {
                    handler(std::unexpected(std::move(response.error())));
                    return;
                }
                if (!response->is_object())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_messages result")));
                    return;
                }

                auto const* messages_value = response->as_object().if_contains("messages");
                auto const* read_value = response->as_object().if_contains("read_positions");
                auto const* more_value = response->as_object().if_contains("has_more");
                if (!messages_value || !messages_value->is_array() || !read_value || !read_value->is_array() || !more_value || !more_value->is_bool())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_messages result")));
                    return;
                }

                messages_result result;
                result.has_more = more_value->as_bool();
                auto const* floor = response->as_object().if_contains("visible_after");
                auto const* hidden = response->as_object().if_contains("hidden");
                if (!floor || !parse_int64(*floor) || !hidden || !hidden->is_bool())
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid history visibility")));
                    return;
                }
                result.visible_after = *parse_int64(*floor);
                result.hidden = hidden->as_bool();
                if (auto const* first = response->as_object().if_contains("first_unread"))
                {
                    result.first_unread = parse_int64(*first);
                }
                for (auto const& position : read_value->as_array())
                {
                    auto const* user_value = position.is_object() ? position.as_object().if_contains("user") : nullptr;
                    auto const* message_value = position.is_object() ? position.as_object().if_contains("message") : nullptr;
                    auto user = user_value ? parse_int64(*user_value) : std::nullopt;
                    auto message = message_value ? parse_int64(*message_value) : std::nullopt;
                    if (!user || *user <= 0 || !message || *message < 0)
                    {
                        handler(std::unexpected(make_error(error_kind::protocol, "Invalid read position")));
                        return;
                    }
                    result.read_positions.push_back(read_position{*user, *message});
                }
                result.messages.reserve(messages_value->as_array().size());
                for (auto const& value : messages_value->as_array())
                {
                    if (!value.is_object())
                    {
                        handler(std::unexpected(make_error(error_kind::protocol, "Invalid message")));
                        return;
                    }

                    message item;
                    if (!parse_message(value.as_object(), item) || item.conversation != conversation)
                    {
                        handler(std::unexpected(make_error(error_kind::protocol, "Invalid message")));
                        return;
                    }
                    result.messages.push_back(std::move(item));
                }
                handler(std::move(result));
            });

        co_return;
    }

    static boost::json::object text_params(pending_text const& item)
    {
        boost::json::object params{{"conversation", item.conversation}, {"text", item.text}, {"client_id", item.client_id}};
        if (item.reply_to > 0)
        {
            params.emplace("reply_to", item.reply_to);
        }
        return params;
    }

    static error outbox_error(std::string message) { return make_error(error_kind::local_storage, std::move(message)); }

    std::expected<std::vector<pending_text>, error> read_pending_texts()
    {
        auto query = journal_.prepare(
            "SELECT client,conversation,COALESCE(reply,0),created,body,rejection FROM text_outbox WHERE user=? ORDER BY rowid", outbox_account_);
        if (!query)
        {
            return std::unexpected(outbox_error("无法读取本地待发送消息"));
        }
        std::vector<pending_text> items;
        for (;;)
        {
            auto row = query->step();
            if (!row)
            {
                return std::unexpected(outbox_error("无法读取本地待发送消息"));
            }
            if (!*row)
            {
                return items;
            }
            auto const id = query->column_int64(0);
            auto const conversation = query->column_int64(1);
            auto const active = sending_texts_.find(conversation);
            bool const sending = active != sending_texts_.end() && active->second == id;
            auto const rejection = static_cast<int>(query->column_int64(5));
            auto state = sending ? outbox_state::sending : outbox_state::queued;
            if (rejection != 0)
            {
                state = outbox_state::failed;
            }
            items.push_back({
                .client_id = id,
                .conversation = conversation,
                .reply_to = query->column_int64(2),
                .created = query->column_int64(3),
                .text = std::string(query->column_text(4)),
                .state = state,
                .rejection = rejection,
            });
        }
    }

    void notify_outbox(pending_text item)
    {
        outbox_handler handler;
        {
            std::scoped_lock const lock(handler_mutex_);
            handler = outbox_handler_;
        }
        if (handler && !suppress_callbacks_.load())
        {
            handler(std::move(item));
        }
    }

    void send_next_text()
    {
        if (account_ == 0 || account_ != outbox_account_ || !journal_.is_open() || closing_ || state_ != connection_state::connected ||
            suppress_callbacks_.load())
        {
            return;
        }
        auto items = read_pending_texts();
        if (!items)
        {
            report_error(items.error());
            return;
        }
        for (auto& item : *items)
        {
            if (item.state != outbox_state::queued || deferred_text_conversations_.contains(item.conversation) ||
                !sending_texts_.try_emplace(item.conversation, item.client_id).second)
            {
                continue;
            }
            send_pending_text(std::move(item));
        }
    }

    void send_pending_text(pending_text item)
    {
        item.state = outbox_state::sending;
        notify_outbox(item);
        auto params = text_params(item);
        send_request(
            "send_message",
            std::move(params),
            [this, user = account_, item = std::move(item)](auto response) mutable
            {
                sending_texts_.erase(item.conversation);
                message value;
                if (response && response->is_object() && parse_receipt(response->as_object(), value) && value.from == user &&
                    value.conversation == item.conversation && value.client_id == item.client_id && !value.attachment)
                {
                    if (!journal_.exec("DELETE FROM text_outbox WHERE user=? AND client=?", user, item.client_id))
                    {
                        notify_message(std::move(value), false);
                        send_next_text();
                        return;
                    }
                    report_error(outbox_error("服务器已确认消息，但无法清理本地待发送记录"));
                }
                else if (!response && response.error().kind == error_kind::rpc)
                {
                    auto const code = response.error().code;
                    if (!journal_.exec(R"SQL(UPDATE text_outbox SET rejection=?1,
                                           reply=CASE WHEN ?1=?5 THEN NULL ELSE reply END
                                           WHERE user=?2 AND conversation=?3
                                           AND (client=?4 OR (?1 IN (?5,?6) AND rejection=0)))SQL",
                                       code,
                                       user,
                                       item.conversation,
                                       item.client_id,
                                       conversation_unavailable,
                                       group_speech_muted))
                    {
                        // The head already got a definitive rejection. Only now can the
                        // never-dispatched rows behind it stop without losing a receipt.
                        if (auto rows = read_pending_texts())
                        {
                            for (auto& row : *rows)
                            {
                                if (row.conversation == item.conversation && row.rejection == code)
                                {
                                    notify_outbox(std::move(row));
                                }
                            }
                        }
                        send_next_text();
                        return;
                    }
                    report_error(outbox_error("无法保存消息的拒绝状态"));
                }
                else if (response || response.error().kind != error_kind::transport)
                {
                    report_error(response ? make_error(error_kind::protocol, "Invalid send_message result") : response.error());
                }
                // An ambiguous outcome must not overtake an earlier message in this conversation.
                deferred_text_conversations_.emplace(item.conversation, item.client_id);
                item.state = outbox_state::queued;
                notify_outbox(std::move(item));
                send_next_text();
            });
    }

    boost::capy::task<> save_draft(std::int64_t user, compose_draft value, outbox_operation_handler done)
    {
        if (user <= 0 || user != outbox_account_ || value.conversation <= 0 || value.revision <= 0 || value.reply < 0)
        {
            done(std::unexpected(outbox_error("无法保存草稿")));
            co_return;
        }
        auto const error = value.text.empty() && !value.reply
                               ? journal_.exec("DELETE FROM compose_drafts WHERE user=? AND conversation=?", user, value.conversation)
                               : journal_.exec(R"SQL(
                INSERT INTO compose_drafts(user,conversation,revision,reply,body,updated)
                VALUES(?,?,?,?,?,unixepoch())
                ON CONFLICT(user,conversation) DO UPDATE SET revision=excluded.revision,reply=excluded.reply,
                  body=excluded.body,updated=excluded.updated
              )SQL",
                                               user,
                                               value.conversation,
                                               value.revision,
                                               value.reply,
                                               std::string_view(value.text));
        done(error ? std::unexpected(outbox_error("无法保存草稿，请检查本地存储")) : std::expected<void, chat::error>{});
    }

    boost::capy::task<> load_drafts(std::int64_t user, drafts_handler done)
    {
        if (user <= 0 || user != outbox_account_)
        {
            done(std::unexpected(outbox_error("无法读取草稿")));
            co_return;
        }
        auto query = journal_.prepare("SELECT conversation,revision,reply,body FROM compose_drafts WHERE user=?", user);
        if (!query)
        {
            done(std::unexpected(outbox_error("无法读取草稿")));
            co_return;
        }
        std::vector<compose_draft> drafts;
        for (;;)
        {
            auto row = query->step();
            if (!row)
            {
                done(std::unexpected(outbox_error("无法读取草稿")));
                co_return;
            }
            if (!*row)
            {
                done(std::move(drafts));
                co_return;
            }
            drafts.push_back({query->column_int64(0), query->column_int64(1), query->column_int64(2), std::string(query->column_text(3))});
        }
    }

    boost::capy::task<> queue_text(pending_text item, std::int64_t draft_revision, outbox_operation_handler handler)
    {
        if (outbox_account_ == 0)
        {
            handler(std::unexpected(make_error(error_kind::transport, "Not authenticated")));
            co_return;
        }
        if (item.conversation <= 0 || item.client_id <= 0 || item.reply_to < 0 || item.text.empty() || item.text.contains('\0'))
        {
            handler(std::unexpected(make_error(error_kind::rpc, "Invalid params", -32602)));
            co_return;
        }
        boost::json::object const request{
            {"jsonrpc", "2.0"}, {"method", "send_message"}, {"params", text_params(item)}, {"id", std::numeric_limits<std::int64_t>::max()}};
        if (boost::json::serialize(request).size() > detail::max_websocket_request_size)
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Request exceeds 64 KiB")));
            co_return;
        }
        item.created = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        auto transaction = sqlite_transaction::begin_immediate(journal_);
        if (!transaction ||
            journal_.exec("INSERT INTO text_outbox(user,client,conversation,body,reply,created) VALUES(?,?,?,?,NULLIF(?,0),?)",
                          outbox_account_,
                          item.client_id,
                          item.conversation,
                          item.text,
                          item.reply_to,
                          item.created) ||
            (draft_revision &&
             journal_.exec(
                 "DELETE FROM compose_drafts WHERE user=? AND conversation=? AND revision=?", outbox_account_, item.conversation, draft_revision)) ||
            transaction->commit())
        {
            handler(std::unexpected(outbox_error("无法保存待发送消息，请检查本地存储")));
            co_return;
        }

        handler({});
        notify_outbox(std::move(item));
        send_next_text();
    }

    boost::capy::task<> list_texts(pending_texts_handler handler)
    {
        if (outbox_account_ == 0)
        {
            handler(std::unexpected(outbox_error("无法读取本地待发送消息")));
            co_return;
        }
        auto items = read_pending_texts();
        // Queued rows are reconciled normally by client_id. A previously muted
        // row will not be retried, so check its membership once on restoration.
        if (!items || !account_ || std::ranges::none_of(*items, [](auto const& item) { return item.rejection == group_speech_muted; }))
        {
            handler(std::move(items));
            co_return;
        }
        co_await request_page<std::vector<message_search_scope>>(
            "message_search_scopes",
            {},
            "Invalid search scopes",
            message_search_scopes_handler{
                [this, user = outbox_account_, items = std::move(*items), handler = std::move(handler)](auto scopes) mutable
                {
                    if (scopes && user == outbox_account_)
                    {
                        for (auto const& item : items)
                        {
                            if (item.rejection == group_speech_muted &&
                                std::ranges::find(*scopes, item.conversation, &message_search_scope::id) == scopes->end())
                            {
                                if (auto e =
                                        journal_.exec("UPDATE text_outbox SET rejection=?1,reply=NULL WHERE user=?2 AND client=?3 AND rejection=?4",
                                                      conversation_unavailable,
                                                      user,
                                                      item.client_id,
                                                      group_speech_muted);
                                    e)
                                {
                                    handler(std::unexpected(outbox_error("无法更新不可用会话的待发送消息")));
                                    return;
                                }
                            }
                        }
                        handler(read_pending_texts());
                    }
                    else
                    {
                        handler(std::move(items));
                    }
                }});
    }

    boost::capy::task<> change_text(std::int64_t id, bool discard, outbox_operation_handler handler)
    {
        if (outbox_account_ == 0 || (!discard && account_ == 0) ||
            std::ranges::any_of(sending_texts_, [id](auto const& active) { return active.second == id; }))
        {
            handler(std::unexpected(outbox_error("当前无法操作这条待发送消息")));
            co_return;
        }
        if (journal_.exec(discard ? "DELETE FROM text_outbox WHERE user=? AND client=?"
                                  : "UPDATE text_outbox SET rejection=0 WHERE user=? AND client=? AND rejection<>0",
                          outbox_account_,
                          id) ||
            journal_.changes() == 0)
        {
            handler(std::unexpected(outbox_error("无法更新本地待发送消息")));
            co_return;
        }
        if (discard)
        {
            std::erase_if(deferred_text_conversations_, [id](auto const& head) { return head.second == id; });
        }
        handler({});
        send_next_text();
    }

    using upload_result = std::expected<std::optional<message>, error>;
    using upload_handler = std::function<void(upload_result)>;
    struct upload_job
    {
        pending_attachment task;
        detail::inspected_file file;
        std::stop_source stop;
        bool putting = false;    // Only the SDK executor starts and releases a part request.
        upload_handler handler;
        progress_handler progress;
    };
    using job_ptr = std::shared_ptr<upload_job>;
    static error file_queue_error(blocking_worker::unavailable) { return make_error(error_kind::file_transfer, "附件工作线程繁忙，请稍后继续"); }
    void end_upload(job_ptr const& job, upload_result result)
    {
        auto it = uploads_.find(job->task.client_id);
        if (it == uploads_.end() || it->second != job)
        {
            return;
        }
        uploads_.erase(it);
        if (suppress_callbacks_)
        {
            return;
        }
        if (result)
        {
            if (auto e = journal_.exec("DELETE FROM tasks WHERE user=? AND client=?", account_, job->task.client_id); e)
            {
                report_error(make_error(error_kind::file_transfer, "无法清理已发送的本地上传记录"));
            }
        }
        else if (!job->stop.stop_requested() && state_ == connection_state::connected && !closing_)
        {
            send_request("pause_attachment", {{"upload", job->task.client_id}}, [](auto) {});
        }
        job->handler(std::move(result));
    }
    void accept_file_message(job_ptr const& job, boost::json::value const& response)
    {
        message m;
        if (!response.is_object() || !parse_receipt(response.as_object(), m) || m.from != account_ || m.client_id != job->task.client_id ||
            m.conversation != job->task.conversation ||
            (!m.acknowledgement_only && !m.deleted && (!m.attachment || m.attachment->size != job->file.size)))
        {
            end_upload(job, std::unexpected(make_error(error_kind::protocol, "Invalid attachment acknowledgement")));
            return;
        }
        end_upload(job, std::move(m));
    }
    boost::capy::task<> notify_file_progress(job_ptr job, std::int64_t current, std::int64_t total)
    {
        if (!job->stop.stop_requested() && !suppress_callbacks_ && uploads_.contains(job->task.client_id))
        {
            job->progress(current, total);
        }
        co_return;
    }
    boost::capy::task<> transfer_part(job_ptr job, std::string url, int number, std::int64_t offset, std::int64_t length)
    {
        if (job->stop.stop_requested())
        {
            co_return;
        }
        job->putting = true;
        progress_handler progress;
        if (job->progress)
        {
            progress = [this, job](auto current, auto total)
            { boost::capy::run_async(io_context_.get_executor())(notify_file_progress(job, current, total)); };
        }
        auto result = co_await file_worker_.run(
            [&] { return detail::put_part(url, job->task.path, job->file, offset, length, job->stop.get_token(), progress); }, file_queue_error);
        job->putting = false;
        if (job->stop.stop_requested() || suppress_callbacks_)
        {
            end_upload(job, std::unexpected(make_error(error_kind::file_transfer, "上传已暂停")));
            co_return;
        }
        if (!result)
        {
            end_upload(job, std::unexpected(result.error()));
            co_return;
        }
        send_request("confirm_attachment",
                     {{"upload", job->task.client_id}, {"number", number}, {"etag", *result}},
                     [this, job](auto r) { on_grant(job, std::move(r)); });
    }
    void attach_group_file(job_ptr const& job, boost::json::value const& response)
    {
        auto* obj = response.if_object();
        auto* key = obj ? obj->if_contains("object_key") : nullptr;
        if (!key || !key->is_string())
        {
            end_upload(job, std::unexpected(make_error(error_kind::protocol, "Invalid completed file")));
            return;
        }
        send_request("add_conversation_file",
                     {{"conversation_id", job->task.conversation}, {"object_key", *key}},
                     [this, job](auto r)
                     {
                         if (!r)
                         {
                             end_upload(job, std::unexpected(r.error()));
                             return;
                         }
                         auto* fields = r->if_object();
                         auto* changed = fields ? fields->if_contains("changed") : nullptr;
                         if (!changed || !changed->is_bool())
                         {
                             end_upload(job, std::unexpected(make_error(error_kind::protocol, "Invalid group file result")));
                             return;
                         }
                         end_upload(job, std::optional<message>{});
                     });
    }
    void on_grant(job_ptr const& job, std::expected<boost::json::value, error> response)
    {
        if (job->stop.stop_requested() || suppress_callbacks_)
        {
            return;
        }
        if (!response)
        {
            end_upload(job, std::unexpected(response.error()));
            return;
        }
        auto* obj = response->if_object();
        if (obj)
        {
            if (job->task.group_file && obj->contains("object_key"))
            {
                attach_group_file(job, *response);
                return;
            }
            if (auto* msg = obj->if_contains("message"); !job->task.group_file && msg)
            {
                accept_file_message(job, *msg);
                return;
            }
        }
        auto integer = [&](char const* key)
        {
            auto* v = obj ? obj->if_contains(key) : nullptr;
            return v ? parse_int64(*v).value_or(-1) : -1;
        };
        auto n = integer("number"), offset = integer("offset"), length = integer("length"), part_size = integer("part_size"),
             count = integer("part_count");
        auto* url = obj ? obj->if_contains("url") : nullptr;
        if (integer("upload") != job->task.client_id || integer("size") != job->file.size || n < 0 || n > count ||
            part_size != attachment_part_size || count != std::max<std::int64_t>(1, (job->file.size + part_size - 1) / part_size) ||
            offset != (n == 0 ? job->file.size : (n - 1) * part_size) || length != (n == 0 ? 0 : std::min(part_size, job->file.size - offset)) ||
            !url || !url->is_string())
        {
            end_upload(job, std::unexpected(make_error(error_kind::protocol, "Invalid multipart layout")));
            return;
        }
        if (journal_.exec("UPDATE tasks SET touched=CAST(strftime('%s','now') AS INTEGER),part_size=?,count=?,number=?,confirmed=?,length=? WHERE "
                          "user=? AND client=?",
                          part_size,
                          count,
                          n,
                          offset,
                          length,
                          account_,
                          job->task.client_id))
        {
            end_upload(job, std::unexpected(make_error(error_kind::file_transfer, "无法保存分片进度")));
            return;
        }
        if (job->progress)
        {
            job->progress(offset, job->file.size);
        }
        if (n)
        {
            boost::capy::run_async(io_context_.get_executor())(
                transfer_part(job, std::string(url->as_string()), static_cast<int>(n), offset, length));
            return;
        }
        boost::json::object params{{"upload", job->task.client_id}};
        if (job->task.reply_to)
        {
            params.emplace("reply_to", job->task.reply_to);
        }
        send_request(job->task.group_file ? "complete_file" : "finish_attachment",
                     std::move(params),
                     [this, job](auto r)
                     {
                         if (job->stop.stop_requested())
                         {
                             return;
                         }
                         if (!r)
                         {
                             end_upload(job, std::unexpected(r.error()));
                             return;
                         }
                         if (job->task.group_file)
                         {
                             attach_group_file(job, *r);
                         }
                         else
                         {
                             accept_file_message(job, *r);
                         }
                     });
    }
    boost::capy::task<> send_file(pending_attachment task, upload_handler handler, progress_handler progress)
    {
        if (!journal_.is_open())
        {
            handler(std::unexpected(make_error(error_kind::file_transfer, "本地上传目录不可用，请检查应用数据目录权限")));
            co_return;
        }
        if (!account_ || uploads_.contains(task.client_id))
        {
            handler(std::unexpected(make_error(error_kind::file_transfer, "尚未登录或任务正在上传")));
            co_return;
        }
        std::error_code path_error;
        auto absolute = std::filesystem::absolute(task.path, path_error);
        if (path_error)
        {
            handler(std::unexpected(make_error(error_kind::file_transfer, "无法定位原文件")));
            co_return;
        }
        task.path = absolute.string();
        auto job = std::make_shared<upload_job>();
        job->task = std::move(task);
        job->handler = std::move(handler);
        job->progress = std::move(progress);
        auto const& t = job->task;
        uploads_.emplace(t.client_id, job);
        auto fail = [&](std::string msg) { end_upload(job, std::unexpected(make_error(error_kind::file_transfer, std::move(msg)))); };
        auto fp = detail::file_snapshot(t.path);
        if (!fp)
        {
            fail(fp.error().message);
            co_return;
        }
        {
            auto tx = sqlite_transaction::begin_immediate(journal_);
            if (!tx)
            {
                fail("无法保存上传任务");
                co_return;
            }
            if (journal_.exec("INSERT OR IGNORE INTO tasks(user,client,conversation,path,size,stamp,reply,checksum) VALUES(?,?,?,?,?,?,?,'')",
                              account_,
                              t.client_id,
                              t.conversation,
                              t.path,
                              fp->first,
                              fp->second,
                              t.reply_to))
            {
                fail("无法保存上传任务");
                co_return;
            }
            if (journal_.changes() && t.group_file && journal_.exec("INSERT INTO group_file_tasks(user,client) VALUES(?,?)", account_, t.client_id))
            {
                fail("无法保存群文件任务");
                co_return;
            }
            if (tx->commit())
            {
                fail("无法保存上传任务");
                co_return;
            }
        }
        std::string checksum;
        bool resuming = false;
        {
            auto q = journal_.prepare(
                "SELECT path,conversation,size,stamp,checksum,part_size,EXISTS(SELECT 1 FROM group_file_tasks g WHERE "
                "(g.user,g.client)=(tasks.user,tasks.client)) FROM tasks WHERE user=? AND client=?",
                account_,
                t.client_id);
            if (!q)
            {
                fail("无法读取上传记录");
                co_return;
            }
            auto row = q->step();
            if (!row || !*row)
            {
                fail("上传记录不可用");
                co_return;
            }
            if (q->column_text(0) != t.path || q->column_int64(1) != t.conversation || q->column_int64(2) != fp->first ||
                q->column_int64(3) != fp->second || (q->column_int64(6) != 0) != t.group_file)
            {
                fail("原文件已变更，无法继续上传");
                co_return;
            }
            checksum = q->column_text(4);
            resuming = q->column_int64(5) > 0;
        }
        auto file = co_await file_worker_.run([&] { return detail::inspect_file(t.path, job->stop.get_token()); }, file_queue_error);
        if (job->stop.stop_requested() || suppress_callbacks_)
        {
            co_return;
        }
        if (!file)
        {
            end_upload(job, std::unexpected(file.error()));
            co_return;
        }
        if (file->size != fp->first || file->stamp != fp->second || (!checksum.empty() && file->checksum != checksum))
        {
            fail("原文件已变更，无法继续上传");
            co_return;
        }
        job->file = std::move(*file);
        if (journal_.exec("UPDATE tasks SET checksum=?,reply=? WHERE user=? AND client=?", job->file.checksum, t.reply_to, account_, t.client_id))
        {
            fail("无法保存文件校验信息");
            co_return;
        }
        if (resuming)
        {
            send_request("resume_attachment", {{"upload", t.client_id}}, [this, job](auto r) { on_grant(job, std::move(r)); });
            co_return;
        }
        send_request("begin_attachment",
                     {{"conversation", t.conversation},
                      {"client_id", t.client_id},
                      {"size", job->file.size},
                      {"filename", t.filename},
                      {"media_type", job->file.media},
                      {"checksum", job->file.checksum}},
                     [this, job](auto r) { on_grant(job, std::move(r)); });
    }
    boost::capy::task<> restore_attachments(pending_attachments_handler handler)
    {
        std::vector<pending_attachment> records;
        if (!account_)
        {
            handler(std::unexpected(make_error(error_kind::transport, "Not authenticated")));
            co_return;
        }
        if (!journal_.is_open())
        {
            handler(std::unexpected(make_error(error_kind::file_transfer, "本地上传目录不可用，请检查应用数据目录权限")));
            co_return;
        }
        auto q = journal_.prepare(
            "SELECT client,conversation,path,size,reply,confirmed,EXISTS(SELECT 1 FROM group_file_tasks g WHERE "
            "(g.user,g.client)=(tasks.user,tasks.client)) FROM tasks WHERE user=? ORDER BY client",
            account_);
        if (!q)
        {
            handler(std::unexpected(make_error(error_kind::file_transfer, "无法读取上传任务")));
            co_return;
        }
        for (;;)
        {
            auto row = q->step();
            if (!row)
            {
                handler(std::unexpected(make_error(error_kind::file_transfer, "无法读取上传任务")));
                co_return;
            }
            if (!*row)
            {
                break;
            }
            auto path = std::string(q->column_text(2));
            auto name = std::filesystem::path(path).filename().string();
            records.push_back({q->column_int64(0),
                               q->column_int64(1),
                               q->column_int64(3),
                               q->column_int64(4),
                               q->column_int64(5),
                               std::move(path),
                               std::move(name),
                               q->column_int64(6) != 0});
        }
        handler(std::move(records));
        co_return;
    }
    boost::capy::task<> interrupt_attachment(std::int64_t id, bool cancel, group_action_handler handler)
    {
        if (!journal_.is_open())
        {
            handler(std::unexpected(make_error(error_kind::file_transfer, "本地上传目录不可用，请检查应用数据目录权限")));
            co_return;
        }
        if (auto it = uploads_.find(id); it != uploads_.end())
        {
            auto job = it->second;
            job->stop.request_stop();
            if (!job->putting)
            {
                end_upload(job, std::unexpected(make_error(error_kind::file_transfer, "上传已暂停")));
            }
        }
        send_request(cancel ? "cancel_attachment" : "pause_attachment",
                     {{"upload", id}},
                     [this, id, cancel, handler = std::move(handler)](auto r)
                     {
                         if (!r && !(r.error().kind == error_kind::rpc && r.error().code == -32008))
                         {
                             handler(std::unexpected(r.error()));
                             return;
                         }
                         if (cancel && journal_.exec("DELETE FROM tasks WHERE user=? AND client=?", account_, id))
                         {
                             handler(std::unexpected(make_error(error_kind::file_transfer, "无法清理上传记录")));
                             return;
                         }
                         handler(true);
                     });
        co_return;
    }
    boost::capy::task<> save_download(std::string url,
                                      std::string checksum,
                                      std::int64_t size,
                                      std::string target,
                                      attachment_handler handler,
                                      std::stop_token stop,
                                      download_options options)
    {
        auto result = co_await file_worker_.run([&] { return detail::download_file(url, size, checksum, target, stop, options); }, file_queue_error);
        if (!suppress_callbacks_)
        {
            handler(std::move(result));
        }
    }
    boost::capy::task<> get_attachment(
        std::string method, boost::json::object params, std::string target, std::int64_t limit, attachment_handler handler, download_options options)
    {
        send_request(std::move(method),
                     std::move(params),
                     [this,
                      target = std::move(target),
                      limit,
                      handler = std::move(handler),
                      options = std::move(options),
                      stop = transfer_stop_.get_token()](auto r) mutable
                     {
                         if (stop.stop_requested() || (options.control && options.control->cancelled()))
                         {
                             handler(std::unexpected(make_error(error_kind::file_transfer, "下载已取消或连接已断开")));
                             return;
                         }
                         if (!r)
                         {
                             handler(std::unexpected(r.error()));
                             return;
                         }
                         auto* obj = r->if_object();
                         auto* v = obj ? obj->if_contains("size") : nullptr;
                         auto size = v ? parse_int64(*v) : std::nullopt;
                         auto* url = obj ? obj->if_contains("url") : nullptr;
                         auto* hash = obj ? obj->if_contains("checksum") : nullptr;
                         if (!size || *size < 0 || *size > limit || !url || !url->is_string() || !hash || !hash->is_string())
                         {
                             handler(std::unexpected(make_error(error_kind::file_transfer, "附件过大或下载信息无效")));
                             return;
                         }
                         boost::capy::run_async(io_context_.get_executor())(save_download(std::string(url->as_string()),
                                                                                          std::string(hash->as_string()),
                                                                                          *size,
                                                                                          std::move(target),
                                                                                          std::move(handler),
                                                                                          std::move(stop),
                                                                                          std::move(options)));
                     });
        co_return;
    }
    void stop_file_transfers()
    {
        transfer_stop_.request_stop();
        account_ = 0;
        auto const jobs = uploads_;
        for (auto const& [id, job] : jobs)
        {
            job->stop.request_stop();
            if (!job->putting)
            {
                end_upload(job, std::unexpected(make_error(error_kind::transport, "连接已断开，上传已暂停")));
            }
        }
    }
    boost::capy::task<> set_message_reaction(std::int64_t conversation, std::int64_t id, std::string emoji, reaction_result_handler handler)
    {
        send_request("set_message_reaction",
                     {{"conversation", conversation}, {"message", id}, {"emoji", std::move(emoji)}},
                     [conversation, id, handler = std::move(handler)](auto result) mutable
                     {
                         if (!result)
                         {
                             handler(std::unexpected(std::move(result.error())));
                             return;
                         }
                         reaction_update value;
                         if (!result->is_object() || !parse_reaction_update(result->as_object(), value) || value.conversation != conversation ||
                             value.message != id)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid reaction result")));
                             return;
                         }
                         handler(std::move(value));
                     });
        co_return;
    }

    boost::capy::task<> request_message(std::string method,
                                        boost::json::object params,
                                        std::int64_t conversation,
                                        std::optional<std::int64_t> expected_id,
                                        message_result_handler handler)
    {
        send_request(std::move(method),
                     std::move(params),
                     [conversation, expected_id, handler = std::move(handler)](auto result) mutable
                     {
                         if (!result)
                         {
                             handler(std::unexpected(std::move(result.error())));
                             return;
                         }
                         message value;
                         if (!result->is_object() ||
                             !(expected_id ? parse_message(result->as_object(), value) : parse_receipt(result->as_object(), value)) ||
                             value.conversation != conversation || (expected_id && value.id != *expected_id))
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid message result")));
                             return;
                         }
                         handler(std::move(value));
                     });
        co_return;
    }

    boost::capy::task<> search_users(std::string query, users_handler handler)
    {
        boost::json::object params;
        params.emplace("query", std::move(query));

        send_request("search_users",
                     std::move(params),
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         if (!response->is_object())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid search_users result")));
                             return;
                         }

                         auto const* users_value = response->as_object().if_contains("users");
                         if (!users_value || !users_value->is_array())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid search_users result")));
                             return;
                         }

                         std::vector<user> users;
                         users.reserve(users_value->as_array().size());
                         for (auto const& value : users_value->as_array())
                         {
                             if (!value.is_object())
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid user")));
                                 return;
                             }

                             user item;
                             if (!parse_user(value.as_object(), item))
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid user")));
                                 return;
                             }
                             users.push_back(std::move(item));
                         }

                         handler(std::move(users));
                     });

        co_return;
    }

    boost::capy::task<> change_friendship(std::string method, boost::json::object params, friendship_handler handler)
    {
        send_request(std::move(method),
                     std::move(params),
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto const* user = response->is_object() ? response->as_object().if_contains("user") : nullptr;
                         auto state = response->is_object() ? parse_friendship_state(response->as_object().if_contains("state")) : std::nullopt;
                         friendship_result result;
                         if (!user || !user->is_object() || !state || !parse_user(user->as_object(), result.user))
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid friendship result")));
                             return;
                         }
                         result.state = *state;
                         handler(std::move(result));
                     });
        co_return;
    }

    boost::capy::task<> get_friend_requests(friend_requests_handler handler)
    {
        send_request("get_friend_requests",
                     {},
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         friend_requests_result result;
                         auto parse = [&](char const* name, std::vector<friend_request>& output)
                         {
                             auto const* list = response->is_object() ? response->as_object().if_contains(name) : nullptr;
                             if (!list || !list->is_array())
                             {
                                 return false;
                             }
                             std::unordered_set<std::int64_t> seen;
                             for (auto const& entry : list->as_array())
                             {
                                 auto const* user = entry.is_object() ? entry.as_object().if_contains("user") : nullptr;
                                 auto const* time = entry.is_object() ? entry.as_object().if_contains("created_at") : nullptr;
                                 auto created = time ? parse_int64(*time) : std::nullopt;
                                 friend_request request;
                                 if (!user || !user->is_object() || !created || *created < 0 || !parse_user(user->as_object(), request.user) ||
                                     !seen.insert(request.user.id).second)
                                 {
                                     return false;
                                 }
                                 request.created_at = *created;
                                 output.push_back(std::move(request));
                             }
                             return true;
                         };
                         if (!parse("incoming", result.incoming) || !parse("outgoing", result.outgoing))
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid friend requests result")));
                             return;
                         }
                         for (auto const& incoming : result.incoming)
                         {
                             if (std::ranges::any_of(result.outgoing, [&](auto const& outgoing) { return incoming.user.id == outgoing.user.id; }))
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Conflicting friend requests")));
                                 return;
                             }
                         }
                         handler(std::move(result));
                     });
        co_return;
    }

    boost::capy::task<> clear_history(std::int64_t conversation, std::int64_t through, mark_read_handler handler)
    {
        send_request("clear_history",
                     {{"conversation", conversation}, {"through", through}},
                     [through, handler = std::move(handler)](auto r)
                     {
                         if (!r)
                         {
                             handler(std::unexpected(r.error()));
                             return;
                         }
                         auto field = r->is_object() ? r->as_object().if_contains("through") : nullptr;
                         auto actual = field ? parse_int64(*field) : std::nullopt;
                         if (!actual || *actual < through)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid history boundary")));
                             return;
                         }
                         handler(*actual);
                     });
        co_return;
    }

    boost::capy::task<> mark_read(std::int64_t conversation, std::int64_t message_id, mark_read_handler handler)
    {
        send_request("mark_read",
                     {{"conversation", conversation}, {"message", message_id}},
                     [handler = std::move(handler)](auto response)
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto const* message_value = response->is_object() ? response->as_object().if_contains("message") : nullptr;
                         auto read_message = message_value ? parse_int64(*message_value) : std::nullopt;
                         if (!read_message || *read_message <= 0)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid mark_read result")));
                             return;
                         }
                         handler(*read_message);
                     });

        co_return;
    }

    struct avatar_upload_job
    {
        std::int64_t upload = 0;
        std::string data;
        std::size_t offset = 0;
        avatar_state_handler handler;
    };

    void fail_avatar_upload(std::shared_ptr<avatar_upload_job> const& job, error value)
    {
        if (job->upload > 0)
        {
            send_request("cancel_avatar_upload", {{"upload", job->upload}}, [](auto) {});
        }
        job->handler(std::unexpected(std::move(value)));
    }

    void upload_avatar_next(std::shared_ptr<avatar_upload_job> job)
    {
        if (job->offset == job->data.size())
        {
            send_request("finish_avatar_upload",
                         {{"upload", job->upload}},
                         [this, job](auto response)
                         {
                             avatar_state state;
                             if (!response)
                             {
                                 fail_avatar_upload(job, std::move(response.error()));
                             }
                             else if (!response->is_object() || !parse_avatar_state(response->as_object(), state) || !state.present)
                             {
                                 fail_avatar_upload(job, make_error(error_kind::protocol, "Invalid avatar upload result"));
                             }
                             else
                             {
                                 job->handler(state);
                             }
                         });
            return;
        }
        auto count = std::min(avatar_chunk_size, job->data.size() - job->offset);
        send_request("upload_avatar_chunk",
                     {{"upload", job->upload},
                      {"offset", job->offset},
                      {"data", detail::encode_base64(std::string_view(job->data).substr(job->offset, count))}},
                     [this, job, count](auto response)
                     {
                         if (!response)
                         {
                             fail_avatar_upload(job, std::move(response.error()));
                             return;
                         }
                         auto const* field = response->is_object() ? response->as_object().if_contains("offset") : nullptr;
                         auto offset = field ? parse_int64(*field) : std::nullopt;
                         if (!offset || *offset != static_cast<std::int64_t>(job->offset + count))
                         {
                             fail_avatar_upload(job, make_error(error_kind::protocol, "Invalid avatar upload offset"));
                             return;
                         }
                         job->offset += count;
                         upload_avatar_next(job);
                     });
    }

    boost::capy::task<> set_avatar(std::string data, avatar_state_handler handler, std::int64_t group)
    {
        if (suppress_callbacks_.load())
        {
            co_return;
        }
        if (data.empty() || data.size() > max_avatar_size)
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Avatar size must be between 1 byte and 1 MiB")));
            co_return;
        }
        auto job = std::make_shared<avatar_upload_job>();
        job->data = std::move(data);
        job->handler = std::move(handler);
        boost::json::object params{{"size", job->data.size()}};
        if (group)
        {
            params.emplace("conversation", group);
        }
        send_request("begin_avatar_upload",
                     std::move(params),
                     [this, job](auto response)
                     {
                         if (!response)
                         {
                             fail_avatar_upload(job, std::move(response.error()));
                             return;
                         }
                         auto const* field = response->is_object() ? response->as_object().if_contains("upload") : nullptr;
                         auto upload = field ? parse_int64(*field) : std::nullopt;
                         if (!upload || *upload <= 0)
                         {
                             fail_avatar_upload(job, make_error(error_kind::protocol, "Invalid avatar upload identity"));
                             return;
                         }
                         job->upload = *upload;
                         upload_avatar_next(job);
                     });
        co_return;
    }

    struct avatar_download_job
    {
        avatar value;
        std::optional<std::int64_t> size;
        avatar_handler handler;
    };

    void download_avatar_next(std::shared_ptr<avatar_download_job> job)
    {
        send_request(
            "get_avatar",
            {{job->value.conversation ? "conversation" : "user", job->value.conversation ? job->value.conversation : job->value.user},
             {"revision", job->value.state.revision},
             {"offset", job->value.data.size()}},
            [this, job](auto response)
            {
                if (!response)
                {
                    job->handler(std::unexpected(std::move(response.error())));
                    return;
                }
                auto const* fields = response->is_object() ? &response->as_object() : nullptr;
                avatar_state state;
                auto const* user_field = fields ? fields->if_contains(job->value.conversation ? "conversation" : "user") : nullptr;
                auto const* size_field = fields ? fields->if_contains("size") : nullptr;
                auto const* offset_field = fields ? fields->if_contains("offset") : nullptr;
                auto const* data_field = fields ? fields->if_contains("data") : nullptr;
                auto const* type_field = fields ? fields->if_contains("media_type") : nullptr;
                auto const* more_field = fields ? fields->if_contains("has_more") : nullptr;
                auto user = user_field ? parse_int64(*user_field) : std::nullopt;
                auto size = size_field ? parse_int64(*size_field) : std::nullopt;
                auto offset = offset_field ? parse_int64(*offset_field) : std::nullopt;
                if (!fields || !parse_avatar_state(*fields, state) || !user ||
                    *user != (job->value.conversation ? job->value.conversation : job->value.user) || !size || *size < 0 ||
                    *size > static_cast<std::int64_t>(max_avatar_size) || !offset || *offset < 0 || !data_field || !data_field->is_string() ||
                    !type_field || !type_field->is_string() || !more_field || !more_field->is_bool() ||
                    data_field->as_string().size() > 4 * ((avatar_chunk_size + 2) / 3))
                {
                    job->handler(std::unexpected(make_error(error_kind::protocol, "Invalid avatar chunk")));
                    return;
                }
                if (state.revision != job->value.state.revision || !state.present)
                {
                    if (*size != 0 || *offset != 0 || !data_field->as_string().empty() || !type_field->as_string().empty() || more_field->as_bool())
                    {
                        job->handler(std::unexpected(make_error(error_kind::protocol, "Invalid stale avatar metadata")));
                        return;
                    }
                    job->handler(avatar{job->value.user, job->value.conversation, state, {}, {}});
                    return;
                }
                auto const type = std::string_view(type_field->as_string());
                auto bytes = detail::decode_base64(std::string_view(data_field->as_string()));
                if (*size == 0 || *offset != static_cast<std::int64_t>(job->value.data.size()) || *offset > *size ||
                    (job->size && *job->size != *size) || (type != "image/png" && type != "image/jpeg") ||
                    (!job->value.media_type.empty() && job->value.media_type != type) || !bytes ||
                    bytes->size() != std::min(avatar_chunk_size, static_cast<std::size_t>(*size - *offset)) ||
                    more_field->as_bool() != (*offset + static_cast<std::int64_t>(bytes->size()) < *size))
                {
                    job->handler(std::unexpected(make_error(error_kind::protocol, "Invalid avatar chunk data")));
                    return;
                }
                job->value.state = state;
                job->value.media_type = type;
                job->size = size;
                job->value.data.append(*bytes);
                if (more_field->as_bool())
                {
                    download_avatar_next(job);
                }
                else
                {
                    job->handler(std::move(job->value));
                }
            });
    }

    boost::capy::task<> get_avatar(std::int64_t user, std::int64_t revision, avatar_handler handler, std::int64_t group = 0)
    {
        auto job = std::make_shared<avatar_download_job>();
        job->value.user = user;
        job->value.conversation = group;
        job->value.state.revision = revision;
        job->handler = std::move(handler);
        download_avatar_next(job);
        co_return;
    }

    boost::capy::task<> clear_avatar(avatar_state_handler handler, std::int64_t group)
    {
        boost::json::object params;
        if (group)
        {
            params.emplace("conversation", group);
        }
        send_request("clear_avatar",
                     std::move(params),
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         avatar_state state;
                         if (!response->is_object() || !parse_avatar_state(response->as_object(), state) || state.present)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid clear avatar result")));
                             return;
                         }
                         handler(state);
                     });
        co_return;
    }

    std::string transfer_directory_, endpoint_;
    sqlite_database journal_;
    std::string login_username_, login_token_;
    std::int64_t account_ = 0;
    // Keep the authenticated journal owner across transport loss, including a same-user reconnect.
    std::int64_t outbox_account_ = 0;
    std::string outbox_username_;
    std::unordered_map<std::int64_t, std::int64_t> sending_texts_;
    std::unordered_map<std::int64_t, std::int64_t> deferred_text_conversations_;
    outbox_handler outbox_handler_;
    blocking_worker file_worker_{64, 2};
    std::stop_source transfer_stop_;
    boost::corosio::io_context io_context_{1};
    detail::websocket_client websocket_;
    boost::capy::work_guard<boost::corosio::io_context::executor_type> work_;
    std::thread network_thread_;

    std::int64_t next_request_id_ = 1;
    std::unordered_map<std::int64_t, response_handler> pending_;
    std::unordered_map<std::int64_t, job_ptr> uploads_;
    std::deque<std::string> outgoing_;
    connection_state state_ = connection_state::disconnected;
    bool closing_ = false;

    std::mutex handler_mutex_;
    connection_handler connected_handler_;
    connection_handler disconnected_handler_;
    connection_handler login_revoked_handler_, login_sessions_changed_handler_;
    error_handler error_handler_;
    message_handler message_handler_;
    message_handler message_updated_handler_;
    read_handler read_handler_;
    conversation_view_handler view_handler_;
    conversation_changed_handler conversation_handler_;
    presence_handler presence_handler_;
    typing_handler typing_handler_;
    avatar_changed_handler avatar_handler_;
    reaction_handler reaction_handler_;
    bookmark_changed_handler bookmark_changed_handler_;
    group_join_request_handler join_request_handler_;
    connection_handler contacts_changed_handler_;
    friendship_changed_handler profile_handler_;
    friendship_changed_handler friendship_handler_;
    std::atomic_bool suppress_callbacks_ = false;
};

client::client(std::string directory) : impl_(std::make_unique<impl>(std::move(directory))) {}

client::~client()
{
    if (impl_->network_thread_.get_id() == std::this_thread::get_id())
    {
        impl_->suppress_callbacks_.store(true);
        // Shutdown joins the network thread after the current callback returns.
        std::thread([state = std::move(impl_)] mutable { state.reset(); }).detach();
    }
}

void client::set_avatar_handler(avatar_changed_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->avatar_handler_ = std::move(handler);
}

void client::set_avatar(std::string data, avatar_state_handler handler, std::int64_t group)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->set_avatar(std::move(data), std::move(handler), group));
}

void client::get_group_avatar(std::int64_t group, std::int64_t revision, avatar_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_avatar(0, revision, std::move(handler), group));
}

void client::get_avatar(std::int64_t user, std::int64_t revision, avatar_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_avatar(user, revision, std::move(handler)));
}

void client::clear_avatar(avatar_state_handler handler, std::int64_t group)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->clear_avatar(std::move(handler), group));
}

void client::set_connected_handler(connection_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->connected_handler_ = std::move(handler);
}

void client::set_disconnected_handler(connection_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->disconnected_handler_ = std::move(handler);
}

void client::set_login_revoked_handler(connection_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->login_revoked_handler_ = std::move(handler);
}

void client::set_login_sessions_changed_handler(connection_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->login_sessions_changed_handler_ = std::move(handler);
}

void client::set_error_handler(error_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->error_handler_ = std::move(handler);
}

void client::set_message_handler(message_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->message_handler_ = std::move(handler);
}

void client::set_message_updated_handler(message_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->message_updated_handler_ = std::move(handler);
}

void client::set_bookmark_changed_handler(bookmark_changed_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->bookmark_changed_handler_ = std::move(handler);
}

void client::set_reaction_handler(reaction_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->reaction_handler_ = std::move(handler);
}

void client::set_group_join_request_handler(group_join_request_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->join_request_handler_ = std::move(handler);
}

void client::set_read_handler(read_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->read_handler_ = std::move(handler);
}

void client::set_presence_handler(presence_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->presence_handler_ = std::move(handler);
}

void client::set_typing_handler(typing_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->typing_handler_ = std::move(handler);
}

void client::connect(std::string url) { boost::capy::run_async(impl_->io_context_.get_executor())(impl_->open(std::move(url))); }

void client::close(bool keep_local_account) { boost::capy::run_async(impl_->io_context_.get_executor())(impl_->close(keep_local_account)); }

void client::authenticate(std::string username, std::string password, authenticate_handler handler, std::string device_name, std::string client_type)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->authenticate(std::move(username), std::move(password), std::move(device_name), std::move(client_type), std::move(handler)));
}

void client::resume_session(std::string username, authenticate_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->authenticate(std::move(username), {}, {}, {}, std::move(handler)));
}

void client::get_login_sessions(login_sessions_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->list_login_sessions(std::move(handler)));
}

void client::revoke_login_session(std::int64_t session, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("revoke_login_session", {{"session", session}}, std::move(handler)));
}

void client::revoke_other_login_sessions(group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action("revoke_other_login_sessions", {}, std::move(handler)));
}

void client::logout_session(group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->logout_session(std::move(handler)));
}

void client::change_password(std::string current, std::string replacement, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action(
        "change_password", {{"current_password", std::move(current)}, {"new_password", std::move(replacement)}}, std::move(handler)));
}

void client::register_user(std::string username, std::string password, register_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->register_user(std::move(username), std::move(password), std::move(handler)));
}

void client::get_conversations(std::optional<conversation_cursor> before, conversations_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_conversations(before, std::move(handler)));
}

void client::set_notification_level(std::int64_t conversation, notification_level level, std::int64_t until, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action(
        "set_notification_level", {{"conversation", conversation}, {"level", static_cast<int>(level)}, {"until", until}}, std::move(handler)));
}
void client::mark_conversations_read(std::optional<std::int64_t> conversation, std::optional<std::int64_t> folder, group_action_handler handler)
{
    boost::json::object params;
    if (conversation)
    {
        params.emplace("conversation", *conversation);
    }
    if (folder)
    {
        params.emplace("folder", *folder);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action("mark_conversations_read", std::move(params), std::move(handler)));
}

void client::set_unread_mark(std::int64_t conversation, bool marked, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_unread_mark", {{"conversation", conversation}, {"marked", marked}}, std::move(handler)));
}
void client::create_conversation_folder(std::string name, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("create_conversation_folder", {{"name", std::move(name)}}, std::move(handler)));
}

void client::rename_conversation_folder(std::int64_t folder, std::string name, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("rename_conversation_folder", {{"folder", folder}, {"name", std::move(name)}}, std::move(handler)));
}

void client::delete_conversation_folder(std::int64_t folder, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("delete_conversation_folder", {{"folder", folder}}, std::move(handler)));
}

void client::reorder_conversation_folder(std::int64_t folder, bool up, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("reorder_conversation_folder", {{"folder", folder}, {"up", up}}, std::move(handler)));
}

void client::move_conversation_to_folder(std::int64_t conversation, std::int64_t folder, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("move_conversation_to_folder", {{"conversation", conversation}, {"folder", folder}}, std::move(handler)));
}

void client::set_conversation_pinned(std::int64_t conversation, bool pinned, pin_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->request_bool(
        "set_conversation_pinned", {{"conversation", conversation}, {"pinned", pinned}}, "pinned", "Invalid pin result", std::move(handler)));
}

void client::open_direct_conversation(std::int64_t user, conversation_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->open_direct_conversation(user, std::move(handler)));
}

void client::create_group(std::string title, std::vector<std::int64_t> members, conversation_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->create_group(std::move(title), std::move(members), std::move(handler)));
}

void client::get_members(std::int64_t conversation, members_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_members(conversation, std::move(handler)));
}

void client::set_group_admin(std::int64_t conversation, std::int64_t user, bool admin, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_group_admin", {{"conversation", conversation}, {"user", user}, {"admin", admin}}, std::move(handler)));
}

void client::transfer_group_owner(std::int64_t conversation, std::int64_t user, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("transfer_group_owner", {{"conversation", conversation}, {"user", user}}, std::move(handler)));
}

void client::set_group_mute(std::int64_t conversation, bool muted, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_group_mute", {{"conversation", conversation}, {"muted", muted}}, std::move(handler)));
}
void client::set_group_member_mute(std::int64_t conversation, std::int64_t user, std::int64_t seconds, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_group_member_mute", {{"conversation", conversation}, {"user", user}, {"seconds", seconds}}, std::move(handler)));
}
void client::dissolve_group(std::int64_t conversation, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("dissolve_group", {{"conversation", conversation}}, std::move(handler)));
}
void client::remove_group_member(std::int64_t conversation, std::int64_t user, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("remove_group_member", {{"conversation", conversation}, {"user", user}}, std::move(handler)));
}

void client::set_conversation_view_handler(conversation_view_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->view_handler_ = std::move(handler);
}
void client::clear_history(std::int64_t conversation, std::int64_t through, mark_read_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->clear_history(conversation, through, std::move(handler)));
}
void client::hide_conversation(std::int64_t conversation, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("hide_conversation", {{"conversation", conversation}}, std::move(handler)));
}
void client::set_conversation_handler(conversation_changed_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->conversation_handler_ = std::move(handler);
}

void client::rename_group(std::int64_t conversation, std::string title, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("rename_group", {{"conversation", conversation}, {"title", std::move(title)}}, std::move(handler)));
}

void client::invite_group_members(std::int64_t conversation, std::vector<std::int64_t> members, group_action_handler handler)
{
    boost::json::array values;
    for (auto id : members)
    {
        values.push_back(id);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("invite_group_members", {{"conversation", conversation}, {"members", std::move(values)}}, std::move(handler)));
}

void client::set_group_description(std::int64_t conversation, std::string text, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_group_description", {{"conversation", conversation}, {"text", std::move(text)}}, std::move(handler)));
}

void client::set_group_announcement(std::int64_t conversation, std::string text, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_group_announcement", {{"conversation", conversation}, {"text", std::move(text)}}, std::move(handler)));
}

void client::get_group_invite(std::int64_t conversation, group_invite_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_invite("get_group_invite", conversation, std::move(handler)));
}

void client::create_group_invite(std::int64_t conversation, group_invite_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_invite("create_group_invite", conversation, std::move(handler)));
}

void client::revoke_group_invite(std::int64_t conversation, group_invite_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_invite("revoke_group_invite", conversation, std::move(handler)));
}

void client::join_group(std::string token, group_join_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->join_group(std::move(token), std::move(handler)));
}

void client::set_group_join_approval(std::int64_t conversation, bool required, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_group_join_approval", {{"conversation", conversation}, {"required", required}}, std::move(handler)));
}

void client::get_group_join_requests(std::int64_t conversation, std::optional<std::int64_t> before, group_join_requests_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_group_join_requests(conversation, before, std::move(handler)));
}

void client::respond_group_join_request(std::int64_t conversation, std::int64_t user, bool accept, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("respond_group_join_request", {{"conversation", conversation}, {"user", user}, {"accept", accept}}, std::move(handler)));
}

void client::leave_group(std::int64_t conversation, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("leave_group", {{"conversation", conversation}}, std::move(handler)));
}

void client::pin_group_message(std::int64_t conversation, std::int64_t message, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("pin_group_message", {{"conversation", conversation}, {"message", message}}, std::move(handler)));
}

void client::unpin_group_message(std::int64_t conversation, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("unpin_group_message", {{"conversation", conversation}}, std::move(handler)));
}

void client::set_contacts_changed_handler(connection_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->contacts_changed_handler_ = std::move(handler);
}
void client::set_friend_remark(std::int64_t user, std::string remark, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_friend_remark", {{"user", user}, {"remark", std::move(remark)}}, std::move(handler)));
}
void client::move_friend(std::int64_t user, std::string group, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("move_friend", {{"user", user}, {"group", std::move(group)}}, std::move(handler)));
}
void client::create_friend_group(std::string name, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("create_friend_group", {{"name", std::move(name)}}, std::move(handler)));
}
void client::rename_friend_group(std::string group, std::string name, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("rename_friend_group", {{"group", std::move(group)}, {"name", std::move(name)}}, std::move(handler)));
}
void client::delete_friend_group(std::string group, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("delete_friend_group", {{"group", std::move(group)}}, std::move(handler)));
}
void client::get_contacts(contacts_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<contact_book>("get_contacts", {}, "Invalid contacts result", std::move(handler)));
}

void client::get_presence(presences_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_presence(std::move(handler)));
}

void client::get_messages(
    std::int64_t conversation, std::optional<std::int64_t> before, messages_handler handler, std::optional<std::int64_t> after, bool capture_unread)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->get_messages(conversation, before, std::move(handler), after, {}, {}, {}, capture_unread));
}

void client::get_categorized_history(
    std::int64_t conversation, message_category category, std::string query, std::optional<std::int64_t> before, messages_handler handler)
{
    std::optional<std::string> keyword;
    if (!query.empty())
    {
        keyword.emplace(std::move(query));
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->get_messages(conversation, before, std::move(handler), {}, std::move(keyword), {}, category));
}

void client::get_messages_around(std::int64_t conversation, std::int64_t around, messages_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_messages(conversation, {}, std::move(handler), {}, {}, around));
}

void client::search_all_messages(message_search_filter const& filter, std::optional<std::int64_t> before, message_search_handler handler)
{
    boost::json::object params;
    if (!filter.query.empty())
    {
        params.emplace("query", filter.query);
    }
    if (filter.category != message_category::all)
    {
        params.emplace("category", category_name(filter.category));
    }
    auto add = [&params](char const* key, std::optional<std::int64_t> value)
    {
        if (value)
        {
            params.emplace(key, *value);
        }
    };
    add("conversation", filter.conversation);
    add("sender", filter.sender);
    add("start", filter.start);
    add("end", filter.end);
    if (filter.mentions_me)
    {
        params.emplace("mentions_me", true);
    }
    if (before)
    {
        params.emplace("before", *before);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<message_search_page>("search_all_messages", std::move(params), "Invalid global message search page", std::move(handler)));
}

void client::set_message_bookmark(std::int64_t message, bool bookmarked, bookmark_state_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->request_page<bookmark_state>(
        "set_message_bookmark", {{"message", message}, {"bookmarked", bookmarked}}, "Invalid bookmark state", std::move(handler)));
}

void client::get_message_bookmark(std::int64_t message, bookmark_state_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<bookmark_state>("get_message_bookmark", {{"message", message}}, "Invalid bookmark state", std::move(handler)));
}

void client::list_message_bookmarks(std::string query, std::optional<std::int64_t> before, bookmark_page_handler handler)
{
    boost::json::object params;
    if (!query.empty())
    {
        params.emplace("query", std::move(query));
    }
    if (before)
    {
        params.emplace("before", *before);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<bookmark_page>("list_message_bookmarks", std::move(params), "Invalid bookmark page", std::move(handler)));
}

void client::get_message_search_scopes(message_search_scopes_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<std::vector<message_search_scope>>("message_search_scopes", {}, "Invalid search scopes", std::move(handler)));
}

void client::search_messages(std::int64_t conversation, std::string query, std::optional<std::int64_t> before, messages_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_messages(conversation, before, std::move(handler), {}, std::move(query)));
}

std::int64_t client::new_client_id()
{
    static std::mutex mutex;
    static std::mt19937_64 engine{std::random_device{}()};
    std::lock_guard lock(mutex);
    return static_cast<std::int64_t>((engine() >> 1U) | 1ULL);
}

void client::send_message(std::int64_t conversation,
                          std::string text,
                          outbox_operation_handler queued,
                          std::optional<std::int64_t> reply_to,
                          std::optional<std::int64_t> client_id,
                          std::int64_t draft_revision)
{
    pending_text item{
        .client_id = client_id ? *client_id : new_client_id(),
        .conversation = conversation,
        .reply_to = reply_to.value_or(0),
        .created = 0,
        .text = std::move(text),
    };
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->queue_text(std::move(item), draft_revision, std::move(queued)));
}

void client::save_draft(std::int64_t user, compose_draft draft, outbox_operation_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->save_draft(user, std::move(draft), std::move(handler)));
}

void client::load_drafts(std::int64_t user, drafts_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->load_drafts(user, std::move(handler)));
}

void client::set_outbox_handler(outbox_handler handler)
{
    std::scoped_lock const lock(impl_->handler_mutex_);
    impl_->outbox_handler_ = std::move(handler);
}

void client::pending_texts(pending_texts_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->list_texts(std::move(handler)));
}

void client::retry_text(std::int64_t client_id, outbox_operation_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->change_text(client_id, false, std::move(handler)));
}

void client::discard_text(std::int64_t client_id, outbox_operation_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->change_text(client_id, true, std::move(handler)));
}

void client::send_attachment(std::int64_t conversation,
                             std::string path,
                             message_result_handler handler,
                             std::optional<std::int64_t> reply,
                             std::optional<std::int64_t> id,
                             progress_handler progress)
{
    auto name = std::filesystem::path(path).filename().string();
    pending_attachment t{id.value_or(new_client_id()), conversation, 0, reply.value_or(0), 0, std::move(path), std::move(name)};
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->send_file(
        std::move(t),
        [handler = std::move(handler)](impl::upload_result r)
        {
            if (!r)
            {
                handler(std::unexpected(r.error()));
            }
            else
            {
                handler(std::move(**r));
            }
        },
        std::move(progress)));
}
void client::interrupt_attachment(std::int64_t id, bool cancel, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->interrupt_attachment(id, cancel, std::move(handler)));
}
void client::upload_conversation_file(
    std::int64_t conversation, std::string path, std::int64_t id, group_action_handler handler, progress_handler progress)
{
    auto name = std::filesystem::path(path).filename().string();
    pending_attachment t{id, conversation, 0, 0, 0, std::move(path), std::move(name), true};
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->send_file(
        std::move(t),
        [handler = std::move(handler)](impl::upload_result r)
        {
            if (!r)
            {
                handler(std::unexpected(r.error()));
            }
            else
            {
                handler(true);
            }
        },
        std::move(progress)));
}
void client::list_conversation_files(std::int64_t conversation,
                                     std::string search,
                                     std::string category,
                                     std::int64_t before,
                                     std::string before_key,
                                     conversation_files_handler handler)
{
    boost::json::object params{{"conversation_id", conversation}, {"search", search}, {"category", category}};
    if (before)
    {
        params.emplace("cursor_created_at", before);
        params.emplace("cursor_object_key", before_key);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<conversation_file_page>("list_conversation_files", std::move(params), "Invalid group file page", std::move(handler)));
}
void client::add_conversation_file(std::int64_t conversation, std::string key, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("add_conversation_file", {{"conversation_id", conversation}, {"object_key", key}}, std::move(handler)));
}
void client::remove_conversation_file(std::int64_t conversation, std::string key, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("remove_conversation_file", {{"conversation_id", conversation}, {"object_key", key}}, std::move(handler)));
}
void client::download_conversation_file(
    std::int64_t conversation, std::string key, std::string path, attachment_handler handler, download_options options)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_attachment("get_conversation_file",
                                                                                    {{"conversation_id", conversation}, {"object_key", key}},
                                                                                    std::move(path),
                                                                                    max_attachment_size,
                                                                                    std::move(handler),
                                                                                    std::move(options)));
}
void client::pending_attachments(pending_attachments_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->restore_attachments(std::move(handler)));
}
void client::get_attachment(
    std::int64_t conversation, std::int64_t message, std::string path, attachment_handler handler, std::int64_t limit, download_options options)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_attachment(
        "get_attachment", {{"conversation", conversation}, {"message", message}}, std::move(path), limit, std::move(handler), std::move(options)));
}

void client::forward_message(std::int64_t conversation, std::int64_t source, std::int64_t client_id, message_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->request_message(
        "send_message", {{"conversation", conversation}, {"forward", source}, {"client_id", client_id}}, conversation, {}, std::move(handler)));
}
void client::delete_message(std::int64_t conversation, std::int64_t message, message_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_message("delete_message", {{"conversation", conversation}, {"message", message}}, conversation, message, std::move(handler)));
}

void client::set_message_reaction(std::int64_t conversation, std::int64_t message, std::string emoji, reaction_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->set_message_reaction(conversation, message, std::move(emoji), std::move(handler)));
}

void client::edit_message(std::int64_t conversation, std::int64_t message, std::string text, message_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_message("edit_message",
                               {{"conversation", conversation}, {"message", message}, {"text", std::move(text)}},
                               conversation,
                               message,
                               std::move(handler)));
}

void client::search_users(std::string query, users_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->search_users(std::move(query), std::move(handler)));
}

void client::get_user_profile(std::int64_t user, profile_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<user_profile>("get_user_profile", {{"user", user}}, "Invalid user profile", std::move(handler)));
}
void client::set_user_profile(std::string nickname, std::string signature, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_user_profile", {{"nickname", std::move(nickname)}, {"signature", std::move(signature)}}, std::move(handler)));
}
void client::set_profile_changed_handler(friendship_changed_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->profile_handler_ = std::move(handler);
}
void client::set_friendship_handler(friendship_changed_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->friendship_handler_ = std::move(handler);
}

void client::get_friend_requests(friend_requests_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_friend_requests(std::move(handler)));
}

void client::send_friend_request(std::int64_t user, friendship_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->change_friendship("send_friend_request", {{"user", user}}, std::move(handler)));
}

void client::respond_friend_request(std::int64_t user, bool accept, friendship_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->change_friendship("respond_friend_request", {{"user", user}, {"accept", accept}}, std::move(handler)));
}

void client::cancel_friend_request(std::int64_t user, friendship_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->change_friendship("cancel_friend_request", {{"user", user}}, std::move(handler)));
}

void client::remove_contact(std::int64_t user, remove_contact_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_bool("remove_contact", {{"user", user}}, "removed", "Invalid remove_contact result", std::move(handler)));
}

void client::mark_read(std::int64_t user, std::int64_t message, mark_read_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->mark_read(user, message, std::move(handler)));
}

void client::set_typing(std::int64_t conversation, bool typing, typing_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->request_bool(
        "set_typing", {{"conversation", conversation}, {"typing", typing}}, "realtime", "Invalid set_typing result", std::move(handler)));
}

}    // namespace chat
