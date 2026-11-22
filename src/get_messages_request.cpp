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
};

bool valid_search(std::string_view query) { return !query.empty() && query.size() <= 1024 && !query.contains('\0'); }

simdjson::error_code parse_params(json_rpc_params& params, auto& value)
{
    if (!params.present)
    {
        return simdjson::NO_SUCH_FIELD;
    }

    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    auto error = parser.iterate(params.json).get(document);
    if (error)
    {
        return error;
    }

    error = document.get(value);
    if (error)
    {
        return error;
    }

    if (!document.at_end())
    {
        return simdjson::TRAILING_CONTENT;
    }

    return simdjson::SUCCESS;
}

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
        auto const parse_error = parse_params(request.params, params);
        auto const category = params.category ? chat::parse_message_category(*params.category) : chat::message_category::all;
        if (parse_error || !category || (params.query && !valid_search(*params.query)) || (params.before && *params.before <= 0) ||
            (params.conversation && *params.conversation <= 0) || (params.sender && *params.sender <= 0) ||
            (params.start && params.end && *params.start >= *params.end) ||
            (!params.query && !params.conversation && *category == chat::message_category::all && !params.sender && !params.start && !params.end))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        chat::message_search_filter filter{params.query.value_or(""), params.conversation, *category, params.sender, params.start, params.end};
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
    auto const parse_error = parse_params(request.params, params);
    auto const category = params.category ? chat::parse_message_category(*params.category) : std::nullopt;
    if (parse_error || (params.category && !category) || params.conversation <= 0 || (params.before && *params.before <= 0) ||
        (params.after && (*params.after < 0 || params.before)) ||
        (params.around && (*params.around <= 0 || params.before || params.after || searching || category)) || (category && params.after) ||
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
        return read_message_history(
            db, user, params.conversation, params.before, params.after, params.query ? std::string_view(*params.query) : "", params.around, category);
    };
    auto history = category || searching || params.around ? co_await database_.search(read) : co_await database_.run(read);
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
    };
    result value{{}, std::move((*history)->read_positions), (*history)->has_more, (*history)->visible_after, (*history)->hidden};
    value.messages.reserve((*history)->messages.size());
    for (auto const& message : (*history)->messages)
    {
        value.messages.push_back(make_message_payload(message, true));
    }
    co_return serialize_json_rpc_value(value, std::move(request.id), response);
}
