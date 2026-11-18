#include "attachment_io.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#include <openssl/rand.h>

#include <chat/attachment.hpp>

#include "attachment_access.hpp"
#include "attachment_cleanup.hpp"
#include "database.hpp"

boost::capy::task<std::expected<std::variant<message_write, message_write_rejection>, attachment_io_error>>
publish_attachment(database& db, s3_objects& objects, completed_attachment_upload upload)
{
    if (upload.bytes.size() > chat::max_attachment_size)
    {
        co_return std::unexpected(attachment_io_error{s3_error{0, "attachment exceeds the size limit"}});
    }

    auto const preflight = [&upload](sqlite_database& connection) {
        auto snapshot = sqlite_transaction::begin_read(connection);
        if (!snapshot)
        {
            return std::expected<std::optional<message_write_outcome>, sqlite_error>(
                std::unexpected(std::move(snapshot.error())));
        }
        return check_message_write(connection,
            {upload.sender, upload.conversation, upload.client_id, upload.filename, upload.reply_to}, true);
    };
    auto checked = co_await db.run(preflight);
    if (!checked)
    {
        co_return std::unexpected(attachment_io_error{std::move(checked.error())});
    }
    if (*checked)
    {
        co_return **checked;
    }

    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
    {
        co_return std::unexpected(attachment_io_error{s3_error{0, "cannot generate attachment key"}});
    }
    constexpr std::string_view hex = "0123456789abcdef";
    std::string key = "attachments/";
    for (auto const byte : random)
    {
        key.push_back(hex[byte >> 4]);
        key.push_back(hex[byte & 0x0f]);
    }
    auto reserve = [&key](sqlite_database& connection) -> std::expected<void, sqlite_error> {
        auto const now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (auto error = reserve_attachment_object(connection, key, now); error)
        {
            return std::unexpected(std::move(error));
        }
        return {};
    };
    if (auto reserved = co_await db.run(reserve); !reserved)
    {
        co_return std::unexpected(attachment_io_error{std::move(reserved.error())});
    }

    auto const size = static_cast<std::int64_t>(upload.bytes.size());
    auto checksum = co_await objects.put(key, std::move(upload.bytes), upload.media_type);
    if (!checksum)
    {
        co_return std::unexpected(attachment_io_error{std::move(checksum.error())});
    }

    auto commit = [&](sqlite_database& connection) {
        uploaded_attachment const metadata{key, upload.filename, upload.media_type, size, *checksum};
        return write_message(connection,
            {upload.sender, upload.conversation, upload.client_id, upload.filename, upload.reply_to}, &metadata);
    };
    auto committed = co_await db.run(commit);
    if (!committed)
    {
        co_return std::unexpected(attachment_io_error{std::move(committed.error())});
    }
    co_return std::move(*committed);
}

boost::capy::task<std::expected<std::optional<attachment_chunk>, attachment_io_error>> download_attachment_chunk(
    database& db, s3_objects& objects, std::int64_t user, std::int64_t conversation, std::int64_t message,
    std::size_t offset)
{
    auto resolve = [=](sqlite_database& connection) {
        return find_attachment_object(connection, user, conversation, message);
    };
    auto object = co_await db.run(resolve);
    if (!object)
    {
        co_return std::unexpected(attachment_io_error{std::move(object.error())});
    }
    if (!*object || offset > static_cast<std::size_t>((*object)->size))
    {
        co_return std::nullopt;
    }
    auto const size = static_cast<std::size_t>((*object)->size);
    auto bytes = co_await objects.read(std::move((*object)->key), size, offset,
                                       std::min(chat::attachment_chunk_size, size - offset));
    if (!bytes)
    {
        co_return std::unexpected(attachment_io_error{std::move(bytes.error())});
    }
    co_return attachment_chunk{size, std::move(*bytes)};
}

boost::capy::task<std::expected<std::size_t, attachment_io_error>> recover_attachment_objects(
    database& db, s3_objects& objects, std::int64_t cutoff)
{
    std::size_t removed = 0;
    for (;;)
    {
        auto read_batch = [cutoff](sqlite_database& connection) {
            return attachment_cleanup_batch(connection, cutoff);
        };
        auto batch = co_await db.run(read_batch);
        if (!batch)
        {
            co_return std::unexpected(attachment_io_error{std::move(batch.error())});
        }
        if (batch->empty())
        {
            co_return removed;
        }

        for (auto const& key : *batch)
        {
            if (auto result = co_await objects.remove(key); !result)
            {
                co_return std::unexpected(attachment_io_error{std::move(result.error())});
            }

            auto acknowledge = [&key](sqlite_database& connection) -> std::expected<void, sqlite_error> {
                if (auto error = acknowledge_attachment_cleanup(connection, key); error)
                {
                    return std::unexpected(std::move(error));
                }
                return {};
            };
            if (auto result = co_await db.run(acknowledge); !result)
            {
                co_return std::unexpected(attachment_io_error{std::move(result.error())});
            }
            ++removed;
        }
    }
}
