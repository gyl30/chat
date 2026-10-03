#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{
struct [[= simdjson::deny_unknown_fields]] contact_params
{
    std::int64_t user = 0;
};
struct [[= simdjson::deny_unknown_fields]] respond_params
{
    std::int64_t user = 0;
    bool accept = false;
};

template<class T>
bool parse_params(json_rpc_params& params, T& value)
{
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    return params.present && !parser.iterate(params.json).get(document) &&
        !document.get(value) && document.at_end() && value.user > 0;
}
}

boost::capy::task<simdjson::error_code> chat_session::handle_contact_change(json_rpc_request& request,
                                                                        std::string& response)
{
    if (!request.id.present) { co_return simdjson::SUCCESS; }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto& db = lease.connection();
    if (request.method == "get_friend_requests")
    {
        auto result = co_await db.execute_scalar(
            "WITH found AS (SELECT f.requester_id=$1::bigint AS outgoing,"
            "json_build_object('user',json_build_object('id',u.id,'username',u.username,"
            "'avatar_revision',u.avatar_revision,'has_avatar',EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id)),"
            "'created_at',(extract(epoch FROM f.created_at)*1000)::bigint) AS value,f.created_at,u.id "
            "FROM friend_requests f JOIN users u ON u.id=CASE WHEN f.requester_id=$1::bigint "
            "THEN f.recipient_id ELSE f.requester_id END WHERE f.requester_id=$1::bigint OR f.recipient_id=$1::bigint) "
            "SELECT json_build_object('incoming',COALESCE(json_agg(value ORDER BY created_at DESC,id DESC) "
            "FILTER(WHERE NOT outgoing),'[]'::json),'outgoing',COALESCE(json_agg(value ORDER BY created_at DESC,id DESC) "
            "FILTER(WHERE outgoing),'[]'::json))::text FROM found", {std::to_string(*user_id_)});
        if (std::get<0>(result))
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        co_return serialize_json_rpc_success(std::get<1>(result), std::move(request.id), response);
    }
    contact_params params;
    respond_params answer;
    bool parsed = request.method == "respond_friend_request" ? parse_params(request.params, answer) :
                                                              parse_params(request.params, params);
    if (request.method == "respond_friend_request") { params.user = answer.user; }
    if (!parsed || params.user == *user_id_)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    std::vector<std::string> values{std::to_string(*user_id_), std::to_string(params.user)};
    auto begun = co_await db.execute_row("BEGIN");
    // Serialize an unordered pair even before a request/direct conversation exists. NO KEY UPDATE
    // permits foreign-key KEY SHARE locks taken by sends that already hold the conversation lock.
    auto locked = co_await db.execute_scalar(
        "SELECT count(*)::text FROM (SELECT id FROM users WHERE id IN($1::bigint,$2::bigint) "
        "ORDER BY id FOR NO KEY UPDATE) pair", values);
    if (std::get<0>(begun) || std::get<0>(locked))
    {
        db.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (std::get<1>(locked) != "2")
    {
        auto rollback = co_await db.execute_row("ROLLBACK");
        if (std::get<0>(rollback)) { db.close(); }
        co_return serialize_json_rpc_error(-32004, "User not found", std::move(request.id), response);
    }
    auto conversation = co_await db.execute_row(
        "SELECT id::text FROM conversations WHERE kind='direct' AND direct_user_low=least($1::bigint,$2::bigint) "
        "AND direct_user_high=greatest($1::bigint,$2::bigint) FOR UPDATE", values);
    auto snapshot = co_await db.execute_row(
        "SELECT json_build_object('id',u.id,'username',u.username,'avatar_revision',u.avatar_revision,"
        "'has_avatar',EXISTS(SELECT 1 FROM user_avatars WHERE user_id=u.id))::text,"
        "EXISTS(SELECT 1 FROM contacts WHERE owner_id=$1::bigint AND contact_id=$2::bigint)::text,"
        "COALESCE((SELECT requester_id::text FROM friend_requests WHERE "
        "(requester_id=$1::bigint AND recipient_id=$2::bigint) OR "
        "(requester_id=$2::bigint AND recipient_id=$1::bigint)),'') FROM users u WHERE u.id=$2::bigint", values);
    if (std::get<0>(conversation) || std::get<0>(snapshot) || !std::get<1>(snapshot))
    {
        db.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto const& row = *std::get<1>(snapshot);
    bool accepted = row[1] == "true";
    auto const& requester = row[2];
    std::string state = accepted ? "accepted" : requester.empty() ? "none" :
        requester == values[0] ? "outgoing_pending" : "incoming_pending";
    std::string mutation;
    bool changed = false;
    bool removed = false;
    if (request.method == "send_friend_request")
    {
        if (accepted)
        {
            auto rollback = co_await db.execute_row("ROLLBACK");
            if (std::get<0>(rollback)) { db.close(); }
            co_return serialize_json_rpc_error(-32004, "Already friends", std::move(request.id), response);
        }
        if (requester.empty())
        {
            mutation = "INSERT INTO friend_requests(requester_id,recipient_id) VALUES($1::bigint,$2::bigint)";
            state = "outgoing_pending";
            changed = true;
        }
    }
    else if (request.method == "respond_friend_request")
    {
        if (requester != values[1])
        {
            auto rollback = co_await db.execute_row("ROLLBACK");
            if (std::get<0>(rollback)) { db.close(); }
            co_return serialize_json_rpc_error(-32004, "Incoming friend request not found", std::move(request.id), response);
        }
        mutation = "WITH removed AS (DELETE FROM friend_requests WHERE requester_id=$2::bigint "
                   "AND recipient_id=$1::bigint RETURNING 1) ";
        if (answer.accept)
        {
            mutation += "INSERT INTO contacts(owner_id,contact_id) SELECT $1::bigint,$2::bigint FROM removed "
                        "UNION ALL SELECT $2::bigint,$1::bigint FROM removed ON CONFLICT DO NOTHING";
            state = "accepted";
        }
        else { mutation += "SELECT count(*)::text FROM removed"; state = "none"; }
        changed = true;
    }
    else if (request.method == "cancel_friend_request")
    {
        if (requester == values[0])
        {
            mutation = "DELETE FROM friend_requests WHERE requester_id=$1::bigint AND recipient_id=$2::bigint";
            state = "none";
            changed = true;
        }
    }
    else
    {
        mutation = "WITH requests AS (DELETE FROM friend_requests WHERE "
                   "(requester_id=$1::bigint AND recipient_id=$2::bigint) OR "
                   "(requester_id=$2::bigint AND recipient_id=$1::bigint)) "
                   "DELETE FROM contacts WHERE (owner_id=$1::bigint AND contact_id=$2::bigint) OR "
                   "(owner_id=$2::bigint AND contact_id=$1::bigint)";
        removed = accepted;
        changed = accepted || !requester.empty();
        state = "none";
    }
    if (!mutation.empty())
    {
        auto updated = co_await db.execute_row(std::move(mutation), values);
        if (std::get<0>(updated))
        {
            db.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
    }
    auto committed = co_await db.execute_row("COMMIT");
    if (std::get<0>(committed))
    {
        db.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (changed)
    {
        auto notify = [&](std::int64_t recipient, std::int64_t peer) {
            if (auto* session = users_.find(recipient))
            {
                if (state == "none" && std::get<1>(conversation) && session->upload_ &&
                    session->upload_->conversation == std::stoll(std::get<1>(conversation)->front()))
                { session->upload_.reset(); }
                session->enqueue_message("{\"jsonrpc\":\"2.0\",\"method\":\"friendship\",\"params\":{\"user\":" +
                    std::to_string(peer) + "}}");
            }
        };
        notify(*user_id_, params.user);
        notify(params.user, *user_id_);
    }
    if (request.method == "remove_contact")
    {
        co_return serialize_json_rpc_success(removed ? "{\"removed\":true}" : "{\"removed\":false}",
                                            std::move(request.id), response);
    }
    co_return serialize_json_rpc_success("{\"user\":" + row[0] + ",\"state\":\"" + state + "\"}",
                                        std::move(request.id), response);
}
