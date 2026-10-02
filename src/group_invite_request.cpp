#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <openssl/rand.h>
#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_group_invite(json_rpc_request& request, std::string& response)
{
    if (!request.id.present) { co_return simdjson::SUCCESS; }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    auto const joining = request.method == "join_group";
    auto const creating = request.method == "create_group_invite";
    auto const revoking = request.method == "revoke_group_invite";
    std::int64_t conversation = 0;
    std::string token;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    simdjson::error_code parse_error;
    if (joining)
    {
        struct [[= simdjson::deny_unknown_fields]] join_group_params { std::string token; };
        join_group_params params;
        parse_error = document.get(params);
        token = std::move(params.token);
    }
    else
    {
        struct [[= simdjson::deny_unknown_fields]] group_invite_params { std::int64_t conversation = 0; };
        group_invite_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
    }
    if (parse_error || !document.at_end() || (!joining && conversation <= 0) ||
        (joining && (token.size() != 64 || !std::all_of(token.begin(), token.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto lease = co_await database_.acquire();
    if (lease.error()) { co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response); }
    auto& connection = lease.connection();
    if (joining)
    {
        auto found = co_await connection.execute_row("SELECT id::text FROM conversations WHERE invite_token=$1", {token});
        if (std::get<0>(found))
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!std::get<1>(found))
        {
            co_return serialize_json_rpc_error(-32014, "Invite unavailable", std::move(request.id), response);
        }
        conversation = std::stoll(std::get<1>(found)->front());
    }
    auto begun = co_await connection.execute_row("BEGIN");
    if (std::get<0>(begun))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto locked = co_await connection.execute_row(
        "SELECT kind,coalesce(owner_id,0)::text,coalesce(title,''),coalesce(invite_token,''),join_approval::text "
        "FROM conversations WHERE id=$1::bigint FOR UPDATE", {std::to_string(conversation)});
    auto& [lock_ec, group] = locked;
    if (lock_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    int error = 0;
    std::string message;
    bool changed = false;
    bool request_changed = false;
    std::string join_state;
    std::string result;
    if (!group || (*group)[0] != "group" || (joining && (*group)[3] != token))
    {
        error = joining ? -32014 : -32006;
        message = joining ? "Invite unavailable" : "Group unavailable";
    }
    else if (joining)
    {
        auto member = co_await connection.execute_row(
            "SELECT user_id::text FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
            {std::to_string(conversation), std::to_string(*user_id_)});
        if (std::get<0>(member))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (std::get<1>(member)) { join_state = "member"; }
        else if ((*group)[4] == "true")
        {
            auto submitted = co_await connection.execute_row(
                "INSERT INTO group_join_requests(conversation_id,user_id) VALUES($1::bigint,$2::bigint) "
                "ON CONFLICT DO NOTHING RETURNING user_id::text", {std::to_string(conversation), std::to_string(*user_id_)});
            if (std::get<0>(submitted))
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            request_changed = std::get<1>(submitted).has_value();
            join_state = "pending";
        }
        else
        {
            auto inserted = co_await connection.execute_row(
                "INSERT INTO conversation_members(conversation_id,user_id,joined_message_id) "
                "VALUES($1::bigint,$2::bigint,(SELECT coalesce(max(id),0) FROM messages WHERE conversation_id=$1::bigint)) "
                "ON CONFLICT DO NOTHING RETURNING user_id::text", {std::to_string(conversation), std::to_string(*user_id_)});
            auto cleared = co_await connection.execute_row(
                "DELETE FROM group_join_requests WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING user_id::text",
                {std::to_string(conversation), std::to_string(*user_id_)});
            if (std::get<0>(inserted) || std::get<0>(cleared))
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            changed = std::get<1>(inserted).has_value();
            request_changed = std::get<1>(cleared).has_value();
            join_state = "joined";
        }
        auto count = co_await connection.execute_scalar(
            "SELECT count(*)::text FROM conversation_members WHERE conversation_id=$1::bigint", {std::to_string(conversation)});
        if (std::get<0>(count))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        struct join_result
        {
            std::int64_t conversation;
            std::string title;
            std::uint64_t member_count;
            std::string state;
        };
        join_result value{conversation, (*group)[2], std::stoull(std::get<1>(count)), join_state};
        if (simdjson::builder::to_json_string(value).get(result))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
    }
    else
    {
        auto actor = co_await connection.execute_row(
            "SELECT is_admin::text FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
            {std::to_string(conversation), std::to_string(*user_id_)});
        if (std::get<0>(actor))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!std::get<1>(actor)) { error = -32006; message = "Group unavailable"; }
        else if ((*group)[1] != std::to_string(*user_id_) && std::get<1>(actor)->front() != "true")
        {
            error = -32009;
            message = "Group permission denied";
        }
        else
        {
            std::optional<std::string> current;
            if (!(*group)[3].empty()) { current = (*group)[3]; }
            if (creating && !current)
            {
                std::array<unsigned char, 32> random;
                if (RAND_bytes(random.data(), random.size()) != 1)
                {
                    connection.close();
                    co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
                }
                std::string generated;
                generated.reserve(64);
                constexpr char digits[] = "0123456789abcdef";
                for (auto byte : random) { generated += digits[byte >> 4]; generated += digits[byte & 15]; }
                auto saved = co_await connection.execute_row("UPDATE conversations SET invite_token=$2 WHERE id=$1::bigint",
                    {std::to_string(conversation), generated});
                if (std::get<0>(saved))
                {
                    connection.close();
                    co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
                }
                current = std::move(generated);
                changed = true;
            }
            else if (revoking && current)
            {
                auto removed = co_await connection.execute_row("UPDATE conversations SET invite_token=NULL WHERE id=$1::bigint",
                    {std::to_string(conversation)});
                if (std::get<0>(removed))
                {
                    connection.close();
                    co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
                }
                current.reset();
                changed = true;
            }
            struct invite_result { std::optional<std::string> token; };
            invite_result value{std::move(current)};
            if (simdjson::builder::to_json_string(value).get(result))
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
        }
    }
    auto ended = co_await connection.execute_row(error ? "ROLLBACK" : "COMMIT");
    if (std::get<0>(ended))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    lease = {};
    if (error) { co_return serialize_json_rpc_error(error, message, std::move(request.id), response); }
    if (request_changed) { co_await publish_join_request(conversation, *user_id_, join_state == "pending" ? "pending" : "accepted"); }
    if (changed)
    {
        co_await publish_conversation(conversation,
            "{\"jsonrpc\":\"2.0\",\"method\":\"conversation\",\"params\":{\"conversation\":" + std::to_string(conversation) + "}}");
    }
    co_return serialize_json_rpc_success(result, std::move(request.id), response);
}
