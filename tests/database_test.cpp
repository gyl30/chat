#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <sqlite3.h>
#include <unistd.h>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <latch>
#include <stop_token>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>

#include "conversation_store.hpp"
#include "database.hpp"
#include "friendship_store.hpp"
#include "message_store.hpp"
#include "message_history.hpp"
#include "schema.hpp"
#include "user_store.hpp"

namespace
{

int failures = 0;

void check(bool condition, std::string const& message)
{
    if (condition)
    {
        std::cout << "PASS " << message << '\n';
    }
    else
    {
        std::cerr << "FAIL " << message << '\n';
        ++failures;
    }
}

struct temporary_database
{
    std::string path;

    temporary_database()
    {
        auto const base = std::filesystem::temp_directory_path() /
            ("chat_sqlite_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        path = base.string();
        std::filesystem::remove(path);
        std::filesystem::remove(path + "-wal");
        std::filesystem::remove(path + "-shm");
    }

    ~temporary_database()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path + "-wal", error);
        std::filesystem::remove(path + "-shm", error);
    }

    static inline std::atomic<int> counter{0};
};

std::int64_t count(sqlite_database& database, std::string_view table)
{
    auto statement = database.prepare("SELECT count(*) FROM " + std::string(table));
    if (!statement)
    {
        return -1;
    }
    auto row = statement->step();
    if (!row || !*row)
    {
        return -1;
    }
    return statement->column_int64(0);
}

boost::capy::task<> low_level_tests(temporary_database& file)
{
    {
        auto opened = sqlite_database::open(file.path);
        check(opened.has_value(), "first open creates the database");
        if (!opened)
        {
            co_return;
        }
        check(initialize_schema(*opened).ok(), "first open creates the schema");

        auto statement = opened->prepare("PRAGMA user_version");
        auto row = statement ? statement->step() : std::expected<bool, sqlite_error>{};
        check(statement && row && *row && statement->column_int64(0) == chat_schema_version, "schema version is recorded");

        auto insert = opened->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES(?,?,?)");
        check(insert.has_value(), "prepare insert");
        if (insert)
        {
            check(insert->bind(1, "alice").ok() && insert->bind(2, std::string(60, 'x')).ok() && insert->bind(3, 1).ok(),
                  "bind insert parameters");
            check(insert->step().has_value(), "insert user");
            check(opened->last_insert_rowid() == 1, "first user id is one");
        }

        auto conversation = opened->prepare("INSERT INTO conversations(kind,title,activity) VALUES('group','group',0)");
        check(conversation.has_value(), "prepare conversation insert");
        if (conversation)
        {
            check(conversation->step().has_value(), "insert conversation");
        }

        auto missing_user = opened->prepare(
            "INSERT INTO messages(conversation_id,sender_id,body,created_at) VALUES(1,999,'body',0)");
        check(missing_user.has_value(), "prepare foreign key insert");
        if (missing_user)
        {
            auto result = missing_user->step();
            check(!result && result.error().code == SQLITE_CONSTRAINT_FOREIGNKEY, "foreign key violation is reported");
        }

        auto transaction = sqlite_transaction::begin_immediate(*opened);
        check(transaction.has_value(), "begin immediate transaction");
        if (transaction)
        {
            if (auto statement = opened->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES('bob','" +
                    std::string(60, 'y') + "',1)"); statement)
            {
                check(statement->step().has_value(), "insert inside transaction");
            }
            check(transaction->rollback().ok(), "explicit rollback");
        }
        check(count(*opened, "users") == 1, "rollback removes the inserted user");

        auto committed = sqlite_transaction::begin_immediate(*opened);
        if (committed)
        {
            if (auto statement = opened->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES('carol','" +
                    std::string(60, 'z') + "',1)"); statement)
            {
                check(statement->step().has_value(), "insert before commit");
            }
            check(committed->commit().ok(), "explicit commit");
        }
        check(count(*opened, "users") == 2, "commit persists the user");

        auto member = opened->prepare("INSERT INTO conversation_members(conversation_id,user_id) VALUES(1,1)");
        check(member && member->step().has_value(), "insert group member");
        auto cascade = opened->prepare("DELETE FROM users WHERE id=1");
        check(cascade && cascade->step().has_value(), "delete user");
        check(count(*opened, "conversation_members") == 0, "foreign key cascade deletes memberships");
    }

    {
        auto reopened = sqlite_database::open(file.path);
        check(reopened.has_value(), "reopen existing database");
        if (reopened)
        {
            check(initialize_schema(*reopened).ok(), "reopen accepts the stored schema version");
            check(count(*reopened, "users") == 1, "reopen sees committed data");
        }
    }

    {
        auto read_only = sqlite_database::open_read_only(file.path);
        check(read_only.has_value(), "open read-only connection");
        if (read_only)
        {
            auto statement = read_only->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES('dave','" +
                std::string(60, 'w') + "',1)");
            check(statement.has_value(), "prepare write on a read-only connection");
            if (statement)
            {
                auto result = statement->step();
                check(!result && (result.error().code & 0xff) == SQLITE_READONLY, "read-only write is rejected");
            }
        }
    }

    {
        auto first = sqlite_database::open(file.path, 0);
        auto second = sqlite_database::open(file.path, 0);
        check(first && second, "open two writers for busy testing");
        if (first && second)
        {
            check(first->exec("BEGIN IMMEDIATE").ok(), "first connection takes the write lock");
            auto insert = second->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES('eve','" +
                std::string(60, 'v') + "',1)");
            check(insert.has_value(), "prepare competing write");
            if (insert)
            {
                auto result = insert->step();
                check(!result && ((result.error().code & 0xff) == SQLITE_BUSY || (result.error().code & 0xff) == SQLITE_LOCKED),
                      "competing write reports busy");
            }
            check(first->exec("COMMIT").ok(), "first connection releases the write lock");
            if (insert)
            {
                check(insert->step().has_value(), "competing write succeeds after the lock is released");
            }
        }
    }

    {
        auto opened = sqlite_database::open(file.path);
        check(opened.has_value(), "open for disk-full test");
        if (opened)
        {
            auto pages = opened->prepare("PRAGMA page_count");
            auto row = pages ? pages->step() : std::expected<bool, sqlite_error>{};
            check(pages && row && *row, "read page count");
            std::int64_t const limit = pages ? pages->column_int64(0) : 0;
            check(opened->exec("PRAGMA max_page_count=" + std::to_string(limit)).ok(), "limit the database size");
            auto insert = opened->prepare("INSERT INTO users(username,password_hash,last_seen_at) VALUES(?,?,1)");
            if (insert)
            {
                check(insert->bind(1, std::string(8192, 'u')).ok() && insert->bind(2, std::string(60, 'u')).ok(),
                      "bind oversized value");
            }
            auto result = insert ? insert->step() : std::expected<bool, sqlite_error>{};
            check(insert && !result && (result.error().code & 0xff) == SQLITE_FULL, "disk full is reported");
            check(count(*opened, "users") >= 1, "database remains usable after a failed write");
        }
    }

    co_return;
}

