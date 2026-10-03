#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

namespace
{

simdjson::error_code serialize_get_presence_result(std::string_view users, json_rpc_id id, std::string& response)
{
    std::string result = R"({"users":)";
    result.append(users);
    result.push_back('}');
    return serialize_json_rpc_success(result, std::move(id), response);
}

}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_get_presence(json_rpc_request& request, std::string& response)
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

    std::string rows;
    {
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }

        std::vector<std::string> parameters;
        parameters.emplace_back(std::to_string(*user_id_));
        auto query_result = co_await lease.connection().execute_scalar(
            "WITH peers AS ("
            "SELECT contact_id AS id FROM contacts WHERE owner_id = $1::bigint "
            "), found AS ("
            "SELECT u.id, (extract(epoch from u.last_seen_at) * 1000)::bigint AS last_seen "
            "FROM peers JOIN users u ON u.id = peers.id "
            "WHERE u.id <> $1::bigint"
            ") "
            "SELECT COALESCE(string_agg(id::text || ':' || last_seen::text, ',' ORDER BY id), '') FROM found",
            std::move(parameters));
        auto& [query_ec, query_rows] = query_result;
        if (query_ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        rows = std::move(query_rows);
    }

    std::string users_json = "[";
    std::string_view remaining = rows;
    bool first_user = true;
    while (!remaining.empty())
    {
        auto const separator = remaining.find(',');
        auto const row = remaining.substr(0, separator);
        auto const colon = row.find(':');
        if (colon == std::string_view::npos)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }

        std::int64_t user = 0;
        std::int64_t last_seen = 0;
        auto const user_text = row.substr(0, colon);
        auto const last_seen_text = row.substr(colon + 1);
        auto const [user_end, user_error] =
            std::from_chars(user_text.data(), user_text.data() + user_text.size(), user);
        auto const [last_seen_end, last_seen_error] =
            std::from_chars(last_seen_text.data(), last_seen_text.data() + last_seen_text.size(), last_seen);
        if (user_error != std::errc{} || user_end != user_text.data() + user_text.size() || user <= 0 ||
            last_seen_error != std::errc{} || last_seen_end != last_seen_text.data() + last_seen_text.size() ||
            last_seen <= 0)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }

        if (!first_user)
        {
            users_json.push_back(',');
        }
        first_user = false;
        users_json.append(R"({"user":)");
        users_json.append(std::to_string(user));
        users_json.append(R"(,"online":)");
        users_json.append(users_.find(user) ? "true" : "false");
        users_json.append(R"(,"last_seen":)");
        users_json.append(std::to_string(last_seen));
        users_json.push_back('}');

        if (separator == std::string_view::npos)
        {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    users_json.push_back(']');

    co_return serialize_get_presence_result(users_json, std::move(request.id), response);
}
