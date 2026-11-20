#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "group_access_store.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_group_join_request(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    auto const configuring = request.method == "set_group_join_approval";
    auto const responding = request.method == "respond_group_join_request";
    std::int64_t conversation = 0, user = 0;
    std::optional<std::int64_t> before;
    std::optional<bool> choice;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    simdjson::error_code parse_error;
    if (configuring)
    {
        struct[[= simdjson::deny_unknown_fields]] group_join_approval_params
        {
            std::int64_t conversation = 0;
            std::optional<bool> required;
        };
        group_join_approval_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        choice = params.required;
    }
    else if (responding)
    {
        struct[[= simdjson::deny_unknown_fields]] group_join_response_params
        {
            std::int64_t conversation = 0;
            std::int64_t user = 0;
            std::optional<bool> accept;
        };
        group_join_response_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        user = params.user;
        choice = params.accept;
    }
    else
    {
        struct[[= simdjson::deny_unknown_fields]] group_join_list_params
        {
            std::int64_t conversation = 0;
            std::optional<std::int64_t> before;
        };
        group_join_list_params params;
        parse_error = document.get(params);
        conversation = params.conversation;
        before = params.before;
    }
    if (parse_error || !document.at_end() || conversation <= 0 || (before && *before <= 0) || ((configuring || responding) && !choice.has_value()) ||
        (responding && user <= 0))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto const actor = *user_id_;
    auto group_error = [&](group_member_rejection reason)
    {
        bool const unavailable = reason == group_member_rejection::unavailable;
        return serialize_json_rpc_error(
            unavailable ? -32006 : -32009, unavailable ? "Group unavailable" : "Group permission denied", std::move(request.id), response);
    };
    if (!configuring && !responding)
    {
        auto requests = co_await database_.run([=](sqlite_database& db) { return read_group_join_requests(db, conversation, actor, before); });
        if (!requests)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (auto const* rejected = std::get_if<group_member_rejection>(&*requests))
        {
            co_return group_error(*rejected);
        }
        auto& page = std::get<chat::group_join_requests_result>(*requests);
        struct applicant
        {
            std::int64_t id;
            std::string username;
            std::int64_t avatar_revision;
            bool has_avatar;
            std::int64_t created_at;
        };
        struct result
        {
            std::vector<applicant> requests;
            std::optional<std::int64_t> next;
        };
        result payload{{}, page.next};
        for (auto& item : page.requests)
        {
            auto& user = item.applicant;
            payload.requests.push_back({user.id, std::move(user.username), user.avatar.revision, user.avatar.present, item.created_at});
        }
        co_return serialize_json_rpc_value(payload, std::move(request.id), response);
    }
    auto updated = co_await database_.run(
        [=](sqlite_database& db)
        {
            return configuring ? set_group_join_approval(db, conversation, actor, *choice)
                               : respond_to_group_join_request(db, conversation, actor, user, *choice);
        });
    if (!updated)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (auto const* rejected = std::get_if<group_member_rejection>(&*updated))
    {
        co_return group_error(*rejected);
    }
    bool const changed = std::get<bool>(*updated);
    if (changed)
    {
        if (responding)
        {
            co_await publish_join_request(conversation, user, *choice ? "accepted" : "rejected");
        }
        if (configuring || *choice)
        {
            co_await publish_conversation(
                conversation, R"({"jsonrpc":"2.0","method":"conversation","params":{"conversation":)" + std::to_string(conversation) + "}}");
        }
    }
    co_return serialize_json_rpc_success(changed ? "{\"changed\":true}" : "{\"changed\":false}", std::move(request.id), response);
}

boost::capy::task<void> chat_session::publish_join_request(std::int64_t conversation, std::int64_t user, std::string state)
{
    auto recipients = co_await database_.run(
        [&](sqlite_database& db) -> std::expected<std::vector<std::int64_t>, sqlite_error>
        {
            auto query = db.prepare(R"SQL(
            WITH current AS (
                SELECT EXISTS(SELECT 1 FROM group_join_requests WHERE conversation_id=?1 AND user_id=?2) AS pending,
                       EXISTS(SELECT 1 FROM conversation_members WHERE conversation_id=?1 AND user_id=?2) AS member
            ), recipients AS (
                SELECT m.user_id FROM conversation_members m JOIN conversations c ON c.id=m.conversation_id
                WHERE c.id=?1 AND (m.is_admin OR m.user_id=c.owner_id)
                UNION SELECT ?2
            )
            SELECT user_id FROM recipients,current WHERE
              CASE ?3 WHEN 'pending' THEN pending WHEN 'accepted' THEN member ELSE NOT pending AND NOT member END
        )SQL",
                                    conversation,
                                    user,
                                    state);
            if (!query)
            {
                return std::unexpected(std::move(query.error()));
            }
            std::vector<std::int64_t> result;
            for (;;)
            {
                auto row = query->step();
                if (!row)
                {
                    return std::unexpected(std::move(row.error()));
                }
                if (!*row)
                {
                    return result;
                }
                result.push_back(query->column_int64(0));
            }
        });
    if (!recipients)
    {
        co_return;
    }
    std::string notification = R"({"jsonrpc":"2.0","method":"join_request","params":{"conversation":)" + std::to_string(conversation) +
                               R"(,"user":)" + std::to_string(user) + R"(,"state":")" + state + "\"}}";
    for (auto const recipient : *recipients)
    {
        for (auto* session : users_.sessions(recipient))
        {
            session->enqueue_message(notification);
        }
    }
}