void friendship_tests()
{
    using enum friendship_action;
    using chat::friendship_state;
    temporary_database file;
    auto left = sqlite_database::open(file.path);
    check(left && initialize_schema(*left).ok(), "open friendship schema");
    if (!left)
    {
        return;
    }
    check(left->exec(
        "INSERT INTO users(username,password_hash,last_seen_at)"
        " VALUES('Alice',printf('%060d',1),0),('Bob',printf('%060d',2),0)").ok(), "seed two users");
    auto right = sqlite_database::open(file.path);
    check(right.has_value(), "open independent friendship writer");
    if (!right)
    {
        return;
    }

    std::barrier start(2);
    auto opposite = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        return change_friendship(*right, 2, 1, request);
    });
    start.arrive_and_wait();
    auto first = change_friendship(*left, 1, 2, request);
    auto second = opposite.get();
    auto const state = [](auto const& result) {
        auto const* change = result ? std::get_if<friendship_change>(&*result) : nullptr;
        return change ? change->result.state : friendship_state::none;
    };
    auto const first_state = state(first);
    auto const second_state = state(second);
    bool const first_sent = first_state == friendship_state::outgoing_pending;
    check((first_sent && second_state == friendship_state::incoming_pending) ||
          (second_state == friendship_state::outgoing_pending && first_state == friendship_state::incoming_pending),
          "opposite requests leave one pending request and never auto-accept");
    auto const sender = first_sent ? 1 : 2;
    auto const recipient = first_sent ? 2 : 1;
    auto requests = read_friend_requests(*left, recipient);
    check(requests && requests->incoming.size() == 1 && requests->outgoing.empty() &&
          requests->incoming.front().user.id == sender && requests->incoming.front().created_at > 0,
          "recipient sees a single pending request with millisecond timestamp");
    auto const not_contacts = read_contacts(*left, recipient);
    check(not_contacts && not_contacts->empty(), "pending request is not a contact");
    auto duplicate = change_friendship(*left, sender, recipient, request);
    check(state(duplicate) == friendship_state::outgoing_pending &&
          std::get<friendship_change>(*duplicate).previous == friendship_state::outgoing_pending,
          "same direction request is idempotent");

    // Fail after the request deletion to prove the transaction restores it.
    check(left->exec("CREATE TRIGGER fail_contacts BEFORE INSERT ON contacts "
                     "BEGIN SELECT RAISE(ABORT,'test failure'); END;").ok(), "install SQLite failure fixture");
    auto failed_acceptance = change_friendship(*left, recipient, sender, accept);
    check(!failed_acceptance && failed_acceptance.error().code == SQLITE_CONSTRAINT_TRIGGER &&
          count(*left, "friend_requests") == 1 && count(*left, "contacts") == 0,
          "acceptance failure restores the request and creates no contact");
    check(left->exec("DROP TRIGGER fail_contacts").ok(), "remove SQLite failure fixture");

    auto accepted = change_friendship(*left, recipient, sender, accept);
    check(state(accepted) == friendship_state::accepted && count(*left, "friend_requests") == 0 &&
          count(*left, "contacts") == 2, "acceptance commits both contact directions atomically");
    auto reciprocal = read_contacts(*right, sender);
    check(reciprocal && reciprocal->size() == 1 && reciprocal->front().id == recipient,
          "second database connection observes both committed contacts");
    auto removed = change_friendship(*right, sender, recipient, remove);
    check(state(removed) == friendship_state::none && count(*left, "contacts") == 0,
          "removal clears both directions");
    check(state(change_friendship(*left, sender, recipient, request)) == friendship_state::outgoing_pending &&
          state(change_friendship(*right, sender, recipient, cancel)) == friendship_state::none &&
          count(*left, "friend_requests") == 0, "requester can cancel a new request");
    check(state(change_friendship(*left, sender, recipient, request)) == friendship_state::outgoing_pending &&
          state(change_friendship(*right, recipient, sender, reject)) == friendship_state::none &&
          count(*left, "friend_requests") == 0, "recipient can reject without creating contacts");
}

