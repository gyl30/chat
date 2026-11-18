#ifndef CHAT_SRC_SCHEMA_HPP
#define CHAT_SRC_SCHEMA_HPP

#include <string_view>

#include "sqlite_database.hpp"

inline constexpr int chat_schema_version = 1;

std::string_view chat_schema_sql();

// Creates the schema on first open and rejects unknown schema versions.
// The caller must be the only writer while this runs (startup only).
sqlite_error initialize_schema(sqlite_database& database);

#endif
