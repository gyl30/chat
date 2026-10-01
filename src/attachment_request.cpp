#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include <chat/attachment.hpp>
#include <chat/detail/base64.hpp>
#include <simdjson.h>

#include "chat_session.hpp"
#include "pg_connection_pool.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_attachment(json_rpc_request& request,
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
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (!request.params.present || parser.iterate(request.params.json).get(document))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    if (request.method == "begin_attachment")
    {
        struct [[= simdjson::deny_unknown_fields]] begin_params
        {
            std::int64_t conversation = 0;
            std::string filename;
            std::int64_t size = -1;
        };
        begin_params params;
        if (document.get(params) || !document.at_end() || params.conversation <= 0 || params.size < 0 ||
            params.size > static_cast<std::int64_t>(chat::max_attachment_size) || params.filename.empty() ||
            params.filename.size() > 255 || params.filename.find_first_of("/\\") != std::string::npos ||
            std::ranges::any_of(params.filename, [](unsigned char value) { return value < 32 || value == 127; }))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        if (upload_)
        {
            co_return serialize_json_rpc_error(-32008, "Attachment upload in progress", std::move(request.id), response);
        }
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto& connection = lease.connection();
        auto begun = co_await connection.execute_row("BEGIN");
        auto locked = co_await connection.execute_row(
            "SELECT id::text FROM conversations WHERE id=$1::bigint FOR UPDATE", {std::to_string(params.conversation)});
        if (std::get<0>(begun) || std::get<0>(locked))
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto result = co_await connection.execute_row(
            "SELECT user_id::text FROM conversation_members WHERE conversation_id=$2::bigint AND user_id=$1::bigint",
            {std::to_string(*user_id_), std::to_string(params.conversation)});
        auto& [ec, member] = result;
        if (ec)
        {
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto const upload = next_upload_id_;
        if (member)
        {
            upload_.emplace(next_upload_id_++, params.conversation, std::move(params.filename), params.size, std::string{});
        }
        auto ended = co_await connection.execute_row(member ? "COMMIT" : "ROLLBACK");
        if (std::get<0>(ended))
        {
            upload_.reset();
            connection.close();
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!member)
        {
            co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
        }
        co_return serialize_json_rpc_success("{\"upload\":" + std::to_string(upload) + "}",
                                            std::move(request.id), response);
    }
    if (request.method == "get_attachment")
    {
        struct [[= simdjson::deny_unknown_fields]] get_params
        {
            std::int64_t conversation = 0;
            std::int64_t message = 0;
            std::int64_t offset = 0;
        };
        get_params params;
        if (document.get(params) || !document.at_end() || params.conversation <= 0 || params.message <= 0 ||
            params.offset < 0 || params.offset > static_cast<std::int64_t>(chat::max_attachment_size))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        auto lease = co_await database_.acquire();
        if (lease.error())
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        auto result = co_await lease.connection().execute_row(
            "SELECT json_build_object('offset',$3::bigint,'size',a.size,'data',"
            "replace(encode(substring(a.data FROM $3::int+1 FOR 32768),'base64'),chr(10),''),"
            "'has_more',$3::bigint+32768<a.size)::text "
            "FROM message_attachments a JOIN messages m ON m.id=a.message_id JOIN conversation_members own "
            "ON own.conversation_id=m.conversation_id WHERE m.id=$2::bigint AND m.conversation_id=$4::bigint "
            "AND own.user_id=$1::bigint AND NOT m.deleted AND $3::bigint<=a.size",
            {std::to_string(*user_id_), std::to_string(params.message), std::to_string(params.offset),
             std::to_string(params.conversation)});
        auto& [ec, row] = result;
        if (ec)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!row)
        {
            co_return serialize_json_rpc_error(-32007, "Attachment unavailable", std::move(request.id), response);
        }
        co_return serialize_json_rpc_success(row->front(), std::move(request.id), response);
    }
    if (request.method == "cancel_attachment")
    {
        struct [[= simdjson::deny_unknown_fields]] cancel_params
        {
            std::int64_t upload = 0;
        };
        cancel_params params;
        if (document.get(params) || !document.at_end() || params.upload <= 0)
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        bool const cancelled = upload_ && upload_->id == params.upload;
        if (cancelled)
        {
            upload_.reset();
        }
        co_return serialize_json_rpc_success(cancelled ? "{\"cancelled\":true}" : "{\"cancelled\":false}",
                                            std::move(request.id), response);
    }
    struct [[= simdjson::deny_unknown_fields]] chunk_params
    {
        std::int64_t upload = 0;
        std::int64_t offset = -1;
        std::string data;
    };
    chunk_params params;
    if (document.get(params) || !document.at_end() || params.upload <= 0 || params.offset < 0 || params.data.empty() ||
        params.data.size() > 4 * ((chat::attachment_chunk_size + 2) / 3))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    if (!upload_ || upload_->id != params.upload)
    {
        co_return serialize_json_rpc_error(-32008, "Attachment upload unavailable", std::move(request.id), response);
    }
    auto bytes = chat::detail::decode_base64(params.data);
    if (!bytes || bytes->empty() || bytes->size() > chat::attachment_chunk_size ||
        params.offset != static_cast<std::int64_t>(upload_->content.size()) ||
        upload_->content.size() + bytes->size() > static_cast<std::size_t>(upload_->size))
    {
        co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
    }
    upload_->content.append(*bytes);
    co_return serialize_json_rpc_success("{\"offset\":" + std::to_string(upload_->content.size()) + "}",
                                        std::move(request.id), response);
}
