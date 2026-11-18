#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <simdjson.h>
#include <chat/text.hpp>

#include "chat_session.hpp"
#include "database.hpp"
#include "conversation_store.hpp"
#include "user_payload.hpp"

namespace
{

struct [[= simdjson::deny_unknown_fields]] create_conversation_params
{
    [[= simdjson::default_value]] std::int64_t user = 0;
    [[= simdjson::default_value]] std::string title;
    [[= simdjson::default_value]] std::vector<std::int64_t> members;
};

}

boost::capy::task<simdjson::error_code> chat_session::handle_create_conversation(json_rpc_request& request,
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
    create_conversation_params params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end())
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto const group = request.method == "create_group";
    std::sort(params.members.begin(), params.members.end());
    if ((!group && (params.user <= 0 || !params.title.empty() || !params.members.empty())) ||
        (group && (params.user != 0 || !chat::valid_group_title(params.title) || params.members.empty() ||
                   params.members.front() <= 0 ||
                   std::adjacent_find(params.members.begin(), params.members.end()) != params.members.end() ||
                   std::binary_search(params.members.begin(), params.members.end(), *user_id_))))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto id = co_await database_.run([&, user=*user_id_](sqlite_database& db) {
        return group ? create_group(db, user, params.title, params.members) : open_direct(db, user, params.user);
    });
    if (!id)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!*id)
    {
        co_return serialize_json_rpc_error(-32005, group ? "User unavailable" : "Contact required", std::move(request.id), response);
    }
    auto const conversation = std::to_string(**id);
    if (group)
    {
        co_await publish_conversation(**id, R"({"jsonrpc":"2.0","method":"conversation","params":{"conversation":)" + conversation + "}}");
    }
    co_return serialize_json_rpc_success("{\"conversation\":" + conversation + (group ? "}" : ",\"can_send\":true}"),
                                         std::move(request.id), response);
}

boost::capy::task<simdjson::error_code> chat_session::handle_get_members(json_rpc_request& request,
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
    struct [[= simdjson::deny_unknown_fields]] params_type
    {
        std::int64_t conversation = 0;
    };
    params_type params;
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document) || document.get(params) ||
        !document.at_end() || params.conversation <= 0)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto members = co_await database_.run([=, user=*user_id_](sqlite_database& db) {
        return read_conversation_members(db, params.conversation, user);
    });
    if (!members)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (members->empty())
    {
        co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
    }
    struct member_payload
    {
        std::int64_t id;
        std::string username;
        std::int64_t avatar_revision;
        bool has_avatar;
        std::string_view role;
    };
    struct result { std::vector<member_payload> members; };
    result value;
    value.members.reserve(members->size());
    for (auto& member : *members)
    {
        auto role = member.role == chat::member_role::owner ? "owner" :
            member.role == chat::member_role::admin ? "admin" : "member";
        value.members.push_back({member.id, std::move(member.username), member.avatar.revision, member.avatar.present, role});
    }
    co_return serialize_json_rpc_value(value, std::move(request.id), response);
}
