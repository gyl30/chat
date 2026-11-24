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

#include <chat/client.hpp>
#include <chat/invite.hpp>
#include <chat/detail/base64.hpp>

#include "websocket.hpp"
#include "attachment_files.hpp"
#include "blocking_worker.hpp"
#include "rpc_payload.hpp"
#include <filesystem>

namespace chat
{
struct login_session_list
{
    std::vector<login_session> sessions;
};

namespace
{

namespace dom = simdjson::dom;

using detail::encode_request;
using detail::find_field;
using detail::make_params;
using detail::object_of;
using detail::read_payload;
using detail::request_params;

// Invoked synchronously within receive(): only owning values leave this callback.
using response_handler = std::function<void(std::expected<dom::element, error>)>;

error make_error(error_kind kind, std::string message, int code = 0)
{
    error value;
    value.kind = kind;
    value.code = code;
    value.message = std::move(message);
    return value;
}

std::optional<std::int64_t> integer_field(auto object, std::string_view key)
{
    auto value = find_field(object, key);
    return value ? read_payload<std::int64_t>(*value) : std::nullopt;
}

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

bool parse_deleted(dom::object object, bool& deleted)
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

bool parse_edited_at(dom::object object, std::optional<std::int64_t>& edited_at)
{
    auto value = find_field(object, "edited_at");
    if (!value || value->is_null())
    {
        return true;
    }
    edited_at = read_payload<std::int64_t>(*value);
    return edited_at && *edited_at > 0;
}

bool parse_quote(dom::object object, std::string_view key, std::optional<quoted_message>& reply)
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

bool parse_reactions(dom::object object, std::int64_t& revision, std::vector<reaction>& reactions)
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

bool parse_mentions(dom::object object, std::vector<mention>& result)
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
    item.unread = *unread;
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

    // Handlers may replace themselves or submit another request.
    template <class Handler, class... Args>
    void notify(Handler const& registered, Args&&... args)
    {
        Handler handler;
        {
            std::lock_guard lock(handler_mutex_);
            handler = registered;
        }
        if (handler && !suppress_callbacks_.load())
        {
            handler(std::forward<Args>(args)...);
        }
    }

    void report_error(error const& value) { notify(error_handler_, value); }

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

    void send_request(std::string method, request_params params, response_handler handler)
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
        auto payload = encode_request(method, params, id);
        if (!payload)
        {
            handler(std::unexpected(payload.error()));
            return;
        }
        pending_.emplace(id, std::move(handler));
        outgoing_.push_back(std::move(*payload));
        websocket_.interrupt_receive();
    }

    void receive(std::string const& payload)
    {
        dom::object object;
        if (response_parser_.parse(payload).get(object))
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC message"));
            return;
        }

