#ifndef CHAT_SRC_UPLOAD_STORE_HPP
#define CHAT_SRC_UPLOAD_STORE_HPP
#include "sqlite_database.hpp"
#include <vector>

struct upload_description
{
    std::int64_t conversation = 0, client_id = 0, size = -1;
    std::string filename, media_type, checksum;
};
struct attachment_part
{
    int number;
    std::int64_t offset, length;
    std::string etag;
};
struct multipart_upload : upload_description
{
    std::int64_t id, part_size;
    std::string key, multipart, state;
    std::vector<attachment_part> parts;
};
using upload_lookup = std::expected<std::optional<multipart_upload>, sqlite_error>;
upload_lookup read_upload(sqlite_database& db, std::int64_t user, std::string const& device, std::int64_t client_id);
upload_lookup begin_upload(sqlite_database& db, std::int64_t user, std::string const& device, upload_description const& input);
// Caller holds the publication transaction. Only a still-active, reserved upload may become a file.
std::expected<bool, sqlite_error> register_uploaded_file(sqlite_database& db, std::int64_t upload);
#endif
