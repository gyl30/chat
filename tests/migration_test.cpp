#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <libpq-fe.h>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        return 1;
    }
    std::unique_ptr<PGconn, decltype(&PQfinish)> connection(
        PQconnectdb("hostaddr=172.20.54.83 port=5432 dbname=chat user=chat sslmode=disable"), &PQfinish);
    auto schema = "chat_migration_test_" + std::to_string(getpid());
    auto execute = [&](std::string const& sql)
    {
        std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection.get(), sql.c_str()), &PQclear);
        if (!result ||
            (PQresultStatus(result.get()) != PGRES_COMMAND_OK && PQresultStatus(result.get()) != PGRES_TUPLES_OK))
        {
            throw std::runtime_error(PQerrorMessage(connection.get()));
        }
        return result;
    };
    bool created = false;
    try
    {
        if (PQstatus(connection.get()) != CONNECTION_OK)
        {
            throw std::runtime_error(PQerrorMessage(connection.get()));
        }
        execute("CREATE SCHEMA " + schema);
        created = true;
        execute("SET search_path TO " + schema);
        for (auto const* name :
             {"001_create_users.sql", "002_create_messages.sql", "003_add_messages_conversation_index.sql",
              "004_create_message_read_positions.sql", "005_add_messages_recipient_index.sql",
              "006_create_contacts.sql", "007_add_user_last_seen.sql", "008_create_conversations.sql",
              "009_add_message_replies.sql", "010_add_message_edits.sql"})
        {
            if (std::string(name).starts_with("008"))
            {
                execute("INSERT INTO users(username,password_hash) VALUES('a',repeat('x',60)),('b',repeat('x',60)); "
                        "INSERT INTO contacts(owner_id,contact_id) VALUES(1,2); "
                        "INSERT INTO messages(sender_id,recipient_id,body) VALUES(1,2,'one'),(2,1,'two'),(1,1,'self'); "
                        "INSERT INTO message_read_positions(user_id,peer_user_id,last_read_message_id) "
                        "VALUES(2,1,1),(1,2,2),(1,1,0)");
            }
            std::ifstream file(std::string(argv[1]) + "/" + name);
            if (!file)
            {
                throw std::runtime_error("Migration file missing");
            }
            execute(std::string(std::istreambuf_iterator<char>(file), {}));
        }
        auto result = execute("SELECT (SELECT count(*) FROM conversations)=2 "
                              "AND (SELECT count(*) FROM conversation_members)=3 "
                              "AND (SELECT count(*) FROM messages WHERE id IN (1,2,3))=3 "
                              "AND (SELECT count(*) FROM contacts)=1 "
                              "AND (SELECT count(*) FROM conversation_members WHERE last_read_message_id IN (1,2))=2 "
                              "AND NOT EXISTS(SELECT 1 FROM messages m JOIN conversations c ON c.id=m.conversation_id "
                              "WHERE c.kind<>'direct' OR (m.id=3 AND c.direct_user_low<>c.direct_user_high))");
        if (PQntuples(result.get()) != 1 || std::string(PQgetvalue(result.get(), 0, 0)) != "t")
        {
            throw std::runtime_error("Migration invariants");
        }
        execute("INSERT INTO messages(sender_id,conversation_id,body,reply_to_id) "
                "SELECT 2,conversation_id,'reply',id FROM messages WHERE id=1");
        execute("DELETE FROM users WHERE id=1");
        auto cleaned = execute("SELECT count(*) FROM messages");
        if (std::string(PQgetvalue(cleaned.get(), 0, 0)) != "0")
        {
            throw std::runtime_error("Reply cascade cleanup");
        }
        execute("SET search_path TO public");
        execute("DROP SCHEMA " + schema + " CASCADE");
        created = false;
        std::cout << "PASS fresh schema and populated direct/self/read-position migration\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL migration: " << error.what() << '\n';
        if (created)
        {
            try
            {
                execute("ROLLBACK");
                execute("SET search_path TO public");
                execute("DROP SCHEMA " + schema + " CASCADE");
            }
            catch (std::exception const& cleanup)
            {
                std::cerr << cleanup.what() << '\n';
            }
        }
        return 1;
    }
}