        auto version = find_field(object, "jsonrpc");
        if (!version || !version->is_string() || version->get_string().value_unsafe() != "2.0")
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC message"));
            return;
        }

        auto id_value = find_field(object, "id");
        if (!id_value)
        {
            receive_notification(object);
            return;
        }

        receive_response(object, *id_value);
    }

    void receive_notification(dom::object object)
    {
        auto method = find_field(object, "method");
        auto params = find_field(object, "params");
        if (!method || !method->is_string() || !params || !params->is_object())
        {
            report_error(make_error(error_kind::protocol, "Invalid JSON-RPC notification"));
            return;
        }

        auto const method_name = method->get_string().value_unsafe();
        if (method_name == "login_session_revoked" || method_name == "login_sessions_changed")
        {
            auto const revoked = method_name == "login_session_revoked";
            if (revoked)
            {
                forget_login();
                closing_ = true;
                outgoing_.clear();
                websocket_.cancel();
                stop_file_transfers();
            }
            notify(revoked ? login_revoked_handler_ : login_sessions_changed_handler_);
            return;
        }

        if (method_name == "message" || method_name == "message_updated")
        {
            message notification;
            if (!parse_message(params->get_object().value_unsafe(), notification))
            {
                report_error(make_error(error_kind::protocol, "Invalid message notification"));
                return;
            }
            notify(method_name == "message_updated" ? message_updated_handler_ : message_handler_, std::move(notification));
            return;
        }

        if (method_name == "message_bookmark_changed")
        {
            auto value = find_field(*params, "message");
            if (!value || !value->is_int64() || value->get_int64().value_unsafe() <= 0)
            {
                report_error(make_error(error_kind::protocol, "Invalid bookmark notification"));
                return;
            }
            notify(bookmark_changed_handler_, value->get_int64().value_unsafe());
            return;
        }

        if (method_name == "reaction")
        {
            reaction_update notification;
            if (!parse_reaction_update(params->get_object().value_unsafe(), notification))
            {
                report_error(make_error(error_kind::protocol, "Invalid reaction notification"));
                return;
            }
            notify(reaction_handler_, std::move(notification));
            return;
        }

        if (method_name == "read")
        {
            auto conversation_value = find_field(*params, "conversation");
            auto user_value = find_field(*params, "user");
            auto message_value = find_field(*params, "message");
            if (!conversation_value || !user_value || !message_value)
            {
                report_error(make_error(error_kind::protocol, "Invalid read notification"));
                return;
            }
            auto conversation = read_payload<std::int64_t>(*conversation_value);
            auto user = read_payload<std::int64_t>(*user_value);
            auto read_message = read_payload<std::int64_t>(*message_value);
            if (!conversation || *conversation <= 0 || !user || *user <= 0 || !read_message || *read_message <= 0)
            {
                report_error(make_error(error_kind::protocol, "Invalid read notification"));
                return;
            }
            notify(read_handler_, *conversation, *user, *read_message);
            return;
        }

        if (method_name == "typing")
        {
            auto fields = params->get_object().value_unsafe();
            auto conversation = integer_field(fields, "conversation");
            auto user = integer_field(fields, "user");
            auto username = find_field(fields, "username");
            auto typing = find_field(fields, "typing");
            if (!conversation || *conversation <= 0 || !user || *user <= 0 || !username || !username->is_string() ||
                username->get_string().value_unsafe().empty() || !typing || !typing->is_bool())
            {
                report_error(make_error(error_kind::protocol, "Invalid typing notification"));
                return;
            }
            notify(typing_handler_,
                   typing_event{*conversation, *user, std::string(username->get_string().value_unsafe()), typing->get_bool().value_unsafe()});
            return;
        }

        if (method_name == "presence")
        {
            presence notification;
            if (!parse_presence(params->get_object().value_unsafe(), notification))
            {
                report_error(make_error(error_kind::protocol, "Invalid presence notification"));
                return;
            }
            notify(presence_handler_, std::move(notification));
            return;
        }

        if (method_name == "contacts_changed")
        {
            notify(contacts_changed_handler_);
            return;
        }
        if (method_name == "friendship" || method_name == "profile_changed")
        {
            auto user = integer_field(*params, "user");
            if (!user || *user <= 0)
            {
                report_error(make_error(error_kind::protocol, "Invalid friendship notification"));
                return;
            }
            notify(method_name == "friendship" ? friendship_handler_ : profile_handler_, *user);
            return;
        }

        if (method_name == "conversation_view")
        {
            auto view = read_payload<conversation_view_event>(*params);
            if (!view || (view->action == "clear" && view->through.value_or(-1) < 0) ||
                (view->action == "notifications" && (!view->level || *view->level < 0 || *view->level > 2 || !view->until)))
            {
                report_error(make_error(error_kind::protocol, "Invalid personal conversation state"));
                return;
            }
            notify(view_handler_, std::move(*view));
            return;
        }
        if (method_name == "conversation")
        {
            auto id = integer_field(*params, "conversation");
            auto removed = find_field(*params, "removed");
            if (!id || *id <= 0 || (removed && !removed->is_bool()))
            {
                report_error(make_error(error_kind::protocol, "Invalid conversation notification"));
                return;
            }
            auto const unavailable = removed && removed->get_bool().value_unsafe();
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
                            notify(outbox_handler_, std::move(item));
                        }
                    }
                }
            }
            notify(conversation_handler_, *id, unavailable);
            return;
        }

        if (method_name == "join_request")
        {
            auto object = params->get_object().value_unsafe();
            auto id = integer_field(object, "conversation");
            auto applicant = integer_field(object, "user");
            auto state = find_field(object, "state");
            if (!id || *id <= 0 || !applicant || *applicant <= 0 || !state || !state->is_string() ||
                (state->get_string().value_unsafe() != "pending" && state->get_string().value_unsafe() != "accepted" &&
                 state->get_string().value_unsafe() != "rejected"))
            {
                report_error(make_error(error_kind::protocol, "Invalid join request notification"));
                return;
            }
            notify(join_request_handler_,
                   group_join_request_event{*id,
                                            *applicant,
                                            state->get_string().value_unsafe() == "pending"    ? group_join_request_state::pending
                                            : state->get_string().value_unsafe() == "accepted" ? group_join_request_state::accepted
                                                                                               : group_join_request_state::rejected});
            return;
        }

        if (method_name == "avatar")
        {
            auto user = integer_field(*params, "user");
            avatar_state state;
            if (!user || *user <= 0 || !parse_avatar_state(params->get_object().value_unsafe(), state))
            {
                report_error(make_error(error_kind::protocol, "Invalid avatar notification"));
                return;
            }
            notify(avatar_handler_, *user, state);
            return;
        }
        report_error(make_error(error_kind::protocol, "Invalid JSON-RPC notification"));
    }

    void receive_response(dom::object object, dom::element const& id_value)
    {
        auto id = read_payload<std::int64_t>(id_value);
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

        auto result = find_field(object, "result");
        auto error_value = find_field(object, "error");
        if (result.has_value() == error_value.has_value())
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

        auto error_object = error_value->get_object().value_unsafe();
        auto code = find_field(error_object, "code");
        auto error_message = find_field(error_object, "message");
        if (!code || !code->is_int64() || !error_message || !error_message->is_string())
        {
            handler(std::unexpected(make_error(error_kind::protocol, "Invalid JSON-RPC error")));
            return;
        }

        handler(std::unexpected(make_error(
            error_kind::rpc, std::string(error_message->get_string().value_unsafe()), static_cast<int>(code->get_int64().value_unsafe()))));
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
        notify(connected_handler_);

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
        notify(disconnected_handler_);
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
        request_params params;
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
                    params.add("device_secret", std::move(*secret));
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
            params.add("token", login_token_);
        }
        else
        {
            params.add("username", username);
            params.add("password", std::move(*password));
            if (!device_name.empty())
            {
                params.add("device_name", std::move(device_name));
            }
            if (!client_type.empty())
            {
                params.add("client_type", std::move(client_type));
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
                         auto object = object_of(*response);
                         auto authenticated = object ? find_field(*object, "authenticated") : std::nullopt;
                         auto user_value = object ? find_field(*object, "user") : std::nullopt;
                         auto user = user_value ? read_payload<std::int64_t>(*user_value) : std::nullopt;
                         if (!authenticated || !authenticated->is_bool() || !user)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid authenticate result")));
                             return;
                         }

                         authentication_result value{authenticated->get_bool().value_unsafe(), *user, {}};
                         if (!parse_avatar_state(*object, value.avatar))
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid authenticate avatar")));
                             return;
                         }
                         if (value.authenticated)
                         {
                             auto token = find_field(*object, "token");
                             if (!resuming)
                             {
                                 if (!token || !token->is_string() || token->get_string().value_unsafe().size() != 64)
                                 {
                                     handler(std::unexpected(make_error(error_kind::protocol, "Missing login token")));
                                     return;
                                 }
                                 login_token_ = token->get_string().value_unsafe();
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
        request_params params;
        params.add("username", std::move(username));
        params.add("password", std::move(password));

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

                         auto user_value = find_field(*response, "user");
                         if (!user_value)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid register result")));
                             return;
                         }

                         auto user = read_payload<std::int64_t>(*user_value);
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
        request_params params;
        if (before)
        {
            params.add("before", *before);
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
                         auto list = find_field(*response, "conversations");
                         auto next = find_field(*response, "next");
                         if (!list || !list->is_array() || !next)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversations")));
                             return;
                         }
                         conversations_result result;
                         auto folders = find_field(*response, "folders");
                         if (!folders)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Missing conversation folders")));
                             return;
                         }
                         auto values = read_payload<std::vector<conversation_folder>>(*folders);
                         if (!values)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid conversation folders")));
                             return;
                         }
                         result.folders = std::move(*values);
                         for (auto value : list->get_array().value_unsafe())
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
                             auto a = integer_field(*next, "activity");
                             auto i = integer_field(*next, "id");
                             auto pinned = find_field(*next, "pinned");
                             if (!a || *a <= 0 || !i || *i <= 0 || !pinned || !pinned->is_bool())
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid cursor")));
                                 return;
                             }
                             result.next = conversation_cursor{*a, *i, pinned->get_bool().value_unsafe()};
                         }
                         handler(std::move(result));
                     });
        co_return;
    }

    boost::capy::task<> open_direct_conversation(std::int64_t user, conversation_handler handler)
    {
        send_request("open_direct_conversation",
                     make_params("user", user),
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto id = integer_field(*response, "conversation");
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
        send_request("create_group",
                     make_params("title", std::move(title), "members", std::move(members)),
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto id = integer_field(*response, "conversation");
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
                     make_params("conversation", conversation),
                     [handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto values = find_field(*response, "members");
                         if (!values || !values->is_array())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid members result")));
                             return;
                         }
                         std::vector<conversation_member> members;
                         for (auto value : values->get_array().value_unsafe())
                         {
                             user person;
                             auto role = find_field(value, "role");
                             auto speech_until = find_field(value, "speech_until");
                             if (!speech_until || !speech_until->is_int64() || !role || !role->is_string() ||
                                 !parse_user(value.get_object().value_unsafe(), person) ||
                                 (role->get_string().value_unsafe() != "owner" && role->get_string().value_unsafe() != "admin" &&
                                  role->get_string().value_unsafe() != "member"))
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid member")));
                                 return;
                             }
                             members.push_back({person.id,
                                                std::move(person.username),
                                                role->get_string().value_unsafe() == "owner"   ? member_role::owner
                                                : role->get_string().value_unsafe() == "admin" ? member_role::admin
                                                                                               : member_role::member,
                                                person.avatar,
                                                speech_until->get_int64().value_unsafe(),
                                                std::move(person.nickname),
                                                string_field(value.get_object().value_unsafe(), "remark")});
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
            make_params("conversation", conversation),
            [creating, revoking, handler = std::move(handler)](auto response) mutable
            {
                if (!response)
                {
                    handler(std::unexpected(std::move(response.error())));
                    return;
                }
                auto token = find_field(*response, "token");
                if (!token || (!token->is_null() && !token->is_string()) || (creating && token->is_null()) || (revoking && !token->is_null()))
                {
                    handler(std::unexpected(make_error(error_kind::protocol, "Invalid group invite")));
                    return;
                }
                std::optional<std::string> value;
                if (token->is_string())
                {
                    value = std::string(token->get_string().value_unsafe());
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
                     make_params("token", std::move(token)),
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
                         auto object = response->get_object().value_unsafe();
                         auto id = integer_field(object, "conversation");
                         auto title = find_field(object, "title");
                         auto count = find_field(object, "member_count");
                         auto state = find_field(object, "state");
                         auto members = count ? read_payload<std::uint64_t>(*count) : std::nullopt;
                         if (!id || *id <= 0 || !title || !title->is_string() || title->get_string().value_unsafe().empty() ||
                             title->get_string().value_unsafe().size() > 256 ||
                             title->get_string().value_unsafe().find('\0') != std::string_view::npos || !members || *members == 0 || !state ||
                             !state->is_string() ||
                             (state->get_string().value_unsafe() != "joined" && state->get_string().value_unsafe() != "member" &&
                              state->get_string().value_unsafe() != "pending"))
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid group join result")));
                             return;
                         }
                         handler(group_join_result{*id,
                                                   std::string(title->get_string().value_unsafe()),
                                                   *members,
                                                   state->get_string().value_unsafe() == "joined"   ? group_join_state::joined
                                                   : state->get_string().value_unsafe() == "member" ? group_join_state::member
                                                                                                    : group_join_state::pending});
                     });
        co_return;
    }

    boost::capy::task<> get_group_join_requests(std::int64_t conversation, std::optional<std::int64_t> before, group_join_requests_handler handler)
    {
        request_params params = make_params("conversation", conversation);
        if (before)
        {
            params.add("before", *before);
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
                         auto requests = find_field(*response, "requests");
                         auto next = find_field(*response, "next");
                         if (!requests || !requests->is_array() || requests->get_array().value_unsafe().size() > 50 || !next)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid join requests")));
                             return;
                         }
                         group_join_requests_result result;
                         for (auto value : requests->get_array().value_unsafe())
                         {
                             group_join_request item;
                             auto time = integer_field(value, "created_at");
                             if (!value.is_object() || !parse_user(value.get_object().value_unsafe(), item.applicant) || !time || *time <= 0 ||
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
                             auto cursor = read_payload<std::int64_t>(*next);
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
                                     request_params params,
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
                         auto value = find_field(*response, field);
                         if (!value || !value->is_bool())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, invalid_result)));
                             return;
                         }
                         handler(value->get_bool().value_unsafe());
                     });
        co_return;
    }

    boost::capy::task<> group_action(std::string method, request_params params, group_action_handler handler)
    {
        return request_bool(std::move(method), std::move(params), "changed", "Invalid group action result", std::move(handler));
    }

    template <class Page, class Handler>
    boost::capy::task<> request_page(std::string method, request_params params, char const* invalid_result, Handler handler)
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
                         auto page = read_payload<Page>(*r);
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

                         auto users_value = find_field(*response, "users");
                         if (!users_value || !users_value->is_array())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid get_presence result")));
                             return;
                         }

                         std::vector<presence> users;
                         users.reserve(users_value->get_array().value_unsafe().size());
                         for (auto value : users_value->get_array().value_unsafe())
                         {
                             if (!value.is_object())
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid presence")));
                                 return;
                             }

                             presence item;
                             if (!parse_presence(value.get_object().value_unsafe(), item))
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
        request_params params;
        params.add("conversation", conversation);
        if (before)
        {
            params.add("before", *before);
        }

        if (after)
        {
            params.add("after", *after);
        }
        if (around)
        {
            params.add("around", *around);
        }
        if (query)
        {
            params.add("query", std::move(*query));
        }
        if (category)
        {
            params.add("category", category_name(*category));
        }
        if (capture_unread)
        {
            params.add("capture_unread", true);
        }
        send_request(query && !category ? "search_messages" : "get_messages",
                     std::move(params),
                     [conversation, handler = std::move(handler)](auto response) mutable
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         handler(parse_message_page(*response, conversation));
                     });

        co_return;
    }

    static request_params text_params(pending_text const& item)
    {
        request_params params = make_params("conversation", item.conversation, "text", item.text, "client_id", item.client_id);
        if (item.reply_to > 0)
        {
            params.add("reply_to", item.reply_to);
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
        notify(outbox_handler_, item);
        auto params = text_params(item);
        send_request("send_message",
                     std::move(params),
                     [this, user = account_, item = std::move(item)](auto response) mutable
                     {
                         sending_texts_.erase(item.conversation);
                         message value;
                         if (response && response->is_object() && parse_receipt(response->get_object().value_unsafe(), value) && value.from == user &&
                             value.conversation == item.conversation && value.client_id == item.client_id && !value.attachment)
                         {
                             if (!journal_.exec("DELETE FROM text_outbox WHERE user=? AND client=?", user, item.client_id))
                             {
                                 notify(message_handler_, std::move(value));
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
                                             notify(outbox_handler_, std::move(row));
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
                         notify(outbox_handler_, std::move(item));
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
        auto payload = encode_request("send_message", text_params(item), std::numeric_limits<std::int64_t>::max());
        if (!payload)
        {
            handler(std::unexpected(payload.error()));
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
        notify(outbox_handler_, std::move(item));
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
            send_request("pause_attachment", make_params("upload", job->task.client_id), [](auto) {});
        }
        job->handler(std::move(result));
    }
    void accept_file_message(job_ptr const& job, dom::element const& response)
    {
        message m;
        if (!response.is_object() || !parse_receipt(response.get_object().value_unsafe(), m) || m.from != account_ ||
            m.client_id != job->task.client_id || m.conversation != job->task.conversation ||
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
                     make_params("upload", job->task.client_id, "number", number, "etag", *result),
                     [this, job](auto r) { on_grant(job, std::move(r)); });
    }
    void attach_group_file(job_ptr const& job, dom::element const& response)
    {
        auto obj = object_of(response);
        auto key = obj ? find_field(*obj, "object_key") : std::nullopt;
        if (!key || !key->is_string())
        {
            end_upload(job, std::unexpected(make_error(error_kind::protocol, "Invalid completed file")));
            return;
        }
        send_request("add_conversation_file",
                     make_params("conversation_id", job->task.conversation, "object_key", key->get_string().value_unsafe()),
                     [this, job](auto r)
                     {
                         if (!r)
                         {
                             end_upload(job, std::unexpected(r.error()));
                             return;
                         }
                         auto fields = object_of(*r);
                         auto changed = fields ? find_field(*fields, "changed") : std::nullopt;
                         if (!changed || !changed->is_bool())
                         {
                             end_upload(job, std::unexpected(make_error(error_kind::protocol, "Invalid group file result")));
                             return;
                         }
                         end_upload(job, std::optional<message>{});
                     });
    }
    void on_grant(job_ptr const& job, std::expected<dom::element, error> response)
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
        auto obj = object_of(*response);
        if (obj)
        {
            if (job->task.group_file && find_field(*obj, "object_key"))
            {
                attach_group_file(job, *response);
                return;
            }
            if (auto msg = find_field(*obj, "message"); !job->task.group_file && msg)
            {
                accept_file_message(job, *msg);
                return;
            }
        }
        auto integer = [&](char const* key)
        {
            auto v = obj ? find_field(*obj, key) : std::nullopt;
            return v ? read_payload<std::int64_t>(*v).value_or(-1) : -1;
        };
        auto n = integer("number"), offset = integer("offset"), length = integer("length"), part_size = integer("part_size"),
             count = integer("part_count");
        auto url = obj ? find_field(*obj, "url") : std::nullopt;
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
                transfer_part(job, std::string(url->get_string().value_unsafe()), static_cast<int>(n), offset, length));
            return;
        }
        request_params params = make_params("upload", job->task.client_id);
        if (job->task.reply_to)
        {
            params.add("reply_to", job->task.reply_to);
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
            send_request("resume_attachment", make_params("upload", t.client_id), [this, job](auto r) { on_grant(job, std::move(r)); });
            co_return;
        }
        send_request("begin_attachment",
                     make_params("conversation",
                                 t.conversation,
                                 "client_id",
                                 t.client_id,
                                 "size",
                                 job->file.size,
                                 "filename",
                                 t.filename,
                                 "media_type",
                                 job->file.media,
                                 "checksum",
                                 job->file.checksum),
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
                     make_params("upload", id),
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
        std::string method, request_params params, std::string target, std::int64_t limit, attachment_handler handler, download_options options)
    {
        send_request(
            std::move(method),
            std::move(params),
            [this, target = std::move(target), limit, handler = std::move(handler), options = std::move(options), stop = transfer_stop_.get_token()](
                auto r) mutable
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
                auto obj = object_of(*r);
                auto v = obj ? find_field(*obj, "size") : std::nullopt;
                auto size = v ? read_payload<std::int64_t>(*v) : std::nullopt;
                auto url = obj ? find_field(*obj, "url") : std::nullopt;
                auto hash = obj ? find_field(*obj, "checksum") : std::nullopt;
                if (!size || *size < 0 || *size > limit || !url || !url->is_string() || !hash || !hash->is_string())
                {
                    handler(std::unexpected(make_error(error_kind::file_transfer, "附件过大或下载信息无效")));
                    return;
                }
                boost::capy::run_async(io_context_.get_executor())(save_download(std::string(url->get_string().value_unsafe()),
                                                                                 std::string(hash->get_string().value_unsafe()),
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
                     make_params("conversation", conversation, "message", id, "emoji", std::move(emoji)),
                     [conversation, id, handler = std::move(handler)](auto result) mutable
                     {
                         if (!result)
                         {
                             handler(std::unexpected(std::move(result.error())));
                             return;
                         }
                         reaction_update value;
                         if (!result->is_object() || !parse_reaction_update(result->get_object().value_unsafe(), value) ||
                             value.conversation != conversation || value.message != id)
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid reaction result")));
                             return;
                         }
                         handler(std::move(value));
                     });
        co_return;
    }

    boost::capy::task<> request_message(
        std::string method, request_params params, std::int64_t conversation, std::optional<std::int64_t> expected_id, message_result_handler handler)
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
                             !(expected_id ? parse_message(result->get_object().value_unsafe(), value)
                                           : parse_receipt(result->get_object().value_unsafe(), value)) ||
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
        request_params params;
        params.add("query", std::move(query));

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

                         auto users_value = find_field(*response, "users");
                         if (!users_value || !users_value->is_array())
                         {
                             handler(std::unexpected(make_error(error_kind::protocol, "Invalid search_users result")));
                             return;
                         }

                         std::vector<user> users;
                         users.reserve(users_value->get_array().value_unsafe().size());
                         for (auto value : users_value->get_array().value_unsafe())
                         {
                             if (!value.is_object())
                             {
                                 handler(std::unexpected(make_error(error_kind::protocol, "Invalid user")));
                                 return;
                             }

                             user item;
                             if (!parse_user(value.get_object().value_unsafe(), item))
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

    boost::capy::task<> change_friendship(std::string method, request_params params, friendship_handler handler)
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
                         auto user = find_field(*response, "user");
                         auto state = parse_friendship_state(find_field(*response, "state"));
                         friendship_result result;
                         if (!user || !user->is_object() || !state || !parse_user(user->get_object().value_unsafe(), result.user))
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
                             auto list = find_field(*response, name);
                             if (!list || !list->is_array())
                             {
                                 return false;
                             }
                             std::unordered_set<std::int64_t> seen;
                             for (auto entry : list->get_array().value_unsafe())
                             {
                                 auto user = find_field(entry, "user");
                                 auto created = integer_field(entry, "created_at");
                                 friend_request request;
                                 if (!user || !user->is_object() || !created || *created < 0 ||
                                     !parse_user(user->get_object().value_unsafe(), request.user) || !seen.insert(request.user.id).second)
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
                     make_params("conversation", conversation, "through", through),
                     [through, handler = std::move(handler)](auto r)
                     {
                         if (!r)
                         {
                             handler(std::unexpected(r.error()));
                             return;
                         }
                         auto actual = integer_field(*r, "through");
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
                     make_params("conversation", conversation, "message", message_id),
                     [handler = std::move(handler)](auto response)
                     {
                         if (!response)
                         {
                             handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto read_message = integer_field(*response, "message");
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
            send_request("cancel_avatar_upload", make_params("upload", job->upload), [](auto) {});
        }
        job->handler(std::unexpected(std::move(value)));
    }

    void upload_avatar_next(std::shared_ptr<avatar_upload_job> job)
    {
        if (job->offset == job->data.size())
        {
            send_request("finish_avatar_upload",
                         make_params("upload", job->upload),
                         [this, job](auto response)
                         {
                             avatar_state state;
                             if (!response)
                             {
                                 fail_avatar_upload(job, std::move(response.error()));
                             }
                             else if (!response->is_object() || !parse_avatar_state(response->get_object().value_unsafe(), state) || !state.present)
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
        send_request(
            "upload_avatar_chunk",
            make_params(
                "upload", job->upload, "offset", job->offset, "data", detail::encode_base64(std::string_view(job->data).substr(job->offset, count))),
            [this, job, count](auto response)
            {
                if (!response)
                {
                    fail_avatar_upload(job, std::move(response.error()));
                    return;
                }
                auto offset = integer_field(*response, "offset");
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
        request_params params = make_params("size", job->data.size());
        if (group)
        {
            params.add("conversation", group);
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
                         auto upload = integer_field(*response, "upload");
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
        send_request("get_avatar",
                     make_params(job->value.conversation ? "conversation" : "user",
                                 job->value.conversation ? job->value.conversation : job->value.user,
                                 "revision",
                                 job->value.state.revision,
                                 "offset",
                                 job->value.data.size()),
                     [this, job](auto response)
                     {
                         if (!response)
                         {
                             job->handler(std::unexpected(std::move(response.error())));
                             return;
                         }
                         auto fields = object_of(*response);
                         avatar_state state;
                         auto user_field = fields ? find_field(*fields, job->value.conversation ? "conversation" : "user") : std::nullopt;
                         auto size_field = fields ? find_field(*fields, "size") : std::nullopt;
                         auto offset_field = fields ? find_field(*fields, "offset") : std::nullopt;
                         auto data_field = fields ? find_field(*fields, "data") : std::nullopt;
                         auto type_field = fields ? find_field(*fields, "media_type") : std::nullopt;
                         auto more_field = fields ? find_field(*fields, "has_more") : std::nullopt;
                         auto user = user_field ? read_payload<std::int64_t>(*user_field) : std::nullopt;
                         auto size = size_field ? read_payload<std::int64_t>(*size_field) : std::nullopt;
                         auto offset = offset_field ? read_payload<std::int64_t>(*offset_field) : std::nullopt;
                         if (!fields || !parse_avatar_state(*fields, state) || !user ||
                             *user != (job->value.conversation ? job->value.conversation : job->value.user) || !size || *size < 0 ||
                             *size > static_cast<std::int64_t>(max_avatar_size) || !offset || *offset < 0 || !data_field ||
                             !data_field->is_string() || !type_field || !type_field->is_string() || !more_field || !more_field->is_bool() ||
                             data_field->get_string().value_unsafe().size() > 4 * ((avatar_chunk_size + 2) / 3))
                         {
                             job->handler(std::unexpected(make_error(error_kind::protocol, "Invalid avatar chunk")));
                             return;
                         }
                         if (state.revision != job->value.state.revision || !state.present)
                         {
                             if (*size != 0 || *offset != 0 || !data_field->get_string().value_unsafe().empty() ||
                                 !type_field->get_string().value_unsafe().empty() || more_field->get_bool().value_unsafe())
                             {
                                 job->handler(std::unexpected(make_error(error_kind::protocol, "Invalid stale avatar metadata")));
                                 return;
                             }
                             job->handler(avatar{job->value.user, job->value.conversation, state, {}, {}});
                             return;
                         }
                         auto const type = std::string_view(type_field->get_string().value_unsafe());
                         auto bytes = detail::decode_base64(std::string_view(data_field->get_string().value_unsafe()));
                         if (*size == 0 || *offset != static_cast<std::int64_t>(job->value.data.size()) || *offset > *size ||
                             (job->size && *job->size != *size) || (type != "image/png" && type != "image/jpeg") ||
                             (!job->value.media_type.empty() && job->value.media_type != type) || !bytes ||
                             bytes->size() != std::min(avatar_chunk_size, static_cast<std::size_t>(*size - *offset)) ||
                             more_field->get_bool().value_unsafe() != (*offset + static_cast<std::int64_t>(bytes->size()) < *size))
                         {
                             job->handler(std::unexpected(make_error(error_kind::protocol, "Invalid avatar chunk data")));
                             return;
                         }
                         job->value.state = state;
                         job->value.media_type = type;
                         job->size = size;
                         job->value.data.append(*bytes);
                         if (more_field->get_bool().value_unsafe())
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
        request_params params;
        if (group)
        {
            params.add("conversation", group);
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
                         if (!response->is_object() || !parse_avatar_state(response->get_object().value_unsafe(), state) || state.present)
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
    // The receive loop is serialized on the SDK executor. Its DOM views never survive a frame.
    dom::parser response_parser_;
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
        impl_->group_action("revoke_login_session", make_params("session", session), std::move(handler)));
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
        "change_password", make_params("current_password", std::move(current), "new_password", std::move(replacement)), std::move(handler)));
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
        "set_notification_level", make_params("conversation", conversation, "level", static_cast<int>(level), "until", until), std::move(handler)));
}
void client::mark_conversations_read(std::optional<std::int64_t> conversation, std::optional<std::int64_t> folder, group_action_handler handler)
{
    request_params params;
    if (conversation)
    {
        params.add("conversation", *conversation);
    }
    if (folder)
    {
        params.add("folder", *folder);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action("mark_conversations_read", std::move(params), std::move(handler)));
}

void client::set_unread_mark(std::int64_t conversation, bool marked, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_unread_mark", make_params("conversation", conversation, "marked", marked), std::move(handler)));
}
void client::create_conversation_folder(std::string name, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("create_conversation_folder", make_params("name", std::move(name)), std::move(handler)));
}

void client::rename_conversation_folder(std::int64_t folder, std::string name, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("rename_conversation_folder", make_params("folder", folder, "name", std::move(name)), std::move(handler)));
}

void client::delete_conversation_folder(std::int64_t folder, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("delete_conversation_folder", make_params("folder", folder), std::move(handler)));
}

void client::reorder_conversation_folder(std::int64_t folder, bool up, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("reorder_conversation_folder", make_params("folder", folder, "up", up), std::move(handler)));
}

void client::move_conversation_to_folder(std::int64_t conversation, std::int64_t folder, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("move_conversation_to_folder", make_params("conversation", conversation, "folder", folder), std::move(handler)));
}

void client::set_conversation_pinned(std::int64_t conversation, bool pinned, pin_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->request_bool(
        "set_conversation_pinned", make_params("conversation", conversation, "pinned", pinned), "pinned", "Invalid pin result", std::move(handler)));
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
        impl_->group_action("set_group_admin", make_params("conversation", conversation, "user", user, "admin", admin), std::move(handler)));
}

void client::transfer_group_owner(std::int64_t conversation, std::int64_t user, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("transfer_group_owner", make_params("conversation", conversation, "user", user), std::move(handler)));
}

void client::set_group_mute(std::int64_t conversation, bool muted, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_group_mute", make_params("conversation", conversation, "muted", muted), std::move(handler)));
}
void client::set_group_member_mute(std::int64_t conversation, std::int64_t user, std::int64_t seconds, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action(
        "set_group_member_mute", make_params("conversation", conversation, "user", user, "seconds", seconds), std::move(handler)));
}
void client::dissolve_group(std::int64_t conversation, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("dissolve_group", make_params("conversation", conversation), std::move(handler)));
}
void client::remove_group_member(std::int64_t conversation, std::int64_t user, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("remove_group_member", make_params("conversation", conversation, "user", user), std::move(handler)));
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
        impl_->group_action("hide_conversation", make_params("conversation", conversation), std::move(handler)));
}
void client::set_conversation_handler(conversation_changed_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->conversation_handler_ = std::move(handler);
}

