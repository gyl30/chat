#ifndef CHAT_SRC_ATTACHMENT_CLEANUP_HPP
#define CHAT_SRC_ATTACHMENT_CLEANUP_HPP

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite_database.hpp"

// Persist an upload reservation before writing any bytes to the object store.
std::expected<void, sqlite_error> reserve_attachment_object(sqlite_database& db, std::string_view key, std::int64_t timestamp);

// Mark eligible objects as pending deletion before issuing any S3 request.
// Ordering by key keeps the cursor stable when a reservation is marked.
std::expected<std::vector<std::string>, sqlite_error> claim_attachment_cleanup_batch(sqlite_database& db,
                                                                                     std::int64_t cutoff,
                                                                                     std::string_view after);

// Forget an unclaimed object only after DELETE succeeds.
std::expected<void, sqlite_error> acknowledge_attachment_cleanup(sqlite_database& db, std::string_view key);

#endif
