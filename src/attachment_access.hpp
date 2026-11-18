#ifndef CHAT_SRC_ATTACHMENT_ACCESS_HPP
#define CHAT_SRC_ATTACHMENT_ACCESS_HPP

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "sqlite_database.hpp"

struct attachment_object
{
    std::string key;
    std::int64_t size;
};

// The object key stays on the server. Resolve it anew for every download chunk.
std::expected<std::optional<attachment_object>, sqlite_error> find_attachment_object(
    sqlite_database& db, std::int64_t user, std::int64_t conversation, std::int64_t message);

#endif
