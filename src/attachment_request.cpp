#include "chat_session.hpp"
#include "attachment_access.hpp"
#include "attachment_io.hpp"
#include "conversation_store.hpp"
#include "database.hpp"
#include "s3_objects.hpp"
#include "timestamp.hpp"
#include <chat/attachment.hpp>
#include <chat/error.hpp>
#include <algorithm>

namespace
{
struct[[= simdjson::deny_unknown_fields]] begin_params
{
    std::int64_t conversation = 0, client_id = 0, size = -1;
    std::string filename, media_type, checksum;
};
struct[[= simdjson::deny_unknown_fields]] task_params
{
    std::int64_t upload = 0;
};
struct[[= simdjson::deny_unknown_fields]] confirm_params
{
    std::int64_t upload = 0;
    int number = 0;
    std::string etag;
};
struct[[= simdjson::deny_unknown_fields]] get_params
{
    std::int64_t conversation = 0, message = 0;
};
struct[[= simdjson::deny_unknown_fields]] group_download
{
    std::int64_t conversation_id = 0;
    std::string object_key;
};
struct completed_file
{
    std::string object_key;
};
struct grant
{
    std::int64_t upload, size, part_size, part_count, offset, length;
    int number;
    std::string url;
};
template <class... Args>
std::expected<bool, sqlite_error> write_one(sqlite_database& db, std::string_view sql, Args const&... args)
{
    if (auto e = db.exec(sql, args...); e)
    {
        return std::unexpected(e);
    }
    return db.changes() == 1;
}
}    // namespace

