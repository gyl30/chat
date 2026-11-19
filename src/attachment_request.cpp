#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include <chat/attachment.hpp>
#include <chat/detail/base64.hpp>
#include <simdjson.h>

#include "chat_session.hpp"
#include "database.hpp"
#include "attachment_io.hpp"
#include "conversation_store.hpp"

boost::capy::task<simdjson::error_code> chat_session::handle_attachment(json_rpc_request& request, std::string& response)
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
        struct[[= simdjson::deny_unknown_fields]] begin_params
        {
            std::int64_t conversation = 0;
            std::string filename;
            std::int64_t size = -1;
            std::int64_t client_id = 0;
        };
        begin_params params;
        if (document.get(params) || !document.at_end() || params.conversation <= 0 || params.client_id <= 0 || params.size < 0 ||
            params.size > static_cast<std::int64_t>(chat::max_attachment_size) || params.filename.empty() || params.filename.size() > 255 ||
            params.filename.find_first_of("/\\") != std::string::npos ||
            std::ranges::any_of(params.filename, [](unsigned char value) { return value < 32 || value == 127; }))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        // An attachment already stored under this client_id is returned instead of uploaded again.
        auto sent = co_await find_sent_message(params.client_id);
        if (!sent)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (*sent)
        {
            if ((*sent)->conversation != params.conversation || (!(*sent)->deleted && !(*sent)->attachment))
            {
                co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
            }
            if (!(*sent)->member)
            {
                co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
            }
            co_return serialize_json_rpc_success("{\"message\":" + (*sent)->json + "}", std::move(request.id), response);
        }
        if (upload_)
        {
            co_return serialize_json_rpc_error(-32008, "Attachment upload in progress", std::move(request.id), response);
        }
        auto allowed =
            co_await database_.run([=, user = *user_id_](sqlite_database& db) { return can_send_to_conversation(db, params.conversation, user); });
        if (!allowed)
        {
            co_return serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        if (!*allowed)
        {
            co_return serialize_json_rpc_error(-32006, "Conversation unavailable", std::move(request.id), response);
        }
        auto const upload = next_upload_id_++;
        upload_.emplace(upload, params.conversation, params.client_id, std::move(params.filename), params.size, std::string{});
        co_return serialize_json_rpc_success("{\"upload\":" + std::to_string(upload) + "}", std::move(request.id), response);
    }
    if (request.method == "get_attachment")
    {
        struct[[= simdjson::deny_unknown_fields]] get_params
        {
            std::int64_t conversation = 0;
            std::int64_t message = 0;
            std::int64_t offset = 0;
        };
        get_params params;
        if (document.get(params) || !document.at_end() || params.conversation <= 0 || params.message <= 0 || params.offset < 0 ||
            params.offset > static_cast<std::int64_t>(chat::max_attachment_size))
        {
            co_return serialize_json_rpc_invalid_params(std::move(request.id), response);
        }
        auto const offset = static_cast<std::size_t>(params.offset);
        auto result = co_await download_attachment_chunk(database_, objects_, *user_id_, params.conversation, params.message, offset);
        if (!result)
        {
            co_return result.error() == attachment_download_error::unavailable
                ? serialize_json_rpc_error(-32007, "Attachment unavailable", std::move(request.id), response)
                : serialize_json_rpc_error(-32000, "Server error", std::move(request.id), response);
        }
        struct chunk
        {
            std::size_t offset, size;
            std::string data;
            bool has_more;
        };
        auto const& bytes = *result;
        chunk value{offset, bytes.size, chat::detail::encode_base64(bytes.bytes), bytes.size - offset > bytes.bytes.size()};
        co_return serialize_json_rpc_value(value, std::move(request.id), response);
    }
    if (request.method == "cancel_attachment")
    {
        struct[[= simdjson::deny_unknown_fields]] cancel_params
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
        co_return serialize_json_rpc_success(cancelled ? "{\"cancelled\":true}" : "{\"cancelled\":false}", std::move(request.id), response);
    }
    struct[[= simdjson::deny_unknown_fields]] chunk_params
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
    co_return serialize_json_rpc_success("{\"offset\":" + std::to_string(upload_->content.size()) + "}", std::move(request.id), response);
}