void client::rename_group(std::int64_t conversation, std::string title, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("rename_group", make_params("conversation", conversation, "title", std::move(title)), std::move(handler)));
}

void client::invite_group_members(std::int64_t conversation, std::vector<std::int64_t> members, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("invite_group_members", make_params("conversation", conversation, "members", std::move(members)), std::move(handler)));
}

void client::set_group_description(std::int64_t conversation, std::string text, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_group_description", make_params("conversation", conversation, "text", std::move(text)), std::move(handler)));
}

void client::set_group_announcement(std::int64_t conversation, std::string text, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_group_announcement", make_params("conversation", conversation, "text", std::move(text)), std::move(handler)));
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
        impl_->group_action("set_group_join_approval", make_params("conversation", conversation, "required", required), std::move(handler)));
}

void client::get_group_join_requests(std::int64_t conversation, std::optional<std::int64_t> before, group_join_requests_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_group_join_requests(conversation, before, std::move(handler)));
}

void client::respond_group_join_request(std::int64_t conversation, std::int64_t user, bool accept, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->group_action(
        "respond_group_join_request", make_params("conversation", conversation, "user", user, "accept", accept), std::move(handler)));
}

void client::leave_group(std::int64_t conversation, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("leave_group", make_params("conversation", conversation), std::move(handler)));
}

