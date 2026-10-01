#include <charconv>
#include <cstdint>
#include <string>
#include <vector>

#include <openssl/rand.h>
#include <chat/avatar.hpp>
#include <chat/attachment.hpp>
#include <chat/detail/base64.hpp>
#include <simdjson.h>

#include "chat_session.hpp"
#include "avatar_image.hpp"
#include "pg_connection_pool.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_avatar(json_rpc_request& request, std::string& response)
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
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    if (request.method == "begin_avatar_upload")
    {
        struct[[= simdjson::deny_unknown_fields]] begin_avatar_params
        {
            std::int64_t size = 0;
        };
        begin_avatar_params params;
        if (document.get(params) || !document.at_end() || params.size <= 0 || params.size > static_cast<std::int64_t>(chat::max_avatar_size))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        if (avatar_upload_)
        {
            co_return serialize_json_rpc_error(-32011, "Avatar upload in progress", std::move(request.id), response);
        }
        std::uint64_t random = 0;
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&random), sizeof(random)) != 1)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto const id = static_cast<std::int64_t>((random & 0x7fffffffffffffffULL) | 1);
        avatar_upload_.emplace(id, params.size, std::string{});
        co_return serialize_json_rpc_success("{\"upload\":" + std::to_string(id) + "}", std::move(request.id), response);
    }
    if (request.method == "upload_avatar_chunk")
    {
        struct[[= simdjson::deny_unknown_fields]] avatar_chunk_params
        {
            std::int64_t upload = 0;
            std::int64_t offset = -1;
            std::string data;
        };
        avatar_chunk_params params;
        if (document.get(params) || !document.at_end() || params.upload <= 0 || params.offset < 0 || params.data.empty() ||
            params.data.size() > 4 * ((chat::attachment_chunk_size + 2) / 3))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        if (!avatar_upload_ || avatar_upload_->id != params.upload)
        {
            co_return serialize_json_rpc_error(-32011, "Avatar upload unavailable", std::move(request.id), response);
        }
        auto bytes = chat::detail::decode_base64(params.data);
        if (!bytes || bytes->empty() || bytes->size() > chat::attachment_chunk_size ||
            params.offset != static_cast<std::int64_t>(avatar_upload_->content.size()) ||
            avatar_upload_->content.size() + bytes->size() > static_cast<std::size_t>(avatar_upload_->size))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        avatar_upload_->content.append(*bytes);
        co_return serialize_json_rpc_success("{\"offset\":" + std::to_string(avatar_upload_->content.size()) + "}", std::move(request.id), response);
    }
    if (request.method == "get_avatar")
    {
        struct[[= simdjson::deny_unknown_fields]] get_avatar_params
        {
            std::int64_t user = 0;
            std::int64_t revision = -1;
            std::int64_t offset = -1;
        };
        get_avatar_params params;
        if (document.get(params) || !document.at_end() || params.user <= 0 || params.revision < 0 || params.offset < 0 ||
            params.offset > static_cast<std::int64_t>(chat::max_avatar_size))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto result = co_await lease.connection().execute_row(
            "SELECT json_build_object('user',u.id,'avatar_revision',u.avatar_revision,'has_avatar',a.user_id IS NOT NULL,"
            "'offset',CASE WHEN u.avatar_revision=$2::bigint AND a.user_id IS NOT NULL THEN $3::bigint ELSE 0 END,"
            "'size',CASE WHEN u.avatar_revision=$2::bigint THEN COALESCE(a.size,0) ELSE 0 END,"
            "'media_type',CASE WHEN u.avatar_revision=$2::bigint THEN COALESCE(a.media_type,'') ELSE '' END,"
            "'data',CASE WHEN u.avatar_revision=$2::bigint THEN COALESCE("
            "replace(encode(substring(a.data FROM $3::int+1 FOR 32768),'base64'),chr(10),''),'') ELSE '' END,"
            "'has_more',u.avatar_revision=$2::bigint AND $3::bigint+32768<COALESCE(a.size,0))::text "
            "FROM users u LEFT JOIN user_avatars a ON a.user_id=u.id WHERE u.id=$1::bigint "
            "AND (u.avatar_revision<>$2::bigint OR $3::bigint<=COALESCE(a.size,0))",
            {std::to_string(params.user), std::to_string(params.revision), std::to_string(params.offset)});
        auto& [ec, row] = result;
        if (ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!row)
        {
            co_return serialize_json_rpc_error(-32010, "Avatar unavailable", std::move(request.id), response);
        }
        co_return serialize_json_rpc_success(row->front(), std::move(request.id), response);
    }
    if (request.method == "cancel_avatar_upload")
    {
        struct[[= simdjson::deny_unknown_fields]] cancel_avatar_params
        {
            std::int64_t upload = 0;
        };
        cancel_avatar_params params;
        if (document.get(params) || !document.at_end() || params.upload <= 0)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        auto const cancelled = avatar_upload_ && avatar_upload_->id == params.upload;
        if (cancelled)
        {
            avatar_upload_.reset();
        }
        co_return serialize_json_rpc_success(cancelled ? "{\"cancelled\":true}" : "{\"cancelled\":false}", std::move(request.id), response);
    }

    auto const clearing = request.method == "clear_avatar";
    std::string content;
    std::string media_type;
    if (clearing)
    {
        simdjson::ondemand::object params;
        if (document.get_object().get(params) || params.begin() != params.end() || !document.at_end())
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        avatar_upload_.reset();
    }
    else
    {
        struct[[= simdjson::deny_unknown_fields]] finish_avatar_params
        {
            std::int64_t upload = 0;
        };
        finish_avatar_params params;
        if (document.get(params) || !document.at_end() || params.upload <= 0)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        if (!avatar_upload_ || avatar_upload_->id != params.upload ||
            avatar_upload_->content.size() != static_cast<std::size_t>(avatar_upload_->size))
        {
            co_return serialize_json_rpc_error(-32011, "Avatar upload unavailable or incomplete", std::move(request.id), response);
        }
        content = std::move(avatar_upload_->content);
        avatar_upload_.reset();
        media_type = avatar_media_type(content);
        if (media_type.empty())
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
    }
    auto lease = co_await database_.acquire();
    if (lease.error())
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto& connection = lease.connection();
    auto begun = co_await connection.execute_row("BEGIN");
    if (std::get<0>(begun))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto revised = co_await connection.execute_scalar(
        "UPDATE users SET avatar_revision=avatar_revision+1 WHERE id=$1::bigint RETURNING avatar_revision::text", {std::to_string(*user_id_)});
    auto& [revision_ec, revision] = revised;
    if (revision_ec)
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    std::string query = clearing ? "DELETE FROM user_avatars WHERE user_id=$1::bigint"
                                 : "INSERT INTO user_avatars(user_id,media_type,size,data) VALUES($1::bigint,$2,$3::bigint,decode($4,'base64')) "
                                   "ON CONFLICT(user_id) DO UPDATE SET media_type=EXCLUDED.media_type,size=EXCLUDED.size,data=EXCLUDED.data,"
                                   "updated_at=CURRENT_TIMESTAMP";
    std::vector<std::string> values{std::to_string(*user_id_)};
    if (!clearing)
    {
        values.insert(values.end(), {media_type, std::to_string(content.size()), chat::detail::encode_base64(content)});
    }
    auto written = co_await connection.execute_row(std::move(query), std::move(values));
    if (std::get<0>(written))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    auto committed = co_await connection.execute_row("COMMIT");
    if (std::get<0>(committed))
    {
        connection.close();
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    std::string state = "{\"avatar_revision\":" + revision + ",\"has_avatar\":" + (clearing ? "false}" : "true}");
    std::string notification =
        "{\"jsonrpc\":\"2.0\",\"method\":\"avatar\",\"params\":{\"user\":" + std::to_string(*user_id_) + "," + state.substr(1) + "}";
    enqueue_message(notification);
    auto watchers_result = co_await connection.execute_scalar(
        "SELECT COALESCE(string_agg(peer::text,',' ORDER BY peer),'') FROM ("
        "SELECT CASE WHEN owner_id=$1::bigint THEN contact_id ELSE owner_id END AS peer FROM contacts "
        "WHERE owner_id=$1::bigint OR contact_id=$1::bigint UNION "
        "SELECT CASE WHEN direct_user_low=$1::bigint THEN direct_user_high ELSE direct_user_low END "
        "FROM conversations WHERE kind='direct' AND (direct_user_low=$1::bigint OR direct_user_high=$1::bigint) UNION "
        "SELECT m.user_id FROM conversation_members own JOIN conversation_members m ON m.conversation_id=own.conversation_id "
        "JOIN conversations c ON c.id=own.conversation_id WHERE own.user_id=$1::bigint AND c.kind='group'"
        ") peers WHERE peer<>$1::bigint",
        {std::to_string(*user_id_)});
    auto& [watchers_ec, watchers] = watchers_result;
    if (!watchers_ec)
    {
        std::string_view remaining = watchers;
        while (!remaining.empty())
        {
            auto const separator = remaining.find(',');
            auto token = remaining.substr(0, separator);
            std::int64_t peer = 0;
            auto const [end, ec] = std::from_chars(token.data(), token.data() + token.size(), peer);
            if (ec == std::errc{} && end == token.data() + token.size())
            {
                if (auto* session = users_.find(peer))
                {
                    session->enqueue_message(notification);
                }
            }
            if (separator == std::string_view::npos)
            {
                break;
            }
            remaining.remove_prefix(separator + 1);
        }
    }
    co_return serialize_json_rpc_success(state, std::move(request.id), response);
}
