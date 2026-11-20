#include "archive_reader.hpp"

#include <sqlite3.h>

namespace
{
struct month_reader
{
    std::string directory;
    std::string key;
    std::unique_ptr<sqlite3, sqlite3_deleter> db;
    std::unique_ptr<sqlite3_stmt, sqlite3_stmt_deleter> row;

    bool open(std::string_view next)
    {
        if (next == key && row)
        {
            return true;
        }
        if (next.size() != 32 || next.find_first_not_of("0123456789abcdef") != std::string_view::npos)
        {
            return false;
        }
        row.reset();
        key.clear();
        sqlite3* file = nullptr;
        auto const path = directory + std::string(next) + ".sqlite3";
        int rc = sqlite3_open_v2(path.c_str(), &file, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
        db.reset(file);
        if (rc != SQLITE_OK)
        {
            return false;
        }
        sqlite3_stmt* statement = nullptr;
        rc = sqlite3_prepare_v2(file, "SELECT body FROM messages WHERE id=?1", -1, &statement, nullptr);
        row.reset(statement);
        if (rc != SQLITE_OK)
        {
            return false;
        }
        key = next;
        return true;
    }
};
void read_body(sqlite3_context* ctx, int, sqlite3_value** args)
{
    auto& reader = *static_cast<month_reader*>(sqlite3_user_data(ctx));
    auto const* key = reinterpret_cast<char const*>(sqlite3_value_text(args[0]));
    if (key == nullptr || !reader.open({key, static_cast<size_t>(sqlite3_value_bytes(args[0]))}))
    {
        sqlite3_result_error(ctx, "message archive unavailable", -1);
        return;
    }
    auto* row = reader.row.get();
    sqlite3_bind_int64(row, 1, sqlite3_value_int64(args[1]));
    if (sqlite3_step(row) == SQLITE_ROW && sqlite3_column_type(row, 0) == SQLITE_TEXT)
    {
        sqlite3_result_value(ctx, sqlite3_column_value(row, 0));
    }
    else
    {
        sqlite3_result_error(ctx, "archived message unavailable", -1);
    }
    sqlite3_reset(row);
}
}    // namespace

sqlite_error register_archive_reader(sqlite_database& db)
{
    auto* handle = db.native_handle();
    auto* reader = new month_reader{std::string(sqlite3_db_filename(handle, "main")) + ".archives/", {}, {}, {}};
    int const code = sqlite3_create_function_v2(
        handle, "chat_archive_body", 2, SQLITE_UTF8, reader, read_body, nullptr, nullptr, [](void* ptr) { delete static_cast<month_reader*>(ptr); });
    return code == SQLITE_OK ? sqlite_error{} : sqlite_error{.code = code, .message = sqlite3_errmsg(handle)};
}
