#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_group_join_request(json_rpc_request& request, std::string& response)
{
    if (!request.id.present) { co_return simdjson::SUCCESS; }
    if (!user_id_) { co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response); }
    auto const configuring = request.method == "set_group_join_approval";
    auto const responding = request.method == "respond_group_join_request";
    std::int64_t conversation = 0, user = 0;
    std::optional<std::int64_t> before;
    std::optional<bool> choice;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document))
    { co_return serialize_json_rpc_invalid_params(std::move(request.id), response); }
    simdjson::error_code parse_error;
    if (configuring)
    {
        struct [[= simdjson::deny_unknown_fields]] group_join_approval_params { std::int64_t conversation = 0; std::optional<bool> required; };
        group_join_approval_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        choice = params.required;
    }
    else if (responding)
    {
        struct [[= simdjson::deny_unknown_fields]] group_join_response_params { std::int64_t conversation = 0; std::int64_t user = 0; std::optional<bool> accept; };
        group_join_response_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        user = params.user;
        choice = params.accept;
    }
    else
    {
        struct [[= simdjson::deny_unknown_fields]] group_join_list_params { std::int64_t conversation = 0; std::optional<std::int64_t> before; };
        group_join_list_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        before = params.before;
    }
    if (parse_error || !document.at_end() || conversation <= 0 || (before && *before <= 0) ||
        ((configuring || responding) && !choice.has_value()) || (responding && user <= 0))
    { co_return serialize_json_rpc_invalid_params(std::move(request.id), response); }
    auto lease = co_await database_.acquire();
    if (lease.error()) { co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response); }
    auto& connection = lease.connection();
    auto begun = co_await connection.execute_row("BEGIN");
    auto locked = co_await connection.execute_row(
        "SELECT kind,coalesce(owner_id,0)::text FROM conversations WHERE id=$1::bigint FOR UPDATE", {std::to_string(conversation)});
    auto actor = co_await connection.execute_row(
        "SELECT is_admin::text FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint",
        {std::to_string(conversation), std::to_string(*user_id_)});
    if (std::get<0>(begun) || std::get<0>(locked) || std::get<0>(actor))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto const& group = std::get<1>(locked);
    int error = 0;
    std::string message, result;
    bool changed = false;
    if (!group || (*group)[0] != "group" || !std::get<1>(actor)) { error = -32006; message = "Group unavailable"; }
    else if ((*group)[1] != std::to_string(*user_id_) && std::get<1>(actor)->front() != "true")
    { error = -32009; message = "Group permission denied"; }
    else if (configuring)
    {
        auto updated = co_await connection.execute_row(
            "UPDATE conversations SET join_approval=$2::boolean WHERE id=$1::bigint AND join_approval IS DISTINCT FROM $2::boolean RETURNING id::text",
            {std::to_string(conversation), *choice ? "true" : "false"});
        if (std::get<0>(updated))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        changed = std::get<1>(updated).has_value();
    }
    else if (responding)
    {
        auto removed = co_await connection.execute_row(
            "DELETE FROM group_join_requests WHERE conversation_id=$1::bigint AND user_id=$2::bigint RETURNING user_id::text",
            {std::to_string(conversation), std::to_string(user)});
        if (std::get<0>(removed))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        changed = std::get<1>(removed).has_value();
        if (changed && *choice)
        {
            auto added = co_await connection.execute_row(
                "INSERT INTO conversation_members(conversation_id,user_id,joined_message_id) VALUES($1::bigint,$2::bigint,"
                "(SELECT coalesce(max(id),0) FROM messages WHERE conversation_id=$1::bigint)) ON CONFLICT DO NOTHING",
                {std::to_string(conversation), std::to_string(user)});
            if (std::get<0>(added))
            {
                connection.close();
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
        }
    }
    else
    {
        auto listed = co_await connection.execute_scalar(R"sql(
            WITH page AS (
                SELECT * FROM group_join_requests WHERE conversation_id=$1::bigint
                AND ($2='' OR user_id<NULLIF($2,'')::bigint) ORDER BY user_id DESC LIMIT 51
            ), visible AS (SELECT * FROM page ORDER BY user_id DESC LIMIT 50)
            SELECT json_build_object('requests',coalesce((SELECT json_agg(json_build_object(
                'id',u.id,'username',u.username,'avatar_revision',u.avatar_revision,
                'has_avatar',EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id),
                'created_at',(extract(epoch FROM v.created_at)*1000)::bigint) ORDER BY v.user_id DESC)
                FROM visible v JOIN users u ON u.id=v.user_id),'[]'::json),
                'next',CASE WHEN (SELECT count(*) FROM page)>50 THEN (SELECT min(user_id) FROM visible) END)::text
        )sql", {std::to_string(conversation), before ? std::to_string(*before) : std::string{}});
        if (std::get<0>(listed))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        result = std::move(std::get<1>(listed));
    }
    auto ended = co_await connection.execute_row(error ? "ROLLBACK" : "COMMIT");
    if (std::get<0>(ended))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    lease = {};
    if (error) { co_return serialize_json_rpc_error(error, message, std::move(request.id), response); }
    if (changed && responding)
    {
        co_await publish_join_request(conversation, user, *choice ? "accepted" : "rejected");
        if (*choice) { co_await publish_conversation(conversation, "{\"jsonrpc\":\"2.0\",\"method\":\"conversation\",\"params\":{\"conversation\":" + std::to_string(conversation) + "}}"); }
    }
    else if (changed)
    { co_await publish_conversation(conversation, "{\"jsonrpc\":\"2.0\",\"method\":\"conversation\",\"params\":{\"conversation\":" + std::to_string(conversation) + "}}"); }
    if (configuring || responding) { result = changed ? "{\"changed\":true}" : "{\"changed\":false}"; }
    co_return serialize_json_rpc_success(result, std::move(request.id), response);
}

