#pragma once

#include "sqlite_database.hpp"
#include <chat/message.hpp>

// Called after checking visibility, within the caller's snapshot.
std::expected<chat::poll_summary, sqlite_error> read_poll_summary(sqlite_database& db, std::int64_t message);
