#include <string>
#include <string_view>

#include "schema.hpp"

namespace
{

constexpr int kSchemaVersion = 2;
constexpr std::string_view kSchema = R"SQL(
CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL CHECK (length(password_hash) = 60),
    last_seen_at INTEGER NOT NULL,
    avatar_revision INTEGER NOT NULL DEFAULT 0 CHECK (avatar_revision >= 0)
);

CREATE TABLE user_avatars (
    user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    media_type TEXT NOT NULL CHECK (media_type IN ('image/png', 'image/jpeg')),
    size INTEGER NOT NULL CHECK (size BETWEEN 1 AND 1048576),
    data BLOB NOT NULL CHECK (length(data) = size),
    updated_at INTEGER NOT NULL
);

CREATE TABLE contacts (
    owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    contact_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (owner_id, contact_id),
    CHECK (owner_id <> contact_id)
);

CREATE TABLE friend_requests (
    requester_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    recipient_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (requester_id, recipient_id),
    CHECK (requester_id <> recipient_id)
);

CREATE UNIQUE INDEX friend_requests_pair_unique
    ON friend_requests (min(requester_id, recipient_id), max(requester_id, recipient_id));
CREATE INDEX friend_requests_recipient_index ON friend_requests (recipient_id, created_at DESC);

CREATE TABLE conversations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK (kind IN ('direct', 'group')),
    direct_user_low INTEGER REFERENCES users(id) ON DELETE CASCADE,
    direct_user_high INTEGER REFERENCES users(id) ON DELETE CASCADE,
    title TEXT,
    owner_id INTEGER REFERENCES users(id) ON DELETE CASCADE,
    activity INTEGER NOT NULL,
    pinned_message_id INTEGER REFERENCES messages(id) ON DELETE SET NULL,
    announcement TEXT NOT NULL DEFAULT '',
    invite_token TEXT UNIQUE,
    join_approval INTEGER NOT NULL DEFAULT 0 CHECK (join_approval IN (0, 1)),
    CHECK (
        (kind = 'direct' AND direct_user_low IS NOT NULL AND direct_user_high IS NOT NULL
            AND direct_user_low <= direct_user_high AND title IS NULL AND owner_id IS NULL
            AND pinned_message_id IS NULL AND announcement = '' AND invite_token IS NULL AND join_approval = 0)
        OR
        (kind = 'group' AND direct_user_low IS NULL AND direct_user_high IS NULL
            AND title IS NOT NULL AND length(CAST(title AS BLOB)) > 0)
    ),
    CHECK (invite_token IS NULL OR length(invite_token) = 8),
    CHECK (length(CAST(announcement AS BLOB)) <= 4096),
    FOREIGN KEY (id, owner_id) REFERENCES conversation_members(conversation_id, user_id)
        DEFERRABLE INITIALLY DEFERRED
);

CREATE UNIQUE INDEX conversations_direct_pair_idx
    ON conversations (direct_user_low, direct_user_high) WHERE kind = 'direct';

CREATE TABLE conversation_members (
    conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    last_read_message_id INTEGER NOT NULL DEFAULT 0 CHECK (last_read_message_id >= 0),
    joined_message_id INTEGER NOT NULL DEFAULT 0 CHECK (joined_message_id >= 0),
    is_admin INTEGER NOT NULL DEFAULT 0 CHECK (is_admin IN (0, 1)),
    muted INTEGER NOT NULL DEFAULT 0 CHECK (muted IN (0, 1)),
    pinned INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1)),
    PRIMARY KEY (conversation_id, user_id)
);

CREATE INDEX conversation_members_user_idx ON conversation_members (user_id, conversation_id);

CREATE TABLE messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    sender_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    body TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    reply_to_id INTEGER REFERENCES messages(id) ON DELETE SET NULL DEFERRABLE INITIALLY DEFERRED,
    edited_at INTEGER,
    deleted INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0, 1)),
    reaction_revision INTEGER NOT NULL DEFAULT 0 CHECK (reaction_revision >= 0),
    client_id INTEGER CHECK (client_id IS NULL OR client_id > 0),
    CHECK ((deleted = 1 AND body = '') OR (deleted = 0 AND length(body) > 0))
);

CREATE INDEX messages_conversation_idx ON messages (conversation_id, id DESC);
CREATE UNIQUE INDEX messages_sender_client_id ON messages (sender_id, client_id) WHERE client_id IS NOT NULL;

-- Reservations are durable before PUT. A message claims its object in the
-- publishing transaction. Removing metadata transfers it back to cleanup.
CREATE TABLE attachment_cleanup (
    object_key TEXT PRIMARY KEY NOT NULL,
    reserved_at INTEGER NOT NULL
);
CREATE INDEX attachment_cleanup_age ON attachment_cleanup (reserved_at, object_key);

