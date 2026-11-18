#include <string>
#include <string_view>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"
#include "message_payload.hpp"
#include "message_changes.hpp"
#include "database.hpp"
#include <sqlite3.h>

boost::capy::task<simdjson::error_code> chat_session::handle_update_message(json_rpc_request& request,
                                                                            std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    struct [[= simdjson::deny_unknown_fields]] update_params
    {
        std::int64_t conversation = 0;
        std::int64_t message = 0;
        std::optional<std::string> text;
    };
    update_params params;
    auto const deleting = request.method == "delete_message";
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end() || params.conversation <= 0 || params.message <= 0 ||
        (deleting ? params.text.has_value() : (!params.text || params.text->empty() || params.text->find('\0') != std::string::npos)))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    constexpr std::string_view prefix = R"({"jsonrpc":"2.0","method":"message_updated","params":)";
    struct update_result { std::string acknowledgement, notification; bool cleared_pin; };
    using outcome = std::variant<update_result, message_change_rejection>;
    auto update = co_await database_.run([&, user=*user_id_](sqlite_database& db) -> std::expected<outcome, sqlite_error> {
        auto tx = sqlite_transaction::begin_immediate(db);
        if (!tx)
        {
            return std::unexpected(std::move(tx.error()));
        }
        std::optional<std::string_view> text;
        if (params.text)
        {
            text = *params.text;
        }
        auto changed = change_message_body(db, user, params.conversation, params.message, text);
        if (!changed)
        {
            return std::unexpected(std::move(changed.error()));
        }
        if (auto const* rejection = std::get_if<message_change_rejection>(&*changed))
        {
            return *rejection;
        }
        auto const& saved = std::get<changed_message>(*changed);
        update_result result{{}, {}, saved.cleared_pin};
        auto value = make_message_payload(saved.message);
        std::string payload;
        if (simdjson::builder::to_json_string(value).get(payload) ||
            serialize_json_rpc_success(payload, request.id, result.acknowledgement))
        {
            return std::unexpected(sqlite_error{SQLITE_ERROR, "cannot serialize updated message"});
        }
        if (prefix.size() + payload.size() + 1 > 64 * 1024)
        {
            return std::unexpected(sqlite_error{SQLITE_TOOBIG, "updated message exceeds the frame limit"});
        }
        result.notification = std::string(prefix) + payload + "}";
        if (auto error = tx->commit(); error)
        {
            return std::unexpected(std::move(error));
        }
        return result;
    });
    if (!update)
    {
        co_return update.error().code == SQLITE_TOOBIG
            ? serialize_json_rpc_invalid_params(std::move(request.id), response)
            : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto const* rejected = std::get_if<message_change_rejection>(&*update))
    {
        bool const unavailable = *rejected == message_change_rejection::unavailable;
        co_return serialize_json_rpc_error(unavailable ? -32007 : -32006,
            unavailable ? "Message unavailable" : "Communication not allowed", std::move(request.id), response);
    }
    auto& result = std::get<update_result>(*update);
    co_await publish_conversation(params.conversation, std::move(result.notification));
    if (result.cleared_pin)
    {
        co_await publish_conversation(params.conversation, R"({"jsonrpc":"2.0","method":"conversation","params":{"conversation":)" +
            std::to_string(params.conversation) + "}}");
    }
    response = std::move(result.acknowledgement);
    co_return simdjson::SUCCESS;
}
