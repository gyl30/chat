#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <chat/reaction.hpp>
#include <simdjson.h>
#include <sqlite3.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "message_changes.hpp"

namespace
{
struct[[= simdjson::deny_unknown_fields]] reaction_params
{
    std::int64_t conversation = 0;
    std::int64_t message = 0;
    std::optional<std::string> emoji;
};
}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_message_reaction(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    reaction_params params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) || !document.at_end() ||
        params.conversation <= 0 || params.message <= 0 || !params.emoji ||
        (!params.emoji->empty() && std::ranges::find(chat::reaction_choices, *params.emoji) == chat::reaction_choices.end()))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    struct reaction_result
    {
        std::string acknowledgement, notification;
    };
    using outcome = std::variant<reaction_result, message_change_rejection>;
    auto reaction = co_await database_.run(
        [&, user = *user_id_](sqlite_database& db) -> std::expected<outcome, sqlite_error>
        {
            auto tx = sqlite_transaction::begin_immediate(db);
            if (!tx)
            {
                return std::unexpected(std::move(tx.error()));
            }
            auto changed = set_message_reaction(db, user, params.conversation, params.message, *params.emoji);
            if (!changed)
            {
                return std::unexpected(std::move(changed.error()));
            }
            if (auto const* rejected = std::get_if<message_change_rejection>(&*changed))
            {
                return *rejected;
            }
            auto& change = std::get<changed_reaction>(*changed);
            struct wire_result
            {
                std::int64_t conversation, message, reaction_revision;
                std::vector<chat::reaction> reactions;
            };
            wire_result value{params.conversation, params.message, change.update.revision, std::move(change.update.reactions)};
            reaction_result result;
            std::string payload;
            if (simdjson::builder::to_json_string(value).get(payload) || serialize_json_rpc_success(payload, request.id, result.acknowledgement))
            {
                return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize reaction result"});
            }
            if (change.changed)
            {
                result.notification = R"({"jsonrpc":"2.0","method":"reaction","params":)" + payload + "}";
            }
            if (auto error = tx->commit(); error)
            {
                return std::unexpected(std::move(error));
            }
            return result;
        });
    if (!reaction)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto const* rejected = std::get_if<message_change_rejection>(&*reaction))
    {
        bool const unavailable = *rejected == message_change_rejection::unavailable;
        co_return serialize_json_rpc_error(
            unavailable ? -32007 : -32006, unavailable ? "Message unavailable" : "Communication not allowed", std::move(request.id), response);
    }
    auto& result = std::get<reaction_result>(*reaction);
    if (!result.notification.empty())
    {
        co_await publish_conversation(params.conversation, std::move(result.notification), false, params.message);
    }
    response = std::move(result.acknowledgement);
    co_return simdjson::SUCCESS;
}
