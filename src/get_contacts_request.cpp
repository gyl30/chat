#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <string_view>

#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "friendship_store.hpp"
#include <chat/text.hpp>
#include <sqlite3.h>

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

    auto users = co_await run_db([&, user = *user_id_](sqlite_database& db) { return read_contacts(db, user); });
    if (!users)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    co_return serialize_json_rpc_success(*users, std::move(request.id), response);
}

namespace
{
struct[[= simdjson::deny_unknown_fields]] contact_settings_params
{
    std::optional<std::int64_t> user;
    std::optional<std::string> group;
    std::optional<std::string> name;
    std::optional<std::string> remark;
};

bool valid_label(std::string_view s)
{
    return s.find('\0') == std::string_view::npos && std::count_if(s.begin(), s.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }) <= 32;
}

std::optional<std::string_view> contact_settings_sql(std::string_view method, contact_settings_params const& p)
{
    if (p.user && *p.user > 0 && !p.name)
    {
        if (method == "set_friend_remark" && p.remark && !p.group && valid_label(*p.remark))
        {
            return "UPDATE contacts SET remark=?3 WHERE owner_id=?1 AND contact_id=?2";
        }
        if (method == "move_friend" && !p.remark && p.group)
        {
            return "UPDATE contacts SET friend_group=nullif(?3,'') WHERE owner_id=?1 AND contact_id=?2";
        }
    }
    if (p.user || p.remark)
    {
        return {};
    }
    if (p.name && (!valid_label(*p.name) || chat::text_is_blank(*p.name) || *p.name == "我的好友"))
    {
        return {};
    }
    if (method == "create_friend_group" && !p.group && p.name)
    {
        return "INSERT INTO friend_groups(owner_id,name) VALUES(?1,?3)";
    }
    if (!p.group || p.group->empty())
    {
        return {};
    }
    if (method == "rename_friend_group" && p.name)
    {
        return "UPDATE friend_groups SET name=?3 WHERE owner_id=?1 AND name=?2";
    }
    if (method == "delete_friend_group" && !p.name)
    {
        return "DELETE FROM friend_groups WHERE owner_id=?1 AND name=?2";
    }
    return {};
}
}    // namespace
boost::capy::task<simdjson::error_code> chat_session::handle_contact_settings(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    if (!user_id_)
    {
        co_return serialize_json_rpc_error(-32001, "Authentication required", std::move(request.id), response);
    }
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    contact_settings_params p;
    if (!request.params.present || parser.iterate(request.params.json).get(doc) || doc.get(p) || !doc.at_end())
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto sql = contact_settings_sql(request.method, p);
    if (!sql)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto result = co_await run_db(
        [&, owner = *user_id_](sqlite_database& db) -> std::expected<bool, sqlite_error>
        {
            sqlite_error error;
            if (p.user)
            {
                error = db.exec(*sql, owner, *p.user, p.remark ? *p.remark : *p.group);
            }
            else if (p.name)
            {
                error = db.exec(*sql, owner, p.group.value_or(""), *p.name);
            }
            else
            {
                error = db.exec(*sql, owner, *p.group);
            }
            if (error)
            {
                if ((error.code & 255) == SQLITE_CONSTRAINT)
                {
                    return false;
                }
                return std::unexpected(error);
            }
            return db.changes() == 1;
        });
    if (!result)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!*result)
    {
        co_return serialize_json_rpc_error(-32006, "Contact or group unavailable", std::move(request.id), response);
    }
    for (auto* session : users_.sessions(*user_id_))
    {
        session->enqueue_message(R"({"jsonrpc":"2.0","method":"contacts_changed","params":{}})");
    }
    co_return serialize_json_rpc_success(R"({"changed":true})", std::move(request.id), response);
}
