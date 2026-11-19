#ifndef CHAT_SRC_SQLITE_DATABASE_HPP
#define CHAT_SRC_SQLITE_DATABASE_HPP

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

struct sqlite3;
struct sqlite3_stmt;

// One error channel for every SQLite operation. The code is the extended
// result code so callers can distinguish constraint violations from I/O
// failures without parsing messages.
struct sqlite_error
{
    int code = 0;
    std::string message;

    explicit operator bool() const noexcept { return code != 0; }
};

struct sqlite3_deleter
{
    void operator()(sqlite3* database) const noexcept;
};

struct sqlite3_stmt_deleter
{
    void operator()(sqlite3_stmt* statement) const noexcept;
};

class sqlite_statement;

class sqlite_database
{
   public:
    sqlite_database() = default;
    sqlite_database(sqlite_database&& other) noexcept = default;
    sqlite_database& operator=(sqlite_database&& other) noexcept = default;
    sqlite_database(sqlite_database const&) = delete;
    sqlite_database& operator=(sqlite_database const&) = delete;

    // Opens the connection, enables foreign keys, WAL, full synchronous
    // durability, and a finite busy timeout.
    static std::expected<sqlite_database, sqlite_error> open(std::string const& path);

    [[nodiscard]] bool is_open() const noexcept { return database_ != nullptr; }

    std::expected<sqlite_statement, sqlite_error> prepare(std::string_view sql);

    template <class... Args>
        requires(sizeof...(Args) > 0)
    std::expected<sqlite_statement, sqlite_error> prepare(std::string_view sql, Args const&... args);

    // Runs one or more semicolon-separated statements without parameters.
    // Schema bootstrap and PRAGMAs use this; callers must not embed user data.
    sqlite_error exec(std::string_view sql);

    [[nodiscard]] std::int64_t last_insert_rowid() const noexcept;
    [[nodiscard]] int changes() const noexcept;

   private:
    friend class sqlite_statement;
    friend class sqlite_transaction;

    std::unique_ptr<sqlite3, sqlite3_deleter> database_;
};

class sqlite_statement
{
   public:
    sqlite_statement() = default;
    sqlite_statement(sqlite_statement&& other) noexcept = default;
    sqlite_statement& operator=(sqlite_statement&& other) noexcept = default;
    sqlite_statement(sqlite_statement const&) = delete;
    sqlite_statement& operator=(sqlite_statement const&) = delete;

    sqlite_error bind(int index, std::int64_t value);
    sqlite_error bind(int index, std::optional<std::int64_t> value) { return value ? bind(index, *value) : bind_null(index); }
    sqlite_error bind(int index, int value);
    sqlite_error bind(int index, std::string_view value);
    sqlite_error bind_null(int index);
    sqlite_error bind(int index, std::span<const std::byte> value);

    // true means a row is available, false means the statement finished.
    std::expected<bool, sqlite_error> step();

    // Resets the statement so it can be executed again; bindings are kept.
    sqlite_error reset();

    [[nodiscard]] std::int64_t column_int64(int index) const;
    [[nodiscard]] std::string_view column_text(int index) const;
    [[nodiscard]] std::span<const std::byte> column_blob(int index) const;
    [[nodiscard]] bool column_is_null(int index) const;

   private:
    friend class sqlite_database;

    explicit sqlite_statement(sqlite3_stmt* statement) noexcept : statement_(statement) {}

    std::unique_ptr<sqlite3_stmt, sqlite3_stmt_deleter> statement_;
};

template <class... Args>
    requires(sizeof...(Args) > 0)
std::expected<sqlite_statement, sqlite_error> sqlite_database::prepare(std::string_view sql, Args const&... args)
{
    auto statement = prepare(sql);
    if (!statement)
    {
        return statement;
    }

    sqlite_error error;
    int index = 0;
    if (((error = statement->bind(++index, args)) || ...))
    {
        return std::unexpected(std::move(error));
    }
    return statement;
}

class sqlite_transaction
{
   public:
    sqlite_transaction(sqlite_transaction&& other) noexcept;
    sqlite_transaction(sqlite_transaction const&) = delete;
    sqlite_transaction& operator=(sqlite_transaction const&) = delete;
    ~sqlite_transaction();

    static std::expected<sqlite_transaction, sqlite_error> begin_read(sqlite_database& database);

    // BEGIN IMMEDIATE takes the write lock before the first read so a
    // check-then-write sequence cannot interleave with another writer.
    static std::expected<sqlite_transaction, sqlite_error> begin_immediate(sqlite_database& database);

    sqlite_error commit();

   private:
    explicit sqlite_transaction(sqlite_database* database) noexcept : database_(database) {}
    void rollback();

    sqlite_database* database_ = nullptr;
};

#endif