void message_write_tests()
{
    temporary_database file;
    auto left = sqlite_database::open(file.path);
    auto right = sqlite_database::open(file.path);
    check(left && right && initialize_schema(*left).ok(), "open two SQLite writers for messages");
    if (!left || !right)
    {
        return;
    }
    check(left->exec(R"SQL(
        INSERT INTO users(username,password_hash,last_seen_at)
            VALUES('Alice',printf('%060d',1),0),('Bob',printf('%060d',2),0),('Carol',printf('%060d',3),0);
        INSERT INTO conversations(kind,activity,direct_user_low,direct_user_high)
            VALUES('direct',0,1,2),('direct',0,1,3),('direct',0,1,1);
        INSERT INTO conversation_members(conversation_id,user_id) VALUES(1,1),(1,2),(2,1),(2,3),(3,1);
        INSERT INTO contacts(owner_id,contact_id,created_at) VALUES(1,2,0),(2,1,0),(1,3,0),(3,1,0);
    )SQL").ok(), "seed direct conversations and historical self conversation");

    message_to_write draft{1, 1, 9, "original text", {}};
    std::barrier start(2);
    auto concurrent = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        return write_message(*right, draft);
    });
    start.arrive_and_wait();
    auto first = write_message(*left, draft);
    auto second = concurrent.get();
    auto const* a = first ? std::get_if<message_write>(&*first) : nullptr;
    auto const* b = second ? std::get_if<message_write>(&*second) : nullptr;
    check(a && b && a->id == b->id && a->inserted != b->inserted && count(*left, "messages") == 1,
          "simultaneous retries commit one message");
    if (!a || !b)
    {
        return;
    }

    using enum message_write_rejection;
    auto rejected_as = [](auto const& result, message_write_rejection reason) {
        auto const* failure = result ? std::get_if<message_write_rejection>(&*result) : nullptr;
        return failure && *failure == reason;
    };
    check(rejected_as(write_message(*right, {1, 2, 9, "another conversation", {}}), invalid_client_id),
          "client ID cannot be reused in a different conversation");
    check(rejected_as(write_message(*right, {1, 2, 10, "cross conversation", a->id}), invalid_reply),
          "cross-conversation reply is rejected");
    check(rejected_as(write_message(*right, {1, 3, 10, "new self communication", {}}), unavailable),
          "historical self conversation remains read-only");
    check(left->exec("UPDATE messages SET deleted=1,body=''").ok(), "mark original target deleted");
    check(rejected_as(write_message(*right, {1, 1, 10, "deleted target", a->id}), invalid_reply),
          "a new reply cannot point to a deleted message");
    check(left->exec("DELETE FROM contacts").ok(), "remove friendship without destroying history");
    auto replay = write_message(*right, draft);
    auto const* replayed = replay ? std::get_if<message_write>(&*replay) : nullptr;
    check(replayed && replayed->id == a->id && !replayed->inserted,
          "existing tombstone can settle a replay after friendship ends");
    check(rejected_as(write_message(*right, {1, 1, 10, "new send", {}}), unavailable),
          "removed friendship denies a new direct send");
    check(left->exec("DELETE FROM conversation_members WHERE conversation_id=1 AND user_id=1").ok(),
          "remove sender membership");
    check(rejected_as(write_message(*right, draft), unavailable),
          "replay cannot bypass membership");
    check(count(*left, "messages") == 1, "rejected and replayed requests add no persisted rows");

    check(left->exec(R"SQL(
        WITH RECURSIVE ids(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM ids WHERE n<55)
        INSERT INTO messages(conversation_id,sender_id,body,created_at,client_id)
        SELECT 2,1,'page '||n,n,100+n FROM ids;
    )SQL").ok(), "seed a multi-page conversation");
    check(left->exec(R"SQL(
        INSERT INTO message_reactions(message_id,user_id,emoji)
        SELECT id,3,'👍' FROM messages WHERE conversation_id=2 AND client_id IN (102,106,155);
        INSERT INTO message_mentions(message_id,user_id)
        SELECT id,3 FROM messages WHERE conversation_id=2 AND client_id IN (102,106,155);
    )SQL").ok(), "seed interactions on both history pages");
    int statements = 0;
    sqlite3_trace_v2(left->handle(), SQLITE_TRACE_STMT, [](unsigned, void* count, void*, void*) {
        ++*static_cast<int*>(count);
        return 0;
    }, &statements);
    auto const history_start = std::chrono::steady_clock::now();
    auto latest = read_message_history(*left, 1, 2, {}, {});
    auto const history_elapsed = std::chrono::steady_clock::now() - history_start;
    sqlite3_trace_v2(left->handle(), 0, nullptr, nullptr);
    std::cout << "50-message history: " << statements << " statements, "
              << std::chrono::duration_cast<std::chrono::microseconds>(history_elapsed).count() << " us\n";
    check(statements <= 6, "history fetches interactions in a fixed number of statements");
    check(latest && *latest && (*latest)->messages.size() == 50 && (*latest)->has_more,
          "latest history returns 50 messages and the older page indicator");
    if (!latest || !*latest || (*latest)->messages.size() != 50)
    {
        return;
    }
    auto const first_id = (*latest)->messages.front().id;
    auto const last_id = (*latest)->messages.back().id;
    check((*latest)->messages.front().client_id == 106 && (*latest)->messages.back().client_id == 155,
          "latest history is ascending and includes sender-owned client IDs");
    bool interactions_match = true;
    for (auto const& message : (*latest)->messages)
    {
        bool const has_interactions = message.id == first_id || message.id == last_id;
        interactions_match &= has_interactions
            ? message.reactions.size() == 1 && message.reactions.front().emoji == "👍" &&
              message.reactions.front().users == std::vector<std::int64_t>{3} &&
              message.mentions.size() == 1 && message.mentions.front().user == 3
            : message.reactions.empty() && message.mentions.empty();
    }
    check(interactions_match, "history associates reactions and mentions with the correct message");
    auto older = read_message_history(*right, 3, 2, first_id, {});
    check(older && *older && (*older)->messages.size() == 5 && !(*older)->has_more &&
          (*older)->messages.back().id < first_id && !(*older)->messages.front().client_id,
          "older history is exclusive, complete and hides another user's client ID");
    auto after = read_message_history(*left, 3, 2, {}, first_id);
    check(after && *after && (*after)->messages.size() == 49 && !(*after)->has_more &&
          (*after)->messages.front().id > first_id && (*after)->messages.back().id == last_id,
          "reconnect cursor returns newer messages in ascending order");
    auto outsider = read_message_history(*left, 2, 2, {}, {});
    check(outsider && !*outsider, "non-member cannot query history");
    auto recorded = update_read_position(*left, 3, 2, last_id);
    auto lower = update_read_position(*right, 3, 2, first_id);
    auto cross_conversation = update_read_position(*right, 3, 2, a->id);
    auto unauthorized = update_read_position(*right, 2, 2, last_id);
    check(recorded && *recorded == last_id && lower && *lower == last_id,
          "read position persists and cannot regress after an older acknowledgement");
    check(cross_conversation && !*cross_conversation && unauthorized && !*unauthorized,
          "read update requires membership and a message in the same conversation");
    auto snapshot = read_message_history(*left, 3, 2, {}, last_id);
    check(snapshot && *snapshot && (*snapshot)->messages.empty() && (*snapshot)->read_positions.size() == 2 &&
          (*snapshot)->read_positions.back().user == 3 && (*snapshot)->read_positions.back().message == last_id,
          "empty incremental history still returns authoritative reading positions");
}

void conversation_tests()
{
    temporary_database file;
    auto db = sqlite_database::open(file.path);
    check(db && initialize_schema(*db).ok(), "open SQLite group fixture");
    if (!db)
    {
        return;
    }
    check(db->exec(R"SQL(
        INSERT INTO users(username,password_hash,last_seen_at)
            VALUES('Owner',printf('%060d',1),0),('Member',printf('%060d',2),0),('Invitee',printf('%060d',3),0);
        INSERT INTO contacts(owner_id,contact_id,created_at) VALUES(1,2,0),(2,1,0),(1,3,0),(3,1,0);
    )SQL").ok(), "seed current contacts");
    auto direct = open_direct(*db, 1, 2);
    auto reversed = open_direct(*db, 2, 1);
    auto denied = open_direct(*db, 2, 3);
    check(direct && *direct && reversed && *reversed == *direct && denied && !*denied,
          "opening a direct requires friendship and the unordered pair remains unique");
    auto group = create_group(*db, 1, "群聊", std::array<std::int64_t, 1>{2});
    auto unapproved = create_group(*db, 2, "不能创建", std::array<std::int64_t, 1>{3});
    check(group && *group && unapproved && !*unapproved && count(*db, "conversations") == 2,
          "group creation commits owner and members together and rejects noncontacts");
    if (!group || !*group)
    {
        return;
    }

    auto const conversation = **group;
    std::array<std::int64_t, 1> const invitees{3};
    auto ordinary = invite_group_members(*db, conversation, 2, invitees);
    auto outsider = invite_group_members(*db, conversation, 3, std::array<std::int64_t, 1>{1});
    check(ordinary && std::get_if<group_invite_rejection>(&*ordinary) &&
          std::get<group_invite_rejection>(*ordinary) == group_invite_rejection::permission_denied &&
          outsider && std::get_if<group_invite_rejection>(&*outsider) &&
          std::get<group_invite_rejection>(*outsider) == group_invite_rejection::unavailable,
          "ordinary members and outsiders cannot invite");

    auto other = sqlite_database::open(file.path);
    check(other.has_value(), "open independent writer for group race");
    if (!other)
    {
        return;
    }
    std::barrier start(2);
    auto sender = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        return write_message(*other, {2, conversation, 123, "send while inviting", {}});
    });
    start.arrive_and_wait();
    auto joined = invite_group_members(*db, conversation, 1, invitees);
    auto sent = sender.get();
    auto const* added = joined ? std::get_if<bool>(&*joined) : nullptr;
    auto const* saved = sent ? std::get_if<message_write>(&*sent) : nullptr;
    check(added && *added && saved && saved->inserted, "concurrent invite and send both commit");
    if (!added || !*added || !saved)
    {
        return;
    }

    auto before = db->prepare("SELECT joined_message_id,last_read_message_id FROM conversation_members "
                              "WHERE conversation_id=?1 AND user_id=3", conversation);
    auto member = before ? before->step() : std::expected<bool, sqlite_error>{false};
    check(member && *member && (before->column_int64(0) == 0 || before->column_int64(0) == saved->id) &&
          before->column_int64(1) == 0, "serialized invite preserves the join watermark without marking any message read");
    before = sqlite_statement{};
    auto duplicate = invite_group_members(*other, conversation, 1, invitees);
    check(duplicate && std::get_if<bool>(&*duplicate) && !std::get<bool>(*duplicate),
          "repeat invite does not recreate the membership");

    using enum group_member_action;
    auto changed = [](auto const& result) {
        auto const* value = result ? std::get_if<bool>(&*result) : nullptr;
        return value && *value;
    };
    auto was_denied = [](auto const& result, group_member_rejection reason) {
        auto const* value = result ? std::get_if<group_member_rejection>(&*result) : nullptr;
        return value && *value == reason;
    };
    check(was_denied(change_group_member(*db, conversation, 1, leave, 0), group_member_rejection::permission_denied),
          "owner cannot leave until ownership is transferred");
    check(was_denied(change_group_member(*db, conversation, 1, transfer, 2), group_member_rejection::permission_denied),
          "ownership transfer requires a current administrator");
    check(changed(change_group_member(*other, conversation, 1, promote, 2)), "owner promotes current member");
    check(was_denied(change_group_member(*db, conversation, 2, remove, 1), group_member_rejection::permission_denied),
          "administrator cannot remove the owner");
    std::barrier roles_start(2);
    auto member_leaves = std::async(std::launch::async, [&] {
        roles_start.arrive_and_wait();
        return change_group_member(*other, conversation, 2, leave, 0);
    });
    roles_start.arrive_and_wait();
    auto transferred = change_group_member(*db, conversation, 1, transfer, 2);
    auto left = member_leaves.get();
    check((changed(transferred) && was_denied(left, group_member_rejection::permission_denied)) ||
          (changed(left) && was_denied(transferred, group_member_rejection::member_not_found)),
          "transfer and leave compete without leaving a group with an absent owner");
    auto owner = db->prepare(R"SQL(
        SELECT c.owner_id,m.is_admin FROM conversations c
        JOIN conversation_members m ON m.conversation_id=c.id AND m.user_id=c.owner_id WHERE c.id=?1
    )SQL", conversation);
    auto current = owner ? owner->step() : std::expected<bool, sqlite_error>{false};
    check(current && *current && owner->column_int64(0) == (changed(transferred) ? 2 : 1) &&
          owner->column_int64(1) == 0, "owner membership and role survive serialized changes");
}

