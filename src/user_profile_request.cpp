#include "chat_session.hpp"
#include "database.hpp"
#include "user_store.hpp"
#include <chat/user.hpp>
#include <sqlite3.h>

namespace
{
struct[[= simdjson::deny_unknown_fields]] profile_target
{
    std::int64_t user = 0;
};
struct[[= simdjson::deny_unknown_fields]] profile_edit
{
    std::optional<std::string> nickname, signature;
};
}    // namespace
boost::capy::task<simdjson::error_code> chat_session::handle_user_profile(json_rpc_request& request, std::string& response)
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
    if (!request.params.present || parser.iterate(request.params.json).get(doc))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    if (request.method == "get_user_profile")
    {
        profile_target target;
        if (doc.get(target) || !doc.at_end() || target.user <= 0)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        auto profile = co_await run_db(
            [&, viewer = *user_id_](sqlite_database& db) -> std::expected<std::optional<chat::user_profile>, sqlite_error>
            {
                auto stmt = db.prepare(R"SQL(SELECT id,username,nickname,CASE WHEN id=?2 OR
                EXISTS(SELECT 1 FROM contacts WHERE owner_id=?2 AND contact_id=?1) OR
                EXISTS(SELECT 1 FROM conversation_members own JOIN conversation_members peer USING(conversation_id)
                JOIN conversations c ON c.id=own.conversation_id WHERE own.user_id=?2 AND peer.user_id=?1 AND c.kind='group')
                THEN signature ELSE '' END FROM users WHERE id=?1)SQL",
                                       target.user,
                                       viewer);
                if (!stmt)
                {
                    return std::unexpected(stmt.error());
                }
                auto row = stmt->step();
                if (!row)
                {
                    return std::unexpected(row.error());
                }
                if (!*row)
                {
                    return std::nullopt;
                }
                return chat::user_profile{
                    stmt->column_int64(0), std::string(stmt->column_text(1)), std::string(stmt->column_text(2)), std::string(stmt->column_text(3))};
            });
        if (!profile)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!*profile)
        {
            co_return serialize_json_rpc_error(-32006, "User unavailable", std::move(request.id), response);
        }
        co_return serialize_json_rpc_value(**profile, std::move(request.id), response);
    }
    profile_edit edit;
    if (doc.get(edit) || !doc.at_end() || !edit.nickname || !edit.signature || edit.nickname->find('\0') != std::string::npos ||
        edit.signature->find('\0') != std::string::npos)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto result = co_await run_db(
        [&, user = *user_id_](sqlite_database& db) -> std::expected<bool, sqlite_error>
        {
            if (auto err = db.exec("UPDATE users SET nickname=?2, signature=?3 WHERE id=?1", user, *edit.nickname, *edit.signature); err)
            {
                if ((err.code & 255) == SQLITE_CONSTRAINT)
                {
                    return false;
                }
                return std::unexpected(err);
            }
            return true;
        });
    if (!result)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!*result)
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    std::string event = R"({"jsonrpc":"2.0","method":"profile_changed","params":{"user":)" + std::to_string(*user_id_) + "}}";
    for (auto* session : users_.sessions(*user_id_))
    {
        session->enqueue_message(event);
    }
    auto audience = co_await run_db([user = *user_id_](sqlite_database& db) { return read_avatar_recipients(db, user); });
    if (audience)
    {
        for (auto peer : *audience)
        {
            for (auto* session : users_.sessions(peer))
            {
                session->enqueue_message(event);
            }
        }
    }
    co_return serialize_json_rpc_success(R"({"changed":true})", std::move(request.id), response);
}
