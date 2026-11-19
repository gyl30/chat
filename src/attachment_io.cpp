#include "attachment_io.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include <chat/attachment.hpp>

#include "attachment_access.hpp"
#include "attachment_cleanup.hpp"
#include "database.hpp"
#include "s3_objects.hpp"

// A failed SQLite publication may have left an S3 object without a message.
// Check the durable reservation before deleting: a successful commit claims it.
boost::capy::task<void> discard_unpublished_attachment(database& db, s3_objects& objects, std::string const& key)
{
    auto reserved = co_await db.run([&](sqlite_database& connection) -> std::expected<bool, sqlite_error> {
        auto query = connection.prepare("SELECT 1 FROM attachment_cleanup WHERE object_key=?1", key);
        if (!query)
        {
            return std::unexpected(std::move(query.error()));
        }
        return query->step();
    });
    if (!reserved || !*reserved)
    {
        co_return;
    }
    if (auto removed = co_await objects.remove(key); !removed)
    {
        co_return;
    }
    // Keep the reservation for startup/shutdown recovery if acknowledgement fails.
    co_await db.run([&](sqlite_database& connection) { return acknowledge_attachment_cleanup(connection, key); });
}

boost::capy::task<std::optional<staged_attachment>> stage_attachment(
    database& db, s3_objects& objects, std::string bytes, std::string media_type)
{
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
    {
        spdlog::warn("attachment object key generation failed");
        co_return std::nullopt;
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
        return reserve_attachment_object(connection, key, now);
    };
    if (auto reserved = co_await db.run(reserve); !reserved)
    {
        spdlog::warn("attachment reservation failed (SQLite {})", reserved.error().code);
        co_return std::nullopt;
    }

    auto checksum = co_await objects.put(key, std::move(bytes), std::move(media_type));
    if (!checksum)
    {
        // A timed-out PUT can still complete remotely. Keep its reservation
        // for the delayed recovery pass rather than deleting it immediately.
        spdlog::warn("attachment upload failed: {} (S3 status {})", checksum.error().reason, checksum.error().status);
        co_return std::nullopt;
    }
    co_return staged_attachment{std::move(key), std::move(*checksum)};
}

boost::capy::task<std::expected<attachment_chunk, attachment_download_error>> download_attachment_chunk(
    database& db, s3_objects& objects, std::int64_t user, std::int64_t conversation, std::int64_t message,
    std::size_t offset)
{
    auto resolve = [=](sqlite_database& connection) {
        return find_attachment_object(connection, user, conversation, message);
    };
    auto object = co_await db.run(resolve);
    if (!object)
    {
        spdlog::warn("attachment lookup failed (SQLite {})", object.error().code);
        co_return std::unexpected(attachment_download_error::failed);
    }
    if (!*object || offset > static_cast<std::size_t>((*object)->size))
    {
        co_return std::unexpected(attachment_download_error::unavailable);
    }
    auto const size = static_cast<std::size_t>((*object)->size);
    if (offset == size)
    {
        co_return attachment_chunk{size, {}};
    }
    auto bytes = co_await objects.read(std::move((*object)->key), size, offset,
                                       std::min(chat::attachment_chunk_size, size - offset));
    if (!bytes)
    {
        spdlog::warn("attachment download failed: {} (S3 status {})", bytes.error().reason, bytes.error().status);
        co_return std::unexpected(attachment_download_error::failed);
    }
    co_return attachment_chunk{size, std::move(*bytes)};
}

boost::capy::task<std::int64_t> recover_attachment_objects(
    database& db, s3_objects& objects, std::int64_t cutoff)
{
    std::int64_t removed = 0;
    std::string after;
    bool removal_failed = false;
    for (;;)
    {
        auto claim_batch = [&](sqlite_database& connection) {
            return claim_attachment_cleanup_batch(connection, cutoff, after);
        };
        auto batch = co_await db.run(claim_batch);
        if (!batch)
        {
            co_return -1;
        }
        if (batch->empty())
        {
            co_return removal_failed ? -1 : removed;
        }
        after = *std::ranges::max_element(*batch);

        for (auto const& key : *batch)
        {
            if (auto result = co_await objects.remove(key); !result)
            {
                removal_failed = true;
                continue;
            }

            auto acknowledge = [&key](sqlite_database& connection) {
                return acknowledge_attachment_cleanup(connection, key);
            };
            if (auto result = co_await db.run(acknowledge); !result)
            {
                co_return -1;
            }
            ++removed;
        }
    }
}
