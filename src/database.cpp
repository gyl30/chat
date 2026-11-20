#include "database.hpp"

#include <sqlite3.h>

#include "schema.hpp"
#include "archive_reader.hpp"

std::expected<sqlite_database, sqlite_error> database::open_connection(std::string const& path, sqlite_database::open_mode mode)
{
    auto db = sqlite_database::open(path, mode);
    if (db)
    {
        if (auto error = register_archive_reader(*db); error)
        {
            return std::unexpected(error);
        }
    }
    return db;
}

std::expected<database::pointer, sqlite_error> database::open(std::string path)
{
    auto opened = open_connection(path, sqlite_database::open_mode::incremental_vacuum);
    if (!opened)
    {
        return std::unexpected(std::move(opened.error()));
    }
    if (auto error = initialize_schema(*opened); error)
    {
        return std::unexpected(std::move(error));
    }
    // The process owns the database lock. Active transfers left by a prior process are resumable.
    if (auto error = opened->exec("UPDATE attachment_uploads SET state='paused' WHERE state='active'"); error)
    {
        return std::unexpected(std::move(error));
    }
    return pointer(new database(std::move(path), std::move(*opened)));
}

database::database(std::string path, sqlite_database connection) : path_(std::move(path)), database_(std::move(connection)) {}

sqlite_error database::queue_error(blocking_worker::unavailable reason)
{
    if (reason == blocking_worker::unavailable::full)
    {
        return {SQLITE_BUSY, "database queue is full"};
    }
    return {SQLITE_ABORT, "database is shut down"};
}