boost::capy::task<simdjson::error_code> chat_session::handle_attachment(json_rpc_request& request, std::string& response)
{
    if (!request.id.present)
    {
        co_return simdjson::SUCCESS;
    }
    auto fail = [&](int code, std::string_view msg) { return serialize_json_rpc_error(code, msg, std::move(request.id), response); };
    auto invalid = [&] { return serialize_json_rpc_invalid_params(std::move(request.id), response); };
    if (!user_id_)
    {
        co_return fail(-32001, "Authentication required");
    }
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document doc;
    if (!request.params.present || parser.iterate(request.params.json).get(doc))
    {
        co_return invalid();
    }
    auto const user = *user_id_;
    if (request.method == "get_attachment" || request.method == "get_conversation_file")
    {
        get_params p;
        group_download group;
        if (request.method == "get_conversation_file")
        {
            if (doc.get(group) || !doc.at_end() || group.conversation_id <= 0 || group.object_key.empty())
            {
                co_return invalid();
            }
            p.conversation = group.conversation_id;
        }
        else if (doc.get(p) || !doc.at_end() || p.conversation <= 0 || p.message <= 0)
        {
            co_return invalid();
        }
        auto a = co_await database_.run([&](sqlite_database& db)
                                        { return find_attachment_object(db, user, p.conversation, p.message, group.object_key); });
        if (!a)
        {
            co_return fail(-32000, "Server error");
        }
        if (!*a)
        {
            co_return fail(-32007, "Attachment unavailable");
        }
        auto url = co_await objects_.signed_url((**a).key);
        if (!url)
        {
            co_return fail(-32000, "Could not sign download");
        }
        // The signing work can yield. Do not grant a URL to a member whose
        // conversation or reference disappeared before this response.
        auto still_available = co_await database_.run([&](sqlite_database& db)
                                                      { return find_attachment_object(db, user, p.conversation, p.message, group.object_key); });
        if (!still_available)
        {
            co_return fail(-32000, "Server error");
        }
        if (!*still_available)
        {
            co_return fail(-32007, "Attachment unavailable");
        }
        struct download
        {
            std::string url, checksum;
            std::int64_t size;
        };
        co_return serialize_json_rpc_value(download{*url, (**a).checksum, (**a).size}, std::move(request.id), response);
    }
    if (device_.empty())
    {
        co_return fail(-32008, "Device authentication required");
    }
    upload_lookup task;
    confirm_params confirmation;
    bool const begin = request.method == "begin_attachment", confirm = request.method == "confirm_attachment";
    if (begin)
    {
        begin_params p;
        if (doc.get(p) || !doc.at_end() || p.conversation <= 0 || p.client_id <= 0 || p.size < 0 || p.size > chat::max_attachment_size ||
            p.filename.empty() || p.filename.size() > 255 || p.filename.find_first_of("/\\") != std::string::npos ||
            std::ranges::any_of(p.filename, [](unsigned char c) { return c < 32 || c == 127; }) ||
            (p.media_type != "application/octet-stream" && p.media_type != "image/png" && p.media_type != "image/jpeg") || p.checksum.size() != 64 ||
            p.checksum.find_first_not_of("0123456789abcdef") != std::string::npos)
        {
            co_return invalid();
        }
        auto sent = co_await find_sent_message(p.client_id);
        if (!sent)
        {
            co_return fail(-32000, "Server error");
        }
        if (*sent)
        {
            if ((**sent).conversation != p.conversation || (!(**sent).deleted && !(**sent).attachment))
            {
                co_return invalid();
            }
            if (!(**sent).member)
            {
                co_return fail(-32006, "Conversation unavailable");
            }
            co_return serialize_json_rpc_success("{\"message\":" + (**sent).json + "}", std::move(request.id), response);
        }
        auto access = co_await database_.run([&](sqlite_database& db) { return can_send_to_conversation(db, p.conversation, user); });
        if (!access)
        {
            co_return fail(-32000, "Server error");
        }
        if (*access != send_access::allowed)
        {
            co_return *access == send_access::muted ? fail(chat::group_speech_muted, "Group speech is muted")
                                                    : fail(-32006, "Conversation unavailable");
        }
        auto key = co_await database_.run([&](sqlite_database& db) { return completed_upload_key(db, user, device_, p.client_id); });
        if (!key)
        {
            co_return fail(-32000, "Server error");
        }
        if (!key->empty())
        {
            co_return serialize_json_rpc_value(completed_file{*key}, std::move(request.id), response);
        }
        upload_description input{p.conversation, p.client_id, p.size, p.filename, p.media_type, p.checksum};
        task = co_await database_.run([&](sqlite_database& db) { return begin_upload(db, user, device_, input); });
    }
    else
    {
        task_params p;
        if (confirm)
        {
            if (doc.get(confirmation) || !doc.at_end() || confirmation.number <= 0 || confirmation.etag.empty() || confirmation.etag.size() > 128)
            {
                co_return invalid();
            }
            p.upload = confirmation.upload;
        }
        else if (doc.get(p) || !doc.at_end())
        {
            co_return invalid();
        }
        if (p.upload <= 0)
        {
            co_return invalid();
        }
        task = co_await database_.run([&](sqlite_database& db) { return read_upload(db, user, device_, p.upload); });
        if (task && !*task && (request.method == "resume_attachment" || request.method == "complete_file"))
        {
            if (request.method == "resume_attachment")
            {
                auto sent = co_await find_sent_message(p.upload);
                if (!sent)
                {
                    co_return fail(-32000, "Server error");
                }
                if (*sent && (**sent).member && ((**sent).attachment || (**sent).deleted))
                {
                    co_return serialize_json_rpc_success("{\"message\":" + (**sent).json + "}", std::move(request.id), response);
                }
            }
            auto key = co_await database_.run([&](sqlite_database& db) { return completed_upload_key(db, user, device_, p.upload); });
            if (!key)
            {
                co_return fail(-32000, "Server error");
            }
            if (!key->empty())
            {
                co_return serialize_json_rpc_value(completed_file{*key}, std::move(request.id), response);
            }
        }
    }
    if (!task)
    {
        co_return fail(-32000, "Server error");
    }
    if (!*task)
    {
        co_return fail(-32008, "Upload unavailable");
    }
    auto& t = **task;
    bool const cancel = request.method == "cancel_attachment", pause = request.method == "pause_attachment",
               query = request.method == "query_attachment";
    if (cancel || pause)
    {
        auto saved = co_await database_.run(
            [&](sqlite_database& db)
            {
                return write_one(db,
                                 "UPDATE attachment_uploads SET state=?,touched=? WHERE id=? AND state<>'discarding'",
                                 cancel ? "discarding" : "paused",
                                 epoch_milliseconds(),
                                 t.id);
            });
        if (!saved)
        {
            co_return fail(-32000, "Server error");
        }
        if (!*saved)
        {
            co_return fail(-32008, "Upload unavailable");
        }
        if (cancel && !(co_await discard_upload(database_, objects_, t.id, t.key, t.multipart)))
        {
            co_return fail(-32000, "Abort pending");
        }
        co_return serialize_json_rpc_success(cancel ? "{\"cancelled\":true}" : "{\"paused\":true}", std::move(request.id), response);
    }
    auto allowed = co_await database_.run([&](sqlite_database& db) { return can_send_to_conversation(db, t.conversation, user); });
    if (!allowed)
    {
        co_return fail(-32000, "Server error");
    }
    if (*allowed != send_access::allowed)
    {
        co_return *allowed == send_access::muted ? fail(chat::group_speech_muted, "Group speech is muted") : fail(-32006, "Conversation unavailable");
    }
    if (request.method == "complete_file")
    {
        if (t.state != "active" || t.parts.empty() || t.parts.back().etag.empty())
        {
            co_return fail(-32008, "Attachment upload incomplete");
        }
        if (!(co_await objects_.complete(t)))
        {
            co_return fail(-32000, "Multipart completion failed");
        }
        auto registered = co_await database_.run(
            [&](sqlite_database& db) -> std::expected<send_access, sqlite_error>
            {
                auto tx = sqlite_transaction::begin_immediate(db);
                if (!tx)
                {
                    return std::unexpected(tx.error());
                }
                auto allowed_now = can_send_to_conversation(db, t.conversation, user);
                if (!allowed_now || *allowed_now != send_access::allowed)
                {
                    return allowed_now;
                }
                auto saved = register_uploaded_file(db, t.id);
                if (!saved)
                {
                    return std::unexpected(saved.error());
                }
                if (!*saved)
                {
                    return send_access::unavailable;
                }
                if (auto e = tx->commit(); e)
                {
                    return std::unexpected(e);
                }
                return send_access::allowed;
            });
        if (!registered)
        {
            co_return fail(-32000, "Server error");
        }
        if (*registered != send_access::allowed)
        {
            co_return *registered == send_access::muted ? fail(chat::group_speech_muted, "Group speech is muted")
                                                        : fail(-32008, "Upload unavailable");
        }
        co_return serialize_json_rpc_value(completed_file{t.key}, std::move(request.id), response);
    }
    if (confirm && t.state != "active")
    {
        co_return fail(-32008, "Upload paused");
    }
    auto part = std::ranges::find(t.parts, std::string{}, &attachment_part::etag);
    if (query)
    {
        struct status
        {
            std::string state;
            std::int64_t confirmed, size;
        };
        co_return serialize_json_rpc_value(status{t.state, part == t.parts.end() ? t.size : part->offset, t.size}, std::move(request.id), response);
    }
    auto touch = co_await database_.run(
        [&](sqlite_database& db)
        {
            return write_one(
                db, "UPDATE attachment_uploads SET touched=?,state='active' WHERE id=? AND state<>'discarding'", epoch_milliseconds(), t.id);
        });
    if (!touch)
    {
        co_return fail(-32000, "Server error");
    }
    if (!*touch)
    {
        co_return fail(-32008, "Upload unavailable");
    }
    if (t.multipart.empty())
    {
        auto id = co_await objects_.initiate(t);
        if (!id)
        {
            co_return fail(-32000, "Could not initiate multipart upload");
        }
        auto saved = co_await database_.run(
            [&](sqlite_database& db)
            { return write_one(db, "UPDATE attachment_uploads SET multipart=? WHERE id=? AND multipart='' AND state='active'", *id, t.id); });
        if (!saved || !*saved)
        {
            // A normal pause can retain the upload; a discarded one must not leak
            // an Initiate that finished after the group became unavailable.
            auto discarded = co_await database_.run(
                [&](sqlite_database& db)
                {
                    auto current = db.prepare("SELECT 1 FROM attachment_uploads WHERE id=? AND state<>'discarding'", t.id);
                    if (!current)
                    {
                        return std::expected<bool, sqlite_error>(std::unexpected(current.error()));
                    }
                    return current->step();
                });
            if (discarded && !*discarded)
            {
                co_await objects_.abort(t.key, *id);
            }
            co_return fail(-32000, "Multipart reservation failed");
        }
        t.multipart = std::move(*id);
    }
    if (confirm && (confirmation.number > static_cast<int>(t.parts.size()) || (part != t.parts.end() && confirmation.number > part->number)))
    {
        co_return invalid();
    }
    if (confirm && (part == t.parts.end() || confirmation.number < part->number) && t.parts[confirmation.number - 1].etag != confirmation.etag)
    {
        co_return invalid();
    }
    // A lost confirmation can leave an already uploaded part at the first gap.
    if (part != t.parts.end() && (!confirm || confirmation.number == part->number))
    {
        auto stored = co_await objects_.uploaded_part(t, part->number);
        if (!stored)
        {
            co_return fail(-32000, "Could not list multipart parts");
        }
        bool const matches = *stored && (**stored).first == part->length && !(**stored).second.empty();
        if (confirm && (!matches || confirmation.etag != (**stored).second))
        {
            co_return fail(-32008, "Part size or ETag mismatch");
        }
        if (matches)
        {
            auto saved = co_await database_.run(
                [&](sqlite_database& db)
                {
                    return write_one(db,
                                     "UPDATE attachment_parts SET etag=?,confirmed_at=? WHERE upload=? AND number=? AND etag IS NULL",
                                     (**stored).second,
                                     epoch_milliseconds(),
                                     t.id,
                                     part->number);
                });
            if (!saved || !*saved)
            {
                co_return fail(-32000, "Could not confirm part");
            }
            ++part;
        }
    }
    grant g{t.client_id, t.size, t.part_size, static_cast<std::int64_t>(t.parts.size()), t.size, 0, 0, {}};
    if (part != t.parts.end())
    {
        auto url = co_await objects_.signed_url(t.key, t.multipart, part->number);
        if (!url)
        {
            co_return fail(-32000, "Could not sign part");
        }
        g.number = part->number;
        g.offset = part->offset;
        g.length = part->length;
        g.url = std::move(*url);
    }
    co_return serialize_json_rpc_value(g, std::move(request.id), response);
}
