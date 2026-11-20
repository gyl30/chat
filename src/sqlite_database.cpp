#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <utf8proc.h>

#include <utility>

#include <boost/locale/utf.hpp>

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

void unicode_lower(sqlite3_context* context, int, sqlite3_value** arguments)
{
    auto const* text = reinterpret_cast<char const*>(sqlite3_value_text(arguments[0]));
    if (!text)
    {
        sqlite3_result_null(context);
        return;
    }
    auto const size = sqlite3_value_bytes(arguments[0]);
    auto const* end = text + size;
    std::string lowered;
    lowered.reserve(size);
    while (text != end)
    {
        auto value = boost::locale::utf::utf_traits<char>::decode(text, end);
        if (value > 0x10ffff)
        {
            sqlite3_result_null(context);
            return;
        }
        utf8proc_uint8_t encoded[4];
        auto const length = utf8proc_encode_char(utf8proc_tolower(static_cast<utf8proc_int32_t>(value)), encoded);
        lowered.append(reinterpret_cast<char const*>(encoded), length);
    }
    sqlite3_result_text64(context, lowered.data(), lowered.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
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

std::expected<sqlite_database, sqlite_error> sqlite_database::open(std::string const& path, open_mode access)
{
    sqlite_database result;
    sqlite3* handle = nullptr;
    int const flags = access == open_mode::read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int const status = sqlite3_open_v2(path.c_str(), &handle, flags | SQLITE_OPEN_FULLMUTEX, nullptr);
    result.database_.reset(handle);
    if (status != SQLITE_OK)
    {
        return std::unexpected(make_error(handle, status));
    }

    sqlite3_extended_result_codes(handle, 1);
    if (auto code = sqlite3_create_function_v2(
            handle, "chat_lower", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, unicode_lower, nullptr, nullptr, nullptr);
        code != SQLITE_OK)
    {
        return std::unexpected(make_error(handle, code));
    }

    if (access == open_mode::incremental_vacuum)
    {
        if (auto error = result.exec("PRAGMA auto_vacuum=INCREMENTAL"); error)
        {
            return std::unexpected(std::move(error));
        }
    }
    if (auto error = result.exec("PRAGMA foreign_keys=ON"); error)
    {
        return std::unexpected(std::move(error));
    }
    if (int const code = sqlite3_busy_timeout(handle, 5000); code != SQLITE_OK)
    {
        return std::unexpected(make_error(handle, code));
    }
    if (access == open_mode::read_only)
    {
        return result;
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
    return sqlite_statement(statement);
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

std::int64_t sqlite_database::last_insert_rowid() const noexcept { return database_ ? sqlite3_last_insert_rowid(database_.get()) : 0; }

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

sqlite_error sqlite_statement::bind(int index, std::span<const std::byte> value)
{
    int const status = value.empty() ? sqlite3_bind_zeroblob(statement_.get(), index, 0)
                                     : sqlite3_bind_blob64(statement_.get(), index, value.data(), value.size(), SQLITE_TRANSIENT);
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

sqlite_transaction::sqlite_transaction(sqlite_transaction&& other) noexcept : database_(std::exchange(other.database_, nullptr)) {}

sqlite_transaction::~sqlite_transaction() { rollback(); }

std::expected<sqlite_transaction, sqlite_error> sqlite_transaction::begin_read(sqlite_database& database)
{
    if (auto error = database.exec("BEGIN"); error)
    {
        return std::unexpected(std::move(error));
    }
    return sqlite_transaction(&database);
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

void sqlite_transaction::rollback()
{
    auto* const db = std::exchange(database_, nullptr);
    if (!db || !db->is_open() || sqlite3_get_autocommit(db->database_.get()))
    {
        return;
    }
    if (auto error = db->exec("ROLLBACK"); error)
    {
        if (!sqlite3_get_autocommit(db->database_.get()))
        {
            // Closing prevents any later job from inheriting a failed transaction.
            db->database_.reset();
        }
        spdlog::warn("SQLite rollback failed ({})", error.code);
    }
}
