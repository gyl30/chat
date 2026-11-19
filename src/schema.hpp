#ifndef CHAT_SRC_SCHEMA_HPP
#define CHAT_SRC_SCHEMA_HPP

#include "sqlite_database.hpp"

// Creates the schema on first open and rejects unknown schema versions.
// The caller must be the only writer while this runs (startup only).
sqlite_error initialize_schema(sqlite_database& database);

#endif
