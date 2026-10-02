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
        PQconnectdb(""), &PQfinish);
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
              "009_add_message_replies.sql", "010_add_message_edits.sql", "011_add_message_deletion.sql",
              "012_create_message_attachments.sql", "013_add_group_roles.sql", "014_add_member_join_position.sql",
              "015_create_user_avatars.sql", "016_create_message_reactions.sql", "017_add_conversation_mute.sql",
              "018_add_conversation_pin.sql", "019_create_message_mentions.sql", "020_add_group_pinned_message.sql",
              "021_add_group_announcement.sql", "022_add_group_invite.sql", "023_create_group_join_requests.sql"})
        {
            if (std::string(name).starts_with("008"))
            {
                execute("INSERT INTO users(username,password_hash) VALUES('a',repeat('x',60)),('b',repeat('x',60)); "
                        "INSERT INTO contacts(owner_id,contact_id) VALUES(1,2); "
                        "INSERT INTO messages(sender_id,recipient_id,body) VALUES(1,2,'one'),(2,1,'two'),(1,1,'self'); "
                        "INSERT INTO message_read_positions(user_id,peer_user_id,last_read_message_id) "
                        "VALUES(2,1,1),(1,2,2),(1,1,0)");
            }
            if (std::string(name).starts_with("021") || std::string(name).starts_with("022") || std::string(name).starts_with("023"))
            {
                execute("WITH created AS (INSERT INTO conversations(kind,title,owner_id) VALUES('group','before announcement',1) RETURNING id) "
                        "INSERT INTO conversation_members(conversation_id,user_id) SELECT id,1 FROM created");
            }
            std::ifstream file(std::string(argv[1]) + "/" + name);
            if (!file)
            {
                throw std::runtime_error("Migration file missing");
            }
            execute(std::string(std::istreambuf_iterator<char>(file), {}));
            if (std::string(name).starts_with("021") || std::string(name).starts_with("022") || std::string(name).starts_with("023"))
            {
                auto defaults = execute(std::string(name).starts_with("021") ?
                    "SELECT count(*) FROM conversations WHERE announcement=''" :
                    std::string(name).starts_with("022") ? "SELECT count(*) FROM conversations WHERE invite_token IS NULL" :
                    "SELECT count(*) FROM conversations WHERE NOT join_approval");
                if (std::string(PQgetvalue(defaults.get(), 0, 0)) != "3") { throw std::runtime_error("Existing group metadata defaults"); }
                execute("DELETE FROM conversations WHERE title='before announcement'");
            }
        }
        auto result = execute("SELECT (SELECT count(*) FROM conversations)=2 "
                              "AND (SELECT count(*) FROM conversation_members)=3 "
                              "AND (SELECT count(*) FROM messages WHERE id IN (1,2,3))=3 "
                              "AND (SELECT count(*) FROM contacts)=1 "
                              "AND (SELECT count(*) FROM users WHERE avatar_revision=0)=2 "
                              "AND NOT EXISTS(SELECT 1 FROM user_avatars) "
                              "AND NOT EXISTS(SELECT 1 FROM messages WHERE reaction_revision<>0) "
                              "AND NOT EXISTS(SELECT 1 FROM message_reactions) "
                              "AND NOT EXISTS(SELECT 1 FROM message_mentions) "
                              "AND NOT EXISTS(SELECT 1 FROM conversations WHERE owner_id IS NOT NULL) "
                              "AND NOT EXISTS(SELECT 1 FROM conversations WHERE pinned_message_id IS NOT NULL) "
                              "AND NOT EXISTS(SELECT 1 FROM conversations WHERE announcement<>'') "
                              "AND NOT EXISTS(SELECT 1 FROM conversations WHERE invite_token IS NOT NULL) "
                              "AND NOT EXISTS(SELECT 1 FROM conversations WHERE join_approval) "
                              "AND NOT EXISTS(SELECT 1 FROM group_join_requests) "
                              "AND NOT EXISTS(SELECT 1 FROM conversation_members WHERE is_admin OR joined_message_id<>0 OR muted OR pinned) "
                              "AND (SELECT count(*) FROM conversation_members WHERE last_read_message_id IN (1,2))=2 "
                              "AND NOT EXISTS(SELECT 1 FROM messages m JOIN conversations c ON c.id=m.conversation_id "
                              "WHERE c.kind<>'direct' OR (m.id=3 AND c.direct_user_low<>c.direct_user_high))");
        if (PQntuples(result.get()) != 1 || std::string(PQgetvalue(result.get(), 0, 0)) != "t")
        {
            throw std::runtime_error("Migration invariants");
        }
        execute("INSERT INTO messages(sender_id,conversation_id,body,reply_to_id) "
                "SELECT 2,conversation_id,'reply',id FROM messages WHERE id=1");
        execute("INSERT INTO message_attachments(message_id,filename,media_type,size,data) "
                "VALUES(4,'empty.bin','application/octet-stream',0,''::bytea)");
        execute("INSERT INTO message_reactions(message_id,user_id,emoji) VALUES(4,2,'👍')");
        execute("INSERT INTO message_mentions(message_id,user_id) VALUES(4,2)");
        bool duplicate_rejected = false;
        try { execute("INSERT INTO message_reactions(message_id,user_id,emoji) VALUES(4,2,'❤️')"); }
        catch (std::runtime_error const&) { duplicate_rejected = true; }
        if (!duplicate_rejected) { throw std::runtime_error("Duplicate reaction accepted"); }
        bool direct_pin_rejected = false;
        try { execute("UPDATE conversations SET pinned_message_id=4 WHERE kind='direct'"); }
        catch (std::runtime_error const&) { direct_pin_rejected = true; }
        if (!direct_pin_rejected) { throw std::runtime_error("Direct conversation accepts group pin"); }
        bool direct_announcement_rejected = false;
        try { execute("UPDATE conversations SET announcement='group only' WHERE kind='direct'"); }
        catch (std::runtime_error const&) { direct_announcement_rejected = true; }
        if (!direct_announcement_rejected) { throw std::runtime_error("Direct conversation accepts announcement"); }
        bool direct_invite_rejected = false;
        try { execute("UPDATE conversations SET invite_token=repeat('a',64) WHERE kind='direct'"); }
        catch (std::runtime_error const&) { direct_invite_rejected = true; }
        if (!direct_invite_rejected) { throw std::runtime_error("Direct conversation accepts invite token"); }
        bool direct_approval_rejected = false;
        try { execute("UPDATE conversations SET join_approval=true WHERE kind='direct'"); }
        catch (std::runtime_error const&) { direct_approval_rejected = true; }
        if (!direct_approval_rejected) { throw std::runtime_error("Direct conversation accepts join approval"); }
        execute("UPDATE messages SET deleted=true,body='' WHERE id=1");
        auto deleted = execute("SELECT deleted AND body='' AND id=1 FROM messages WHERE id=1");
        if (std::string(PQgetvalue(deleted.get(), 0, 0)) != "t")
        {
            throw std::runtime_error("Deleted message invariant");
        }
        execute("WITH created AS (INSERT INTO conversations(kind,title,owner_id) VALUES('group','owned',1) RETURNING id) "
                "INSERT INTO conversation_members(conversation_id,user_id,is_admin) "
                "SELECT id,1,false FROM created UNION ALL SELECT id,2,true FROM created");
        auto owner = execute("SELECT count(*) FROM conversations c JOIN conversation_members m "
                             "ON m.conversation_id=c.id AND m.user_id=c.owner_id WHERE c.kind='group'");
        if (std::string(PQgetvalue(owner.get(), 0, 0)) != "1")
        {
            throw std::runtime_error("Group owner membership invariant");
        }
        bool rejected_owner = false;
        try
        {
            execute("DELETE FROM conversation_members WHERE user_id=1 AND conversation_id IN "
                    "(SELECT id FROM conversations WHERE kind='group')");
        }
        catch (std::runtime_error const&)
        {
            rejected_owner = true;
        }
        if (!rejected_owner)
        {
            throw std::runtime_error("Owner membership deletion accepted");
        }
        execute("UPDATE conversations SET announcement=repeat('x',4096) WHERE kind='group'");
        bool oversized_announcement_rejected = false;
        try { execute("UPDATE conversations SET announcement=repeat('x',4097) WHERE kind='group'"); }
        catch (std::runtime_error const&) { oversized_announcement_rejected = true; }
        if (!oversized_announcement_rejected) { throw std::runtime_error("Oversized announcement accepted"); }
        execute("UPDATE conversations SET invite_token=repeat('a',64) WHERE kind='group'");
        bool malformed_invite_rejected = false;
        try { execute("UPDATE conversations SET invite_token=repeat('z',64) WHERE kind='group'"); }
        catch (std::runtime_error const&) { malformed_invite_rejected = true; }
        if (!malformed_invite_rejected) { throw std::runtime_error("Malformed invite token accepted"); }
        bool duplicate_invite_rejected = false;
        try
        {
            execute("WITH created AS (INSERT INTO conversations(kind,title,owner_id,invite_token) "
                    "VALUES('group','duplicate invite',2,repeat('a',64)) RETURNING id) "
                    "INSERT INTO conversation_members(conversation_id,user_id) SELECT id,2 FROM created");
        }
        catch (std::runtime_error const&) { duplicate_invite_rejected = true; }
        if (!duplicate_invite_rejected) { throw std::runtime_error("Duplicate invite token accepted"); }
        execute("UPDATE conversations SET join_approval=true WHERE kind='group'; "
                "INSERT INTO group_join_requests(conversation_id,user_id) SELECT id,2 FROM conversations WHERE kind='group'");
        auto pending = execute("SELECT count(*) FROM group_join_requests WHERE created_at IS NOT NULL");
        if (std::string(PQgetvalue(pending.get(), 0, 0)) != "1") { throw std::runtime_error("Pending join request timestamp"); }
        bool duplicate_request_rejected = false;
        try { execute("INSERT INTO group_join_requests(conversation_id,user_id) SELECT id,2 FROM conversations WHERE kind='group'"); }
        catch (std::runtime_error const&) { duplicate_request_rejected = true; }
        if (!duplicate_request_rejected) { throw std::runtime_error("Duplicate pending request accepted"); }
        execute("INSERT INTO messages(sender_id,conversation_id,body) SELECT 2,id,'pinned' FROM conversations WHERE kind='group'; "
                "UPDATE conversations SET pinned_message_id=(SELECT max(id) FROM messages WHERE conversation_id=conversations.id) "
                "WHERE kind='group'");
        auto pinned = execute("SELECT count(*) FROM conversations WHERE pinned_message_id IS NOT NULL");
        if (std::string(PQgetvalue(pinned.get(), 0, 0)) != "1") { throw std::runtime_error("Pinned message reference"); }
        execute("DELETE FROM messages WHERE body='pinned'");
        auto unpinned = execute("SELECT count(*) FROM conversations WHERE pinned_message_id IS NOT NULL");
        if (std::string(PQgetvalue(unpinned.get(), 0, 0)) != "0") { throw std::runtime_error("Pinned message physical deletion"); }
        execute("INSERT INTO user_avatars(user_id,media_type,size,data) VALUES(1,'image/png',1,'x'::bytea)");
        execute("DELETE FROM users WHERE id=1");
        auto cleaned = execute("SELECT (SELECT count(*) FROM messages)+(SELECT count(*) FROM message_attachments)"
                               "+(SELECT count(*) FROM conversations WHERE kind='group')+(SELECT count(*) FROM user_avatars)"
                               "+(SELECT count(*) FROM message_reactions)+(SELECT count(*) FROM message_mentions)+(SELECT count(*) FROM group_join_requests)");
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