void client::pin_group_message(std::int64_t conversation, std::int64_t message, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("pin_group_message", make_params("conversation", conversation, "message", message), std::move(handler)));
}

void client::unpin_group_message(std::int64_t conversation, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("unpin_group_message", make_params("conversation", conversation), std::move(handler)));
}

void client::set_contacts_changed_handler(connection_handler handler)
{
    std::lock_guard lock(impl_->handler_mutex_);
    impl_->contacts_changed_handler_ = std::move(handler);
}
void client::set_friend_remark(std::int64_t user, std::string remark, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_friend_remark", make_params("user", user, "remark", std::move(remark)), std::move(handler)));
}
void client::move_friend(std::int64_t user, std::string group, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("move_friend", make_params("user", user, "group", std::move(group)), std::move(handler)));
}
void client::create_friend_group(std::string name, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("create_friend_group", make_params("name", std::move(name)), std::move(handler)));
}
void client::rename_friend_group(std::string group, std::string name, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("rename_friend_group", make_params("group", std::move(group), "name", std::move(name)), std::move(handler)));
}
void client::delete_friend_group(std::string group, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("delete_friend_group", make_params("group", std::move(group)), std::move(handler)));
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
    request_params params;
    if (!filter.query.empty())
    {
        params.add("query", filter.query);
    }
    if (filter.category != message_category::all)
    {
        params.add("category", category_name(filter.category));
    }
    auto add = [&params](char const* key, std::optional<std::int64_t> value)
    {
        if (value)
        {
            params.add(key, *value);
        }
    };
    add("conversation", filter.conversation);
    add("sender", filter.sender);
    add("start", filter.start);
    add("end", filter.end);
    if (filter.mentions_me)
    {
        params.add("mentions_me", true);
    }
    if (before)
    {
        params.add("before", *before);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<message_search_page>("search_all_messages", std::move(params), "Invalid global message search page", std::move(handler)));
}

