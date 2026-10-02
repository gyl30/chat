#include "message_mentions.hpp"
#include "pg_connection.hpp"

#include <system_error>
#include <simdjson.h>

boost::capy::io_task<std::vector<chat::mention>> refresh_message_mentions(
    pg_connection& connection, std::int64_t conversation, std::int64_t message, std::string text)
{
    auto cleared = co_await connection.execute_row("DELETE FROM message_mentions WHERE message_id=$1::bigint",
                                                  {std::to_string(message)});
    if (std::get<0>(cleared)) { co_return {std::get<0>(cleared), {}}; }
    if (text.find('@') == std::string::npos) { co_return {std::error_code{}, {}}; }
    auto resolved = co_await connection.execute_scalar(R"SQL(
        WITH candidates AS (
            SELECT u.id,u.username FROM conversation_members cm JOIN users u ON u.id=cm.user_id
            JOIN conversations c ON c.id=cm.conversation_id
            WHERE c.id=$1::bigint AND c.kind='group' AND strpos($3,'@'||u.username)>0
        ), pattern AS (
            SELECT '(?<![[:alnum:]_@])@(' ||
                string_agg(regexp_replace(username,'([^[:alnum:]_[:space:]])','\\\1','g'),'|') ||
                ')(?![[:alnum:]_@])' AS value FROM candidates
        ), targets AS (
            SELECT DISTINCT c.id,c.username FROM pattern,
                LATERAL regexp_matches($3,pattern.value,'g') found
            JOIN candidates c ON c.username=found[1]
        ), stored AS (
            INSERT INTO message_mentions(message_id,user_id) SELECT $2::bigint,id FROM targets RETURNING user_id
        )
        SELECT coalesce(json_agg(json_build_object('user',id,'username',username) ORDER BY id),'[]'::json)::text
        FROM targets JOIN stored ON stored.user_id=targets.id
    )SQL", {std::to_string(conversation), std::to_string(message), std::move(text)});
    auto& [ec, json] = resolved;
    if (ec) { co_return {ec, {}}; }
    std::vector<chat::mention> result;
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    if (parser.iterate(padded).get(document) || document.get(result))
    {
        co_return {std::make_error_code(std::errc::protocol_error), {}};
    }
    co_return {std::error_code{}, std::move(result)};
}