void account_tests()
{
    temporary_database file;
    auto db = sqlite_database::open(file.path);
    check(db && initialize_schema(*db).ok(), "prepare SQLite account schema");
    if (!db)
    {
        return;
    }
    std::string const hash(60, 'x');
    auto created = register_account(*db, "张 三", hash);
    auto duplicate = register_account(*db, "张 三", std::string(60, 'y'));
    auto account = find_account(*db, "张 三");
    check(created && *created && duplicate && !*duplicate && account && *account &&
          (*account)->user.id == **created && (*account)->password_hash == hash &&
          (*account)->user.avatar == chat::avatar_state{}, "registered identity is unique and stores the original hash");
    if (!created || !*created)
    {
        return;
    }
    auto const user = **created;
    std::string bytes(34 * 1024, 'X');
    bytes.front() = '\0';
    bytes.back() = '\0';
    auto first = save_avatar(*db, user, "image/png", bytes);
    check(first && first->state == chat::avatar_state{1, true}, "avatar upload creates revision one");
    auto begin = read_avatar_chunk(*db, user, 1, 0);
    auto end = read_avatar_chunk(*db, user, 1, 32768);
    check(begin && *begin && (*begin)->has_more && end && *end && !(*end)->has_more &&
          (*end)->offset == 32768 && (*begin)->bytes + (*end)->bytes == bytes,
          "avatar BLOB range reads preserve NUL bytes");
    auto invalid = save_avatar(*db, user, "text/plain", "bad media");
    auto preserved = find_account(*db, "张 三");
    check(!invalid && preserved && *preserved && (*preserved)->user.avatar == chat::avatar_state{1, true},
          "invalid avatar cannot change the revision");
    auto cleared = clear_avatar(*db, user);
    auto noop = clear_avatar(*db, user);
    auto updated = save_avatar(*db, user, "image/jpeg", bytes);
    check(cleared && cleared->changed && cleared->state == chat::avatar_state{2, false} && noop && !noop->changed &&
          updated && updated->state == chat::avatar_state{3, true}, "clear is idempotent and revisions never repeat");
    auto stale = read_avatar_chunk(*db, user, 1, 32768);
    check(stale && *stale && (*stale)->state == chat::avatar_state{3, true} && (*stale)->bytes.empty(),
          "stale avatar request returns current metadata and no bytes");

    auto contact = register_account(*db, "contact", hash);
    auto peer = register_account(*db, "former contact", hash);
    auto group_member = register_account(*db, "group member", hash);
    auto stranger = register_account(*db, "stranger", hash);
    if (!contact || !*contact || !peer || !*peer || !group_member || !*group_member || !stranger || !*stranger)
    {
        check(false, "create accounts for notification recipient tests");
        return;
    }
    auto requested = change_friendship(*db, user, **contact, friendship_action::request);
    auto accepted = change_friendship(*db, **contact, user, friendship_action::accept);
    auto peer_request = change_friendship(*db, user, **peer, friendship_action::request);
    auto peer_acceptance = change_friendship(*db, **peer, user, friendship_action::accept);
    auto direct = open_direct(*db, user, **peer);
    auto removed = change_friendship(*db, user, **peer, friendship_action::remove);
    auto member_request = change_friendship(*db, user, **group_member, friendship_action::request);
    auto member_acceptance = change_friendship(*db, **group_member, user, friendship_action::accept);
    std::array members{**group_member, **contact};
    auto group = create_group(*db, user, "avatar watchers", members);
    auto member_removed = change_friendship(*db, user, **group_member, friendship_action::remove);
    check(requested && accepted && peer_request && peer_acceptance && direct && *direct && removed &&
          member_request && member_acceptance && group && *group && member_removed,
          "establish contact, direct history and group relationships");
    auto recipients = read_avatar_recipients(*db, user);
    check(recipients && *recipients == std::vector{**contact, **peer, **group_member},
          "avatar change reaches each related user once, even after contact removal");
    auto connected = record_presence(*db, user, true);
    auto disconnected = record_presence(*db, user, false);
    auto contacts = read_contact_presence(*db, **contact);
    auto former_contacts = read_contact_presence(*db, **peer);
    auto group_only = read_contact_presence(*db, **group_member);
    check(connected && connected->last_seen == 0 && connected->recipients == std::vector{**contact} &&
          disconnected && disconnected->last_seen > 0 && disconnected->recipients == std::vector{**contact},
          "presence events only reach users who still have the contact");
    check(contacts && contacts->size() == 1 && contacts->front().user == user &&
          disconnected && contacts->front().last_seen == disconnected->last_seen &&
          former_contacts && former_contacts->empty() && group_only && group_only->empty(),
          "contact presence exposes persisted last seen without exposing peers or group members");
    auto accent = register_account(*db, "Élodie_work", hash);
    auto homonym = register_account(*db, "Élodie work", hash);
    auto matches = search_accounts(*db, user, "éLODIE_");
    auto home = search_accounts(*db, user, "张 ");
    auto visible_to_peer = search_accounts(*db, **peer, "张 ");
    auto login = find_account(*db, "élodie_work");
    check(accent && *accent && homonym && *homonym &&
          matches && matches->size() == 1 && matches->front().id == **accent,
          "account search matches Unicode case-insensitively and treats underscores literally");
    check(home && home->empty() && visible_to_peer && visible_to_peer->size() == 1 &&
          visible_to_peer->front().id == user && visible_to_peer->front().avatar == chat::avatar_state{3, true},
          "search excludes the caller but includes public avatar metadata for other users");
    check(login && !*login, "login still matches the exact username");
    check(db->exec(R"SQL(
        WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<21)
        INSERT INTO users(username,password_hash,last_seen_at)
        SELECT printf('Limit %02d',i),printf('%060d',i),1 FROM n;
    )SQL").ok(), "seed more accounts than a search can return");
    auto limited = search_accounts(*db, user, "limit ");
    check(limited && limited->size() == 20 && limited->front().username == "Limit 01" &&
          limited->back().username == "Limit 20", "account search caps and orders its results");
}