void client::set_message_bookmark(std::int64_t message, bool bookmarked, bookmark_state_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->request_page<bookmark_state>(
        "set_message_bookmark", make_params("message", message, "bookmarked", bookmarked), "Invalid bookmark state", std::move(handler)));
}

void client::get_message_bookmark(std::int64_t message, bookmark_state_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<bookmark_state>("get_message_bookmark", make_params("message", message), "Invalid bookmark state", std::move(handler)));
}

void client::list_message_bookmarks(std::string query, std::optional<std::int64_t> before, bookmark_page_handler handler)
{
    request_params params;
    if (!query.empty())
    {
        params.add("query", std::move(query));
    }
    if (before)
    {
        params.add("before", *before);
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
    request_params params = make_params("conversation_id", conversation, "search", search, "category", category);
    if (before)
    {
        params.add("cursor_created_at", before);
        params.add("cursor_object_key", before_key);
    }
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_page<conversation_file_page>("list_conversation_files", std::move(params), "Invalid group file page", std::move(handler)));
}
void client::add_conversation_file(std::int64_t conversation, std::string key, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("add_conversation_file", make_params("conversation_id", conversation, "object_key", key), std::move(handler)));
}
void client::remove_conversation_file(std::int64_t conversation, std::string key, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("remove_conversation_file", make_params("conversation_id", conversation, "object_key", key), std::move(handler)));
}
void client::download_conversation_file(
    std::int64_t conversation, std::string key, std::string path, attachment_handler handler, download_options options)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_attachment("get_conversation_file",
                                                                                    make_params("conversation_id", conversation, "object_key", key),
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
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->get_attachment("get_attachment",
                                                                                    make_params("conversation", conversation, "message", message),
                                                                                    std::move(path),
                                                                                    limit,
                                                                                    std::move(handler),
                                                                                    std::move(options)));
}

