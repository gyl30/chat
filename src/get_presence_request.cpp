#include <utility>
#include <vector>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "user_store.hpp"

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

    auto presence = co_await run_db([user = *user_id_](sqlite_database& db) { return read_contact_presence(db, user); });
    if (!presence)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    for (auto& entry : *presence)
    {
        entry.online = !users_.sessions(entry.user).empty();
    }
    struct result
    {
        std::vector<chat::presence> users;
    };
    co_return serialize_json_rpc_value(result{std::move(*presence)}, std::move(request.id), response);
}