boost::capy::task<> transaction_recovery_test()
{
    temporary_database file;
    auto db = database::open(file.path);
    check(db.has_value(), "open worker for transaction recovery");
    if (!db)
    {
        co_return;
    }
    auto poison = [](sqlite_database& conn) -> std::expected<void, sqlite_error> {
        auto tx = sqlite_transaction::begin_immediate(conn);
        if (!tx)
        {
            return std::unexpected(std::move(tx.error()));
        }
        if (auto error = conn.exec("INSERT INTO users(username,password_hash,last_seen_at)"
                                   " VALUES('uncommitted',printf('%060d',1),0)"); error)
        {
            return std::unexpected(std::move(error));
        }
        sqlite3_set_authorizer(conn.handle(), [](void*, int action, char const* argument, char const*, char const*, char const*) {
            return action == SQLITE_TRANSACTION && std::string_view(argument) == "ROLLBACK" ? SQLITE_DENY : SQLITE_OK;
        }, nullptr);
        auto error = tx->rollback();
        return error ? std::unexpected(std::move(error)) : std::expected<void, sqlite_error>{};
    };
    auto failed = co_await (*db)->run(poison);
    check(!failed && failed.error().code == SQLITE_AUTH, "report SQLite rollback failure");

    auto next = co_await (*db)->run([](sqlite_database& conn) {
        return register_account(conn, "recovered", std::string(60, 'x'));
    });
    check(next && *next, "next job can commit after a failed rollback");
    auto accounts = co_await (*db)->run([](sqlite_database& conn) -> std::expected<std::int64_t, sqlite_error> {
        return count(conn, "users");
    });
    check(accounts && *accounts == 1, "failed transaction data is never committed by a later job");
}