void client::forward_message(std::int64_t conversation, std::int64_t source, std::int64_t client_id, message_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->request_message(
        "send_message", make_params("conversation", conversation, "forward", source, "client_id", client_id), conversation, {}, std::move(handler)));
}
void client::delete_message(std::int64_t conversation, std::int64_t message, message_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->request_message(
        "delete_message", make_params("conversation", conversation, "message", message), conversation, message, std::move(handler)));
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
                               make_params("conversation", conversation, "message", message, "text", std::move(text)),
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
        impl_->request_page<user_profile>("get_user_profile", make_params("user", user), "Invalid user profile", std::move(handler)));
}
void client::set_user_profile(std::string nickname, std::string signature, group_action_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->group_action("set_user_profile", make_params("nickname", std::move(nickname), "signature", std::move(signature)), std::move(handler)));
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
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->change_friendship("send_friend_request", make_params("user", user), std::move(handler)));
}

void client::respond_friend_request(std::int64_t user, bool accept, friendship_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->change_friendship("respond_friend_request", make_params("user", user, "accept", accept), std::move(handler)));
}

void client::cancel_friend_request(std::int64_t user, friendship_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->change_friendship("cancel_friend_request", make_params("user", user), std::move(handler)));
}

void client::remove_contact(std::int64_t user, remove_contact_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(
        impl_->request_bool("remove_contact", make_params("user", user), "removed", "Invalid remove_contact result", std::move(handler)));
}

void client::mark_read(std::int64_t user, std::int64_t message, mark_read_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->mark_read(user, message, std::move(handler)));
}

void client::set_typing(std::int64_t conversation, bool typing, typing_result_handler handler)
{
    boost::capy::run_async(impl_->io_context_.get_executor())(impl_->request_bool(
        "set_typing", make_params("conversation", conversation, "typing", typing), "realtime", "Invalid set_typing result", std::move(handler)));
}

}    // namespace chat
