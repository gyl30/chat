#include "attachment_access.hpp"

#include <utility>

std::expected<std::optional<attachment_object>, sqlite_error> find_attachment_object(sqlite_database& db,
                                                                                     std::int64_t user,
                                                                                     std::int64_t conversation,
                                                                                     std::int64_t message)
{
    auto statement = db.prepare(R"SQL(
        SELECT a.object_key,a.size,a.checksum
        FROM message_attachments a
        JOIN messages m ON m.id=a.message_id
        JOIN conversation_members own ON own.conversation_id=m.conversation_id AND own.user_id=?1
        WHERE m.conversation_id=?2 AND m.id=?3 AND m.deleted=0
    )SQL",
                                user,
                                conversation,
                                message);
    if (!statement)
    {
        return std::unexpected(std::move(statement.error()));
    }
    auto row = statement->step();
    if (!row)
    {
        return std::unexpected(std::move(row.error()));
    }
    if (!*row)
    {
        return std::nullopt;
    }
    return attachment_object{std::string(statement->column_text(0)), statement->column_int64(1), std::string(statement->column_text(2))};
}