boost::capy::task<> gated_job(database& db, std::promise<void>& started, std::future<void>& gate, std::atomic<int>& done)
{
    auto result = co_await db.run([&](sqlite_database&) -> std::expected<void, sqlite_error>
    {
        started.set_value();
        gate.wait();
        ++done;
        return {};
    });
    check(result.has_value(), "gated database job completes");
}

boost::capy::task<> counted_job(database& db, std::atomic<int>& done, int total, std::promise<void>& all_done)
{
    auto result = co_await db.run([&](sqlite_database&) -> std::expected<void, sqlite_error>
    {
        if (++done == total)
        {
            all_done.set_value();
        }
        return {};
    });
    check(result.has_value(), "drained database job completes");
}

boost::capy::task<> queue_test(temporary_database& file, boost::corosio::io_context& io)
{
    auto opened = database::open(file.path, 1);
    check(opened.has_value(), "open bounded database");
    if (!opened)
    {
        co_return;
    }
    auto& db = **opened;

    std::atomic<int> done{0};
    std::promise<void> first_started;
    std::promise<void> first_gate;
    auto first_started_future = first_started.get_future();
    auto first_gate_future = first_gate.get_future();
    boost::capy::run_async(io.get_executor())(gated_job(db, first_started, first_gate_future, done));
    first_started_future.wait();

    std::promise<void> second_started;
    std::promise<void> second_gate;
    auto second_started_future = second_started.get_future();
    auto second_gate_future = second_gate.get_future();
    boost::capy::run_async(io.get_executor())(gated_job(db, second_started, second_gate_future, done));

    for (int attempt = 0; attempt < 1000 && db.pending_jobs() != 1; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    check(db.pending_jobs() == 1, "job is queued behind the gated job");

    auto full = co_await db.run([](sqlite_database&) -> std::expected<void, sqlite_error> { return {}; });
    check(!full && full.error().message.find("full") != std::string::npos,
          "bounded queue rejects excess work");

    first_gate.set_value();
    second_started_future.wait();
    second_gate.set_value();
    for (int attempt = 0; attempt < 1000 && done != 2; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    check(done == 2, "queued job runs after the first finishes");
    co_return;
}

boost::capy::task<> async_tests(temporary_database& file, boost::corosio::io_context& io)
{
    auto opened = database::open(file.path, 64);
    check(opened.has_value(), "open asynchronous database");
    if (!opened)
    {
        co_return;
    }
    auto& db = **opened;

    std::weak_ptr<int> job_payload;
    auto deferred = [&] {
        auto payload = std::make_shared<int>(42);
        job_payload = payload;
        return db.run([payload](sqlite_database&) -> std::expected<int, sqlite_error> { return *payload; });
    }();
    bool const owns_job = !job_payload.expired();
    check(owns_job, "a deferred database task owns its job before suspension");
    if (owns_job)
    {
        auto result = co_await std::move(deferred);
        check(result && *result == 42, "a deferred database task returns its owned payload");
    }

    std::atomic<int> completed{0};
    std::promise<void> first_started;
    std::promise<void> first_gate;
    auto first_started_future = first_started.get_future();
    auto first_gate_future = first_gate.get_future();

    boost::capy::run_async(io.get_executor())(
        gated_job(db, first_started, first_gate_future, completed));
    first_started_future.wait();
    check(db.pending_jobs() == 0, "running job is not queued");

    first_gate.set_value();
    for (int attempt = 0; attempt < 1000 && completed != 1; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    check(completed == 1, "gated job completes");

    std::atomic<int> parallel_completed{0};
    std::promise<void> parallel_all_done;
    auto parallel_all_done_future = parallel_all_done.get_future();
    for (int i = 0; i < 32; ++i)
    {
        boost::capy::run_async(io.get_executor())(
            counted_job(db, parallel_completed, 32, parallel_all_done));
    }
    parallel_all_done_future.wait();
    check(parallel_completed == 32, "many concurrent jobs complete in order");

    std::atomic<int> blocked{0};
    std::atomic<int> drained{0};
    std::promise<void> drain_started;
    std::promise<void> drain_gate;
    std::promise<void> drain_done;
    auto drain_started_future = drain_started.get_future();
    auto drain_gate_future = drain_gate.get_future();
    auto drain_done_future = drain_done.get_future();
    boost::capy::run_async(io.get_executor())(
        gated_job(db, drain_started, drain_gate_future, blocked));
    drain_started_future.wait();
    for (int i = 0; i < 4; ++i)
    {
        boost::capy::run_async(io.get_executor())(
            counted_job(db, drained, 4, drain_done));
    }
    for (int attempt = 0; attempt < 1000 && db.pending_jobs() != 4; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    check(db.pending_jobs() == 4, "four jobs wait in the queue");
    drain_gate.set_value();
    db.shutdown();
    drain_done_future.wait();
    check(drained == 4, "shutdown drains queued jobs");
    co_return;
}


boost::capy::task<int> paused_storage_job(blocking_worker& worker, std::latch& started, std::latch& release)
{
    auto result = co_await worker.run([&]() -> std::expected<int, blocking_worker::unavailable> {
        started.count_down();
        release.wait();
        return 42;
    }, std::identity{});
    co_return result.value_or(-1);
}

void stopped_executor_test()
{
    boost::corosio::io_context io;
    blocking_worker database_jobs(1);
    blocking_worker object_jobs(1);
    std::stop_source connection_stop;
    std::latch started(2);
    std::latch release(1);
    std::array<int, 2> results{-1, -1};
    auto executor = io.get_executor();
    boost::capy::run_async(executor, connection_stop.get_token(), [&](int result) { results[0] = result; })(
        paused_storage_job(database_jobs, started, release));
    boost::capy::run_async(executor, connection_stop.get_token(), [&](int result) { results[1] = result; })(
        paused_storage_job(object_jobs, started, release));
    std::thread event_loop([&] { io.run(); });
    started.wait();

    // TCP server shutdown requests a stop token and drains accepted work.
    // Explicit io_context::stop pauses delivery until restart.
    connection_stop.request_stop();
    io.stop();
    event_loop.join();
    release.count_down();
    database_jobs.shutdown();
    object_jobs.shutdown();
    check(results == std::array{-1, -1}, "stopped context has not delivered storage completions");

    io.restart();
    io.run();
    check(results == std::array{42, 42}, "stop and worker drain preserve each queued continuation");
}

}    // namespace

int main()
{
    stopped_executor_test();
    friendship_tests();
    message_write_tests();
    conversation_tests();
    account_tests();
    temporary_database file;
    boost::corosio::io_context io_context;
    int exit_code = 1;
    boost::capy::run_async(io_context.get_executor(), [&exit_code](int result) { exit_code = result; })(
        [](temporary_database& file, boost::corosio::io_context& io_context) -> boost::capy::task<int>
        {
            co_await low_level_tests(file);
            co_await transaction_recovery_test();
            co_await queue_test(file, io_context);
            co_await async_tests(file, io_context);
            co_return failures == 0 ? 0 : 1;
        }(file, io_context));
    io_context.run();
    return exit_code;
}