boost::capy::task<void> chat_session::publish_join_request(std::int64_t conversation, std::int64_t user, std::string state)
{
    auto lease = co_await database_.acquire();
    if (lease.error()) { co_return; }
    auto& connection = lease.connection();
    auto begun = co_await connection.execute_row("BEGIN");
    auto locked = co_await connection.execute_row("SELECT id::text FROM conversations WHERE id=$1::bigint FOR UPDATE", {std::to_string(conversation)});
    auto recipients = co_await connection.execute_scalar(R"sql(
        SELECT coalesce(string_agg(id::text,','),'') FROM (
            SELECT m.user_id AS id FROM conversation_members m JOIN conversations c ON c.id=m.conversation_id
            WHERE m.conversation_id=$1::bigint AND (m.is_admin OR m.user_id=c.owner_id)
            UNION SELECT $2::bigint
        ) targets WHERE CASE $3
            WHEN 'pending' THEN EXISTS(SELECT 1 FROM group_join_requests WHERE conversation_id=$1::bigint AND user_id=$2::bigint)
            WHEN 'accepted' THEN EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint)
            WHEN 'rejected' THEN NOT EXISTS(SELECT 1 FROM group_join_requests WHERE conversation_id=$1::bigint AND user_id=$2::bigint)
                AND NOT EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=$1::bigint AND user_id=$2::bigint)
        END
    )sql", {std::to_string(conversation), std::to_string(user), state});
    if (std::get<0>(begun) || std::get<0>(locked) || std::get<0>(recipients)) { connection.close(); co_return; }
    auto const notification = "{\"jsonrpc\":\"2.0\",\"method\":\"join_request\",\"params\":{\"conversation\":" +
        std::to_string(conversation) + ",\"user\":" + std::to_string(user) + ",\"state\":\"" + state + "\"}}";
    std::string_view remaining = std::get<1>(recipients);
    while (!remaining.empty())
    {
        auto const separator = remaining.find(',');
        auto const token = remaining.substr(0, separator);
        std::int64_t recipient = 0;
        auto const [end, error] = std::from_chars(token.data(), token.data() + token.size(), recipient);
        if (error == std::errc{} && end == token.data() + token.size())
        { if (auto* session = users_.find(recipient)) { session->enqueue_message(notification); } }
        if (separator == std::string_view::npos) { break; }
        remaining.remove_prefix(separator + 1);
    }
    auto committed = co_await connection.execute_row("COMMIT");
    if (std::get<0>(committed)) { connection.close(); }
}
