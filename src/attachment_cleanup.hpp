#ifndef CHAT_SRC_ATTACHMENT_CLEANUP_HPP
#define CHAT_SRC_ATTACHMENT_CLEANUP_HPP

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite_database.hpp"

// Persist an upload reservation before writing any bytes to the object store.
sqlite_error reserve_attachment_object(sqlite_database& db, std::string_view key, std::int64_t timestamp);

// The caller supplies a cutoff that excludes active or still settling uploads.
std::expected<std::vector<std::string>, sqlite_error> attachment_cleanup_batch(sqlite_database& db,
                                                                              std::int64_t cutoff);

// Forget an unclaimed object only after DELETE succeeds.
sqlite_error acknowledge_attachment_cleanup(sqlite_database& db, std::string_view key);

#endif
