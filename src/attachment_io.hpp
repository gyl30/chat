#ifndef CHAT_SRC_ATTACHMENT_IO_HPP
#define CHAT_SRC_ATTACHMENT_IO_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <boost/capy/task.hpp>

class database;
class s3_objects;

struct staged_attachment
{
    std::string key;
    std::string checksum;
};

boost::capy::task<std::optional<staged_attachment>> stage_attachment(database& db, s3_objects& objects, std::string bytes, std::string media_type);
boost::capy::task<void> discard_unpublished_attachment(database& db, s3_objects& objects, std::string const& key);

struct attachment_chunk
{
    std::size_t size;
    std::string bytes;
};

enum class attachment_download_error
{
    unavailable,
    failed
};

boost::capy::task<std::expected<attachment_chunk, attachment_download_error>> download_attachment_chunk(
    database& db, s3_objects& objects, std::int64_t user, std::int64_t conversation, std::int64_t message, std::size_t offset);

// Run before admitting uploads. The cutoff excludes PUTs that may still be
// completing remotely after an interrupted connection.
// Returns the number removed, or -1 when a cleanup pass could not fully complete.
boost::capy::task<std::int64_t> recover_attachment_objects(database& db, s3_objects& objects, std::int64_t cutoff);

#endif
