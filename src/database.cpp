#include "database.hpp"

#include <sqlite3.h>

#include "schema.hpp"

std::expected<database::pointer, sqlite_error> database::open(std::string path)
{
    auto opened = sqlite_database::open(path);
    if (!opened)
    {
        return std::unexpected(std::move(opened.error()));
    }
    if (auto error = initialize_schema(*opened); error)
    {
        return std::unexpected(std::move(error));
    }
    return pointer(new database(std::move(path), std::move(*opened)));
}

database::database(std::string path, sqlite_database connection)
    : path_(std::move(path)), database_(std::move(connection))
{
}

sqlite_error database::queue_error(blocking_worker::unavailable reason)
{
    if (reason == blocking_worker::unavailable::full)
    {
        return {SQLITE_BUSY, "database queue is full"};
    }
    return {SQLITE_ABORT, "database is shut down"};
}
