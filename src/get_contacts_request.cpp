#include <string>
#include <utility>
#include <vector>
#include <string_view>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

simdjson::error_code serialize_get_contacts_result(std::string_view contacts, json_rpc_id id, std::string& response)
{
    std::string result = R"({"users":)";
    result.append(contacts);
    result.push_back('}');
    return serialize_json_rpc_success(result, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_contacts(json_rpc_request& request, std::string& response)
{
    if (!user_id_)
    {
        if (request.id.present)
        {
            co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
        }
        co_return simdjson::SUCCESS;
    }

    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }

    std::string contacts_json;
    {
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }

        std::vector<std::string> parameters;
        parameters.emplace_back(std::to_string(*user_id_));
        auto query_result = co_await lease.connection().execute_scalar(
            "SELECT COALESCE("
            "array_to_json(array_agg(row_to_json(found) ORDER BY found.username, found.id)), "
            "'[]'::json"
            ")::text "
            "FROM ("
            "SELECT u.id, u.username FROM contacts c "
            "JOIN users u ON u.id = c.contact_id "
            "WHERE c.owner_id = $1::bigint "
            "ORDER BY u.username, u.id"
            ") AS found",
            std::move(parameters));
        auto& [query_ec, query_json] = query_result;
        if (query_ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        contacts_json = std::move(query_json);
    }

    co_return serialize_get_contacts_result(contacts_json, std::move(request.id), response);
}
