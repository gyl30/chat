#include <sqlite3.h>

#include <utility>

#include "sqlite_database.hpp"

namespace
{

sqlite_error make_error(sqlite3* database, int code)
{
    sqlite_error result;
    result.code = code;
    if (database)
    {
        char const* message = sqlite3_errmsg(database);
        result.message = message ? message : sqlite3_errstr(code);
        int const extended = sqlite3_extended_errcode(database);
        if (extended != 0)
        {
            result.code = extended;
        }
    }
    else
    {
        result.message = sqlite3_errstr(code);
    }
    return result;
}

sqlite_error make_error(sqlite3_stmt* statement)
{
    sqlite3* database = statement ? sqlite3_db_handle(statement) : nullptr;
    int code = statement ? sqlite3_errcode(database) : SQLITE_MISUSE;
    return make_error(database, code);
}

}    // namespace

void sqlite3_deleter::operator()(sqlite3* database) const noexcept
{
    if (database)
    {
        sqlite3_close_v2(database);
    }
}

void sqlite3_stmt_deleter::operator()(sqlite3_stmt* statement) const noexcept
{
    if (statement)
    {
        sqlite3_finalize(statement);
    }
}

std::expected<sqlite_database, sqlite_error> sqlite_database::open(std::string const& path, int busy_timeout_ms)
{
    sqlite_database result;
    sqlite3* handle = nullptr;
    int const status = sqlite3_open_v2(path.c_str(), &handle,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    result.database_.reset(handle);
    if (status != SQLITE_OK)
    {
        auto error = make_error(handle, status);
        result.database_.reset();
        return std::unexpected(std::move(error));
    }

    sqlite3_extended_result_codes(handle, 1);

    if (auto error = result.exec("PRAGMA foreign_keys=ON"); error)
    {
        return std::unexpected(std::move(error));
    }
    if (int const code = sqlite3_busy_timeout(handle, busy_timeout_ms); code != SQLITE_OK)
    {
        return std::unexpected(make_error(handle, code));
    }
    {
        auto mode = result.prepare("PRAGMA journal_mode=WAL");
        if (!mode)
        {
            return std::unexpected(std::move(mode.error()));
        }
        auto row = mode->step();
        if (!row)
        {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row || mode->column_text(0) != "wal")
        {
            return std::unexpected(sqlite_error{SQLITE_ERROR, "SQLite WAL mode is unavailable"});
        }
    }
    auto foreign_keys = result.prepare("PRAGMA foreign_keys");
    if (!foreign_keys)
    {
        return std::unexpected(std::move(foreign_keys.error()));
    }
    auto row = foreign_keys->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row || foreign_keys->column_int64(0) != 1)
    {
        return std::unexpected(sqlite_error{SQLITE_ERROR, "SQLite foreign key enforcement is unavailable"});
    }
    if (auto error = result.exec("PRAGMA synchronous=FULL"); error)
    {
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<sqlite_database, sqlite_error> sqlite_database::open_read_only(std::string const& path, int busy_timeout_ms)
{
    sqlite_database result;
    sqlite3* handle = nullptr;
    int const status = sqlite3_open_v2(path.c_str(), &handle, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    result.database_.reset(handle);
    if (status != SQLITE_OK)
    {
        auto error = make_error(handle, status);
        result.database_.reset();
        return std::unexpected(std::move(error));
    }

    sqlite3_extended_result_codes(handle, 1);
    if (auto error = result.exec("PRAGMA foreign_keys=ON"); error)
    {
        return std::unexpected(std::move(error));
    }
    if (auto error = result.exec("PRAGMA busy_timeout=" + std::to_string(busy_timeout_ms)); error)
    {
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<sqlite_statement, sqlite_error> sqlite_database::prepare(std::string_view sql)
{
    if (!database_)
    {
        return std::unexpected(sqlite_error{SQLITE_MISUSE, "database is not open"});
    }

    sqlite3_stmt* statement = nullptr;
    int const status = sqlite3_prepare_v2(database_.get(), sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
    if (status != SQLITE_OK)
    {
        return std::unexpected(make_error(database_.get(), status));
    }
    if (!statement)
    {
        return std::unexpected(sqlite_error{SQLITE_ERROR, "statement is empty"});
    }
    return sqlite_statement(database_.get(), statement);
}

sqlite_error sqlite_database::exec(std::string_view sql)
{
    if (!database_)
    {
        return {SQLITE_MISUSE, "database is not open"};
    }

    char* message = nullptr;
    std::string text(sql);
    int const status = sqlite3_exec(database_.get(), text.c_str(), nullptr, nullptr, &message);
    if (status != SQLITE_OK)
    {
        sqlite_error error;
        error.code = sqlite3_extended_errcode(database_.get());
        if (error.code == 0)
        {
            error.code = status;
        }
        error.message = message ? message : sqlite3_errstr(status);
        sqlite3_free(message);
        return error;
    }
    sqlite3_free(message);
    return {};
}

sqlite_error sqlite_database::last_error() const { return make_error(database_.get(), database_ ? sqlite3_extended_errcode(database_.get()) : SQLITE_MISUSE); }

std::int64_t sqlite_database::last_insert_rowid() const noexcept
{
    return database_ ? sqlite3_last_insert_rowid(database_.get()) : 0;
}

int sqlite_database::changes() const noexcept { return database_ ? sqlite3_changes(database_.get()) : 0; }

sqlite_error sqlite_statement::bind(int index, std::int64_t value)
{
    int const status = sqlite3_bind_int64(statement_.get(), index, value);
    return status == SQLITE_OK ? sqlite_error{} : make_error(statement_.get());
}

sqlite_error sqlite_statement::bind(int index, int value)
{
    int const status = sqlite3_bind_int(statement_.get(), index, value);
    return status == SQLITE_OK ? sqlite_error{} : make_error(statement_.get());
}

sqlite_error sqlite_statement::bind(int index, std::string_view value)
{
    int const status = sqlite3_bind_text64(statement_.get(), index, value.empty() ? "" : value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
    return status == SQLITE_OK ? sqlite_error{} : make_error(statement_.get());
}

sqlite_error sqlite_statement::bind_null(int index)
{
    int const status = sqlite3_bind_null(statement_.get(), index);
    return status == SQLITE_OK ? sqlite_error{} : make_error(statement_.get());
}

sqlite_error sqlite_statement::bind_blob(int index, std::span<const std::byte> value)
{
    int const status = value.empty() ? sqlite3_bind_zeroblob(statement_.get(), index, 0) :
        sqlite3_bind_blob64(statement_.get(), index, value.data(), value.size(), SQLITE_TRANSIENT);
    return status == SQLITE_OK ? sqlite_error{} : make_error(statement_.get());
}

std::expected<bool, sqlite_error> sqlite_statement::step()
{
    int const status = sqlite3_step(statement_.get());
    if (status == SQLITE_ROW)
    {
        return true;
    }
    if (status == SQLITE_DONE)
    {
        return false;
    }
    return std::unexpected(make_error(statement_.get()));
}

sqlite_error sqlite_statement::reset()
{
    int const status = sqlite3_reset(statement_.get());
    return status == SQLITE_OK ? sqlite_error{} : make_error(statement_.get());
}

sqlite_error sqlite_statement::clear_bindings()
{
    int const status = sqlite3_clear_bindings(statement_.get());
    return status == SQLITE_OK ? sqlite_error{} : make_error(statement_.get());
}

std::int64_t sqlite_statement::column_int64(int index) const { return sqlite3_column_int64(statement_.get(), index); }

std::string_view sqlite_statement::column_text(int index) const
{
    auto const* text = reinterpret_cast<char const*>(sqlite3_column_text(statement_.get(), index));
    int const size = sqlite3_column_bytes(statement_.get(), index);
    return text ? std::string_view(text, static_cast<std::size_t>(size)) : std::string_view{};
}

std::span<const std::byte> sqlite_statement::column_blob(int index) const
{
    auto const* data = static_cast<std::byte const*>(sqlite3_column_blob(statement_.get(), index));
    int const size = sqlite3_column_bytes(statement_.get(), index);
    return data ? std::span<const std::byte>(data, static_cast<std::size_t>(size)) : std::span<const std::byte>{};
}

bool sqlite_statement::column_is_null(int index) const { return sqlite3_column_type(statement_.get(), index) == SQLITE_NULL; }

int sqlite_statement::column_count() const { return statement_ ? sqlite3_column_count(statement_.get()) : 0; }

sqlite_transaction::sqlite_transaction(sqlite_transaction&& other) noexcept : database_(std::exchange(other.database_, nullptr)) {}

sqlite_transaction& sqlite_transaction::operator=(sqlite_transaction&& other) noexcept
{
    if (this != &other)
    {
        if (database_)
        {
            database_->exec("ROLLBACK");
        }
        database_ = std::exchange(other.database_, nullptr);
    }
    return *this;
}

sqlite_transaction::~sqlite_transaction()
{
    if (database_)
    {
        database_->exec("ROLLBACK");
    }
}

std::expected<sqlite_transaction, sqlite_error> sqlite_transaction::begin_immediate(sqlite_database& database)
{
    if (auto error = database.exec("BEGIN IMMEDIATE"); error)
    {
        return std::unexpected(std::move(error));
    }
    return sqlite_transaction(&database);
}

sqlite_error sqlite_transaction::commit()
{
    if (!database_)
    {
        return {SQLITE_MISUSE, "transaction is not active"};
    }
    auto error = database_->exec("COMMIT");
    if (!error)
    {
        database_ = nullptr;
    }
    return error;
}

sqlite_error sqlite_transaction::rollback()
{
    if (!database_)
    {
        return {};
    }
    auto error = database_->exec("ROLLBACK");
    database_ = nullptr;
    return error;
}
