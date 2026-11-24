#pragma once

#include "sqlite_database.hpp"

std::expected<bool, sqlite_error> store_bundle(sqlite_database& db, std::int64_t parent, std::int64_t sender, std::span<std::int64_t const> sources);
