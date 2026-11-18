#ifndef CHAT_SRC_ATTACHMENT_CLEANUP_HPP
#define CHAT_SRC_ATTACHMENT_CLEANUP_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite_database.hpp"

// Persist an upload reservation before writing any bytes to the object store.
sqlite_error reserve_attachment_object(sqlite_database& db, std::string_view key, std::int64_t timestamp);

struct attachment_cleanup_entry
{
    std::int64_t reserved_at;
    std::string object_key;
};

// The cutoff excludes active uploads. The cursor lets a failed DELETE stay
// pending without preventing recovery of later objects.
std::expected<std::vector<attachment_cleanup_entry>, sqlite_error> attachment_cleanup_batch(
    sqlite_database& db, std::int64_t cutoff, std::optional<attachment_cleanup_entry> const& after);

// Forget an unclaimed object only after DELETE succeeds.
sqlite_error acknowledge_attachment_cleanup(sqlite_database& db, std::string_view key);

#endif
