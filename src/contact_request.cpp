#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "friendship_store.hpp"
#include "user_payload.hpp"

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
    if (request.method == "get_friend_requests")
    {
        auto requests = co_await database_.run([user=*user_id_](sqlite_database& db) { return read_friend_requests(db, user); });
        if (!requests)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        struct item { user_payload user; std::int64_t created_at; };
        struct result { std::vector<item> incoming, outgoing; };
        result value;
        for (auto const& request : requests->incoming)
        {
            value.incoming.push_back({user_payload(request.user), request.created_at});
        }
        for (auto const& request : requests->outgoing)
        {
            value.outgoing.push_back({user_payload(request.user), request.created_at});
        }
        co_return serialize_json_rpc_value(value, std::move(request.id), response);
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
    auto action = friendship_action::remove;
    if (request.method == "send_friend_request")
    {
        action = friendship_action::request;
    }
    else if (request.method == "respond_friend_request")
    {
        action = answer.accept ? friendship_action::accept : friendship_action::reject;
    }
    else if (request.method == "cancel_friend_request")
    {
        action = friendship_action::cancel;
    }
    auto result = co_await database_.run([=, user=*user_id_](sqlite_database& db) {
        return change_friendship(db, user, params.user, action);
    });
    if (!result)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto const* rejection = std::get_if<friendship_rejection>(&*result))
    {
        std::string_view message;
        switch (*rejection)
        {
            case friendship_rejection::user_not_found: message = "User not found"; break;
            case friendship_rejection::already_friends: message = "Already friends"; break;
            case friendship_rejection::no_incoming_request: message = "Incoming friend request not found"; break;
        }
        co_return serialize_json_rpc_error(-32004, message, std::move(request.id), response);
    }
    auto const& change = std::get<friendship_change>(*result);
    if (change.previous != change.result.state)
    {
        auto notify = [&](std::int64_t recipient, std::int64_t peer) {
            if (auto* session = users_.find(recipient))
            {
                if (change.result.state == chat::friendship_state::none && session->upload_ &&
                    session->upload_->conversation == change.direct_conversation)
                {
                    session->upload_.reset();
                }
                session->enqueue_message(R"({"jsonrpc":"2.0","method":"friendship","params":{"user":)" +
                                         std::to_string(peer) + "}}");
            }
        };
        notify(*user_id_, params.user);
        notify(params.user, *user_id_);
    }
    if (action == friendship_action::remove)
    {
        co_return serialize_json_rpc_success(change.previous == chat::friendship_state::accepted
            ? "{\"removed\":true}" : "{\"removed\":false}", std::move(request.id), response);
    }
    struct payload { user_payload user; std::string_view state; };
    std::string_view state;
    switch (change.result.state)
    {
        case chat::friendship_state::accepted: state = "accepted"; break;
        case chat::friendship_state::incoming_pending: state = "incoming_pending"; break;
        case chat::friendship_state::outgoing_pending: state = "outgoing_pending"; break;
        case chat::friendship_state::none: state = "none"; break;
    }
    co_return serialize_json_rpc_value(payload{user_payload(change.result.user), state}, std::move(request.id), response);
}
