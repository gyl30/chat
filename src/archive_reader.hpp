#pragma once

#include "sqlite_database.hpp"

// Only the server connection can resolve immutable monthly message bodies.
sqlite_error register_archive_reader(sqlite_database& db);
