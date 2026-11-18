#ifndef CHAT_SRC_ATTACHMENT_IO_HPP
#define CHAT_SRC_ATTACHMENT_IO_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>

#include <boost/capy/task.hpp>

#include "message_store.hpp"
#include "s3_objects.hpp"

class database;

using attachment_io_error = std::variant<sqlite_error, s3_error>;

struct completed_attachment_upload
{
    std::int64_t sender;
    std::int64_t conversation;
    std::int64_t client_id;
    std::optional<std::int64_t> reply_to;
    std::string filename;
    std::string media_type;
    std::string bytes;
};

boost::capy::task<std::expected<std::variant<message_write, message_write_rejection>, attachment_io_error>>
publish_attachment(database& db, s3_objects& objects, completed_attachment_upload upload);

struct attachment_chunk
{
    std::size_t size;
    std::string bytes;
};

// Absent result means unavailable or an invalid offset. Object keys remain private.
boost::capy::task<std::expected<std::optional<attachment_chunk>, attachment_io_error>> download_attachment_chunk(
    database& db, s3_objects& objects, std::int64_t user, std::int64_t conversation, std::int64_t message,
    std::size_t offset);

// Run before admitting uploads. The cutoff excludes PUTs that may still be
// completing remotely after an interrupted connection.
// Returns the number removed, or -1 when a cleanup pass could not fully complete.
boost::capy::task<std::int64_t> recover_attachment_objects(
    database& db, s3_objects& objects, std::int64_t cutoff);

#endif
