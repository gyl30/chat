#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "message_payload.hpp"
#include "database.hpp"
#include "message_history.hpp"

namespace
{

struct[[= simdjson::deny_unknown_fields]] get_messages_params
{
    std::int64_t conversation = 0;
    std::optional<std::int64_t> before;
    std::optional<std::int64_t> after;
    std::optional<std::int64_t> around;
    std::optional<std::string> query;
    std::optional<std::string> category;
    std::optional<bool> capture_unread;
};

struct[[= simdjson::deny_unknown_fields]] search_all_params
{
    std::optional<std::string> query;
    std::optional<std::int64_t> before;
    std::optional<std::int64_t> conversation;
    std::optional<std::string> category;
    std::optional<std::int64_t> sender;
    std::optional<std::int64_t> start;
    std::optional<std::int64_t> end;
    std::optional<bool> mentions_me;
};

struct[[= simdjson::deny_unknown_fields]] bookmark_params
{
    std::int64_t message = 0;
    std::optional<bool> bookmarked;
};

struct[[= simdjson::deny_unknown_fields]] bookmark_list_params
{
    std::optional<std::string> query;
    std::optional<std::int64_t> before;
};

bool valid_search(std::string_view query) { return !query.empty() && query.size() <= 1024 && !query.contains('\0'); }

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_messages(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    if (request.method == "message_search_scopes")
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        auto scopes = co_await database_.search([user = *user_id_](sqlite_database& db) { return read_message_search_scopes(db, user); });
        if (!scopes)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return serialize_json_rpc_value(*scopes, std::move(request.id), response);
    }
    if (request.method == "search_all_messages")
    {
        if (!request.id.present)
        {
            co_return simdjson::SUCCESS;
        }
        search_all_params params{};
        bool const parsed = request.params.read(params);
        auto const category = params.category ? chat::parse_message_category(*params.category) : chat::message_category::all;
        if (!parsed || !category || (params.query && !valid_search(*params.query)) || (params.before && *params.before <= 0) ||
            (params.conversation && *params.conversation <= 0) || (params.sender && *params.sender <= 0) ||
            (params.start && params.end && *params.start >= *params.end) ||
            (!params.query && !params.conversation && *category == chat::message_category::all && !params.sender && !params.start && !params.end &&
             !params.mentions_me.value_or(false)))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        chat::message_search_filter filter{
            params.query.value_or(""), params.conversation, *category, params.sender, params.start, params.end, params.mentions_me.value_or(false)};
        auto result =
            co_await database_.search([&, user = *user_id_](sqlite_database& db) { return search_all_messages(db, user, filter, params.before); });
        if (!result)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return serialize_json_rpc_value(*result, std::move(request.id), response);
    }
    get_messages_params params{};
    bool const searching = request.method == "search_messages";
    bool const parsed = request.params.read(params);
    auto const category = params.category ? chat::parse_message_category(*params.category) : std::nullopt;
    if (!parsed || (params.category && !category) || params.conversation <= 0 || (params.before && *params.before <= 0) ||
        (params.after && (*params.after < 0 || params.before)) ||
        (params.around && (*params.around <= 0 || params.before || params.after || searching || category)) || (category && params.after) ||
        (params.capture_unread.value_or(false) && (searching || params.before || params.after || params.around || category)) ||
        (params.query && (!valid_search(*params.query) || (!searching && !category))) || (searching && (!params.query || params.after || category)))
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    auto read = [&, user = *user_id_](sqlite_database& db)
    {
        return read_message_history(db,
                                    user,
                                    params.conversation,
                                    params.before,
                                    params.after,
                                    params.query ? std::string_view(*params.query) : "",
                                    params.around,
                                    category,
                                    params.capture_unread.value_or(false));
    };
    auto history = category || searching || params.around ? co_await database_.search(read) : co_await run_db(read);
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!history || !*history)
    {
        co_return history ? serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response)
                          : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    struct result
    {
        std::vector<message_payload> messages;
        decltype(chat::messages_result::read_positions) read_positions;
        bool has_more;
        std::int64_t visible_after;
        bool hidden;
        std::optional<std::int64_t> first_unread;
    };
    result value{
        {}, std::move((*history)->read_positions), (*history)->has_more, (*history)->visible_after, (*history)->hidden, (*history)->first_unread};
    value.messages.reserve((*history)->messages.size());
    for (auto const& message : (*history)->messages)
    {
        value.messages.push_back(make_message_payload(message, true));
    }
    co_return serialize_json_rpc_value(value, std::move(request.id), response);
}

boost::capy::task<simdjson::error_code> chat_session::handle_message_bookmarks(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    if (request.method == "list_message_bookmarks")
    {
        bookmark_list_params params;
        if (!request.params.read(params) || (params.query && !valid_search(*params.query)) || (params.before && *params.before <= 0))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        auto page = co_await database_.search(
            [&, user = *user_id_, login = login_id_](sqlite_database& db) -> std::expected<chat::bookmark_page, sqlite_error>
            {
                auto snapshot = sqlite_transaction::begin_read(db);
                if (!snapshot)
                {
                    return std::unexpected(std::move(snapshot.error()));
                }
                auto active = login_active(db, login, epoch_milliseconds());
                if (!active)
                {
                    return std::unexpected(std::move(active.error()));
                }
                if (!*active)
                {
                    return std::unexpected(sqlite_error{SQLITE_AUTH, "Login session no longer valid"});
                }
                return read_message_bookmarks(db, user, params.query ? std::string_view(*params.query) : "", params.before);
            });
        if (!page)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return serialize_json_rpc_value(*page, std::move(request.id), response);
    }

    bookmark_params params;
    bool const setting = request.method == "set_message_bookmark";
    if (!request.params.read(params) || params.message <= 0 || params.bookmarked.has_value() != setting)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    if (!setting)
    {
        auto exists = co_await run_db([&, user = *user_id_](sqlite_database& db) { return has_message_bookmark(db, user, params.message); });
        if (!exists)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return serialize_json_rpc_value(chat::bookmark_state{params.message, *exists}, std::move(request.id), response);
    }

    auto changed =
        co_await run_db([&, user = *user_id_](sqlite_database& db) { return set_message_bookmark(db, user, params.message, *params.bookmarked); });
    if (!changed)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!*changed)
    {
        co_return serialize_json_rpc_error(-32007, "Message unavailable", std::move(request.id), response);
    }
    if (**changed)
    {
        struct notification
        {
            std::int64_t message;
        };
        std::string payload;
        if (!simdjson::builder::to_json_string(notification{params.message}).get(payload))
        {
            auto json = R"({"jsonrpc":"2.0","method":"message_bookmark_changed","params":)" + payload + "}";
            for (auto* session : users_.sessions(*user_id_))
            {
                session->enqueue_message(json);
            }
        }
    }
    co_return serialize_json_rpc_value(chat::bookmark_state{params.message, *params.bookmarked}, std::move(request.id), response);
}
