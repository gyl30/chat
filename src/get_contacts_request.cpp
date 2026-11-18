#include <string>
#include <utility>
#include <vector>
#include <string_view>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "friendship_store.hpp"
#include "user_payload.hpp"

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

    auto users = co_await database_.run([&, user=*user_id_](sqlite_database& db) {
        return read_contacts(db, user);
    });
    if (!users)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    struct result { std::vector<user_payload> users; };
    result payload;
    payload.users.reserve(users->size());
    for (auto const& user : *users)
    {
        payload.users.emplace_back(user);
    }
    co_return serialize_json_rpc_value(payload, std::move(request.id), response);
}