CREATE TABLE message_reactions (
    message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    emoji TEXT NOT NULL CHECK (emoji IN ('👍', '❤️', '😂', '😮', '😢', '🎉')),
    PRIMARY KEY (message_id, user_id)
);

CREATE TABLE message_mentions (
    message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    PRIMARY KEY (message_id, user_id)
);

CREATE TABLE group_join_requests (
    conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (conversation_id, user_id)
);
)SQL";

constexpr std::string_view attachment_schema = R"SQL(
CREATE TABLE message_attachments (
    message_id INTEGER PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE,
    object_key TEXT NOT NULL UNIQUE,
    filename TEXT NOT NULL,
    media_type TEXT NOT NULL CHECK (media_type IN ('application/octet-stream', 'image/png', 'image/jpeg')),
    size INTEGER NOT NULL CHECK (size >= 0),
    checksum TEXT NOT NULL CHECK (length(checksum) = 64),
    created_at INTEGER NOT NULL,
    CHECK (length(CAST(filename AS BLOB)) BETWEEN 1 AND 255)
);

CREATE TRIGGER queue_deleted_attachment AFTER DELETE ON message_attachments
BEGIN
    INSERT INTO attachment_cleanup(object_key,reserved_at) VALUES(OLD.object_key,0);
END;

CREATE TRIGGER discard_deleted_message_attachment AFTER UPDATE OF deleted ON messages
WHEN NEW.deleted=1 AND OLD.deleted=0
BEGIN
    DELETE FROM message_attachments WHERE message_id=NEW.id;
END;

CREATE TABLE attachment_uploads (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id),
    device TEXT NOT NULL,
    client_id INTEGER NOT NULL CHECK(client_id > 0),
    conversation INTEGER NOT NULL REFERENCES conversations(id),
    object_key TEXT NOT NULL UNIQUE DEFAULT ('attachments/' || lower(hex(randomblob(24)))),
    multipart TEXT NOT NULL DEFAULT '',
    filename TEXT NOT NULL,
    media_type TEXT NOT NULL,
    size INTEGER NOT NULL CHECK(size >= 0),
    checksum TEXT NOT NULL CHECK(length(checksum)=64),
    part_size INTEGER NOT NULL CHECK(part_size > 0),
    state TEXT NOT NULL CHECK(state IN ('active','paused','discarding')),
    created_at INTEGER NOT NULL,
    touched INTEGER NOT NULL,
    UNIQUE(user_id,client_id)
);
CREATE INDEX attachment_upload_expiry ON attachment_uploads(touched);
CREATE TABLE attachment_parts (
    upload INTEGER NOT NULL REFERENCES attachment_uploads(id) ON DELETE CASCADE,
    number INTEGER NOT NULL CHECK(number > 0),
    offset INTEGER NOT NULL CHECK(offset >= 0),
    length INTEGER NOT NULL CHECK(length >= 0),
    etag TEXT,
    confirmed_at INTEGER,
    PRIMARY KEY(upload, number)
);
)SQL";

}    // namespace

sqlite_error initialize_schema(sqlite_database& database)
{
    auto version_statement = database.prepare("PRAGMA user_version");
    if (!version_statement)
    {
        return version_statement.error();
    }
    auto row = version_statement->step();
    if (!row)
    {
        return row.error();
    }
    if (!*row)
    {
        return {1, "cannot read SQLite schema version"};
    }
    std::int64_t const version = version_statement->column_int64(0);
    version_statement->reset();
    if (version == kSchemaVersion)
    {
        return {};
    }
    if (version != 0 && version != 1)
    {
        return {2, "unsupported SQLite schema version " + std::to_string(version)};
    }

    auto transaction = sqlite_transaction::begin_immediate(database);
    if (!transaction)
    {
        return transaction.error();
    }
    auto error = version == 0 ? database.exec(kSchema) : database.exec(R"SQL(
        DROP TRIGGER queue_deleted_attachment;
        DROP TRIGGER discard_deleted_message_attachment;
        ALTER TABLE message_attachments RENAME TO attachment_v1;
    )SQL");
    if (error)
    {
        return error;
    }
    if (auto error = database.exec(attachment_schema); error)
    {
        return error;
    }
    if (version == 1)
    {
        if (auto error = database.exec(R"SQL(
            INSERT INTO message_attachments(message_id,object_key,filename,media_type,size,checksum,created_at)
            SELECT message_id,object_key,filename,media_type,size,checksum,created_at FROM attachment_v1;
            DROP TABLE attachment_v1;
        )SQL");
            error)
        {
            return error;
        }
    }
    if (auto error = database.exec("PRAGMA user_version=" + std::to_string(kSchemaVersion)); error)
    {
        return error;
    }
    return transaction->commit();
}
