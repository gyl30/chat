#include <charconv>
#include <cstdint>
#include <string>
#include <vector>

#include <openssl/rand.h>
#include <chat/avatar.hpp>
#include <chat/detail/base64.hpp>
#include <simdjson.h>

#include "chat_session.hpp"
#include "avatar_image.hpp"
#include "database.hpp"
#include "user_store.hpp"
#include "conversation_store.hpp"

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
    if (request.method == "get_avatar")
    {
        co_return co_await get_avatar_chunk(document, request, response);
    }
    if (request.method == "begin_avatar_upload")
    {
        struct[[= simdjson::deny_unknown_fields]] begin_avatar_params
        {
            std::int64_t size = 0;
            std::optional<std::int64_t> conversation;
        };
        begin_avatar_params params;
        if (document.get(params) || !document.at_end() || params.conversation.value_or(1) <= 0 || params.size <= 0 ||
            params.size > static_cast<std::int64_t>(chat::max_avatar_size))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        if (params.conversation)
        {
            auto role = co_await run_db([&, user = *user_id_](sqlite_database& db) { return read_group_role(db, *params.conversation, user); });
            if (!role)
            {
                co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
            }
            if (!*role || **role == chat::member_role::member)
            {
                co_return serialize_json_rpc_error(-32006, "Group unavailable", std::move(request.id), response);
            }
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
        avatar_upload_.emplace(id, params.size, std::string{}, params.conversation.value_or(0));
        co_return serialize_json_rpc_success("{\"upload\":" + std::to_string(id) + "}", std::move(request.id), response);
    }
    if (request.method == "upload_avatar_chunk" || request.method == "cancel_avatar_upload")
    {
        co_return update_avatar_upload(document, request, response);
    }

    auto const clearing = request.method == "clear_avatar";
    std::int64_t group = 0;
    std::string content;

    if (clearing)
    {
        struct[[= simdjson::deny_unknown_fields]] clear_params
        {
            std::optional<std::int64_t> conversation;
        };
        clear_params params;
        if (document.get(params) || !document.at_end() || params.conversation.value_or(1) <= 0)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        group = params.conversation.value_or(0);
        if (avatar_upload_ && group == avatar_upload_->group)
        {
            avatar_upload_.reset();
        }
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
        group = avatar_upload_->group;
        content = std::move(avatar_upload_->content);
        avatar_upload_.reset();
        if (!valid_avatar_png(content))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
    }
    auto changed =
        co_await run_db([&, user = *user_id_](sqlite_database& db) { return write_avatar(db, user, group, clearing ? "" : "image/png", content); });
    if (!changed)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!*changed)
    {
        co_return serialize_json_rpc_error(-32006, "Group unavailable", std::move(request.id), response);
    }
    auto const& change = **changed;
    std::string state =
        "{\"avatar_revision\":" + std::to_string(change.state.revision) + ",\"has_avatar\":" + (change.state.present ? "true}" : "false}");
    if (group && change.changed)
    {
        co_await publish_conversation(group, R"({"jsonrpc":"2.0","method":"conversation","params":{"conversation":)" + std::to_string(group) + "}}");
    }
    else if (!group && change.changed)
    {
        std::string notification =
            "{\"jsonrpc\":\"2.0\",\"method\":\"avatar\",\"params\":{\"user\":" + std::to_string(*user_id_) + "," + state.substr(1) + "}";
        for (auto* session : users_.sessions(*user_id_))
        {
            session->enqueue_message(notification);
        }
        auto recipients = co_await run_db([user = *user_id_](sqlite_database& db) { return read_avatar_recipients(db, user); });
        if (recipients)
        {
            for (auto const peer : *recipients)
            {
                for (auto* session : users_.sessions(peer))
                {
                    session->enqueue_message(notification);
                }
            }
        }
    }
    co_return serialize_json_rpc_success(state, std::move(request.id), response);
}

boost::capy::task<simdjson::error_code> chat_session::get_avatar_chunk(simdjson::ondemand::document& document,
                                                                       json_rpc_request& request,
                                                                       std::string& response)
{
    struct[[= simdjson::deny_unknown_fields]] get_avatar_params
    {
        std::optional<std::int64_t> user, conversation;
        std::int64_t revision = -1;
        std::int64_t offset = -1;
    };
    get_avatar_params params;
    if (document.get(params) || !document.at_end() || params.user.has_value() == params.conversation.has_value() || params.user.value_or(1) <= 0 ||
        params.conversation.value_or(1) <= 0 || params.revision < 0 || params.offset < 0 ||
        params.offset > static_cast<std::int64_t>(chat::max_avatar_size))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto image = co_await run_db(
        [&, viewer = *user_id_](sqlite_database& db)
        { return read_avatar_chunk(db, params.user.value_or(0), params.conversation.value_or(0), viewer, params.revision, params.offset); });
    if (!image)
    {
        co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
    }
    if (!*image)
    {
        co_return serialize_json_rpc_error(-32010, "Avatar unavailable", std::move(request.id), response);
    }
    struct avatar_result
    {
        std::int64_t user, conversation, avatar_revision;
        bool has_avatar;
        std::int64_t offset, size;
        std::string media_type, data;
        bool has_more;
    };
    auto const& chunk = **image;
    avatar_result value{params.user.value_or(0),
                        params.conversation.value_or(0),
                        chunk.state.revision,
                        chunk.state.present,
                        chunk.offset,
                        chunk.size,
                        chunk.media_type,
                        chat::detail::encode_base64(chunk.bytes),
                        chunk.has_more};
    std::string payload;
    if (auto error = simdjson::builder::to_json_string(value).get(payload))
    {
        co_return error;
    }
    co_return serialize_json_rpc_success(payload, std::move(request.id), response);
}

simdjson::error_code chat_session::update_avatar_upload(simdjson::ondemand::document& document, json_rpc_request& request, std::string& response)
{
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
            params.data.size() > 4 * ((chat::avatar_chunk_size + 2) / 3))
        {
            return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        if (!avatar_upload_ || avatar_upload_->id != params.upload)
        {
            return serialize_json_rpc_error(-32011, "Avatar upload unavailable", std::move(request.id), response);
        }
        auto bytes = chat::detail::decode_base64(params.data);
        if (!bytes || bytes->empty() || bytes->size() > chat::avatar_chunk_size ||
            params.offset != static_cast<std::int64_t>(avatar_upload_->content.size()) ||
            avatar_upload_->content.size() + bytes->size() > static_cast<std::size_t>(avatar_upload_->size))
        {
            return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        avatar_upload_->content.append(*bytes);
        return serialize_json_rpc_success("{\"offset\":" + std::to_string(avatar_upload_->content.size()) + "}", std::move(request.id), response);
    }
    struct[[= simdjson::deny_unknown_fields]] cancel_avatar_params
    {
        std::int64_t upload = 0;
    };
    cancel_avatar_params params;
    if (document.get(params) || !document.at_end() || params.upload <= 0)
    {
        return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    auto const cancelled = avatar_upload_ && avatar_upload_->id == params.upload;
    if (cancelled)
    {
        avatar_upload_.reset();
    }
    return serialize_json_rpc_success(cancelled ? "{\"cancelled\":true}" : "{\"cancelled\":false}", std::move(request.id), response);
}
