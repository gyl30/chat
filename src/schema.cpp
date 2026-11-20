#include <string_view>

#include "schema.hpp"

namespace
{

constexpr std::string_view kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL CHECK (length(password_hash) = 60),
    last_seen_at INTEGER NOT NULL,
    avatar_revision INTEGER NOT NULL DEFAULT 0 CHECK (avatar_revision >= 0)
);

CREATE TABLE IF NOT EXISTS user_avatars (
    user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    media_type TEXT NOT NULL CHECK (media_type IN ('image/png', 'image/jpeg')),
    size INTEGER NOT NULL CHECK (size BETWEEN 1 AND 1048576),
    data BLOB NOT NULL CHECK (length(data) = size),
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS friend_groups (
    owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 32 AND name <> '我的好友'),
    PRIMARY KEY(owner_id,name)
);

CREATE TABLE IF NOT EXISTS contacts (
    owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    contact_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    remark TEXT NOT NULL DEFAULT '' CHECK(length(remark)<=32),
    friend_group TEXT,
    FOREIGN KEY(owner_id,friend_group) REFERENCES friend_groups(owner_id,name) ON UPDATE CASCADE,
    PRIMARY KEY (owner_id, contact_id),
    CHECK (owner_id <> contact_id)
);

CREATE TRIGGER IF NOT EXISTS ungroup_friends BEFORE DELETE ON friend_groups BEGIN
    UPDATE contacts SET friend_group=NULL WHERE owner_id=OLD.owner_id AND friend_group=OLD.name;
END;

CREATE TABLE IF NOT EXISTS friend_requests (
    requester_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    recipient_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (requester_id, recipient_id),
    CHECK (requester_id <> recipient_id)
);

CREATE UNIQUE INDEX IF NOT EXISTS friend_requests_pair_unique
    ON friend_requests (min(requester_id, recipient_id), max(requester_id, recipient_id));
CREATE INDEX IF NOT EXISTS friend_requests_recipient_index ON friend_requests (recipient_id, created_at DESC);

CREATE TABLE IF NOT EXISTS conversations (
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

CREATE UNIQUE INDEX IF NOT EXISTS conversations_direct_pair_idx
    ON conversations (direct_user_low, direct_user_high) WHERE kind = 'direct';

CREATE TABLE IF NOT EXISTS conversation_members (
    conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    last_read_message_id INTEGER NOT NULL DEFAULT 0 CHECK (last_read_message_id >= 0),
    joined_message_id INTEGER NOT NULL DEFAULT 0 CHECK (joined_message_id >= 0),
    cleared_message_id INTEGER NOT NULL DEFAULT 0 CHECK(cleared_message_id>=0),
    visible_after INTEGER GENERATED ALWAYS AS (max(joined_message_id,cleared_message_id)) VIRTUAL,
    hidden_at INTEGER,
    is_admin INTEGER NOT NULL DEFAULT 0 CHECK (is_admin IN (0, 1)),
    notification_level INTEGER NOT NULL DEFAULT 0 CHECK(notification_level BETWEEN 0 AND 2),
    mute_until INTEGER NOT NULL DEFAULT 0 CHECK(mute_until>=0 AND (notification_level=2 OR mute_until=0)),
    unread_mark_message_id INTEGER CHECK(unread_mark_message_id>=0),
    pinned INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1)),
    PRIMARY KEY (conversation_id, user_id)
);

CREATE INDEX IF NOT EXISTS conversation_members_user_idx ON conversation_members (user_id, conversation_id);

CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    sender_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    body TEXT NOT NULL,
    forward_source INTEGER,
    original_sender INTEGER REFERENCES users(id),

    created_at INTEGER NOT NULL,
    reply_to_id INTEGER REFERENCES messages(id) ON DELETE SET NULL DEFERRABLE INITIALLY DEFERRED,
    edited_at INTEGER,
    deleted INTEGER NOT NULL DEFAULT 0 CHECK (deleted IN (0, 1)),
    reaction_revision INTEGER NOT NULL DEFAULT 0 CHECK (reaction_revision >= 0),
    client_id INTEGER CHECK (client_id IS NULL OR client_id > 0),
    CHECK ((forward_source IS NULL) = (original_sender IS NULL)),
    CHECK ((deleted = 1 AND body = '') OR (deleted = 0 AND length(body) > 0))
);

CREATE INDEX IF NOT EXISTS messages_conversation_idx ON messages (conversation_id, id DESC);
CREATE UNIQUE INDEX IF NOT EXISTS messages_sender_client_id ON messages (sender_id, client_id) WHERE client_id IS NOT NULL;

-- A positive reservation protects an unpublished upload. Its first reference claims it;
-- removing its last reference queues the key (reserved_at=0) for deletion.
CREATE TABLE IF NOT EXISTS attachment_cleanup (
    object_key TEXT PRIMARY KEY NOT NULL,
    reserved_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS attachment_cleanup_age ON attachment_cleanup (reserved_at, object_key);

CREATE TABLE IF NOT EXISTS message_reactions (
    message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    emoji TEXT NOT NULL CHECK (emoji IN ('👍', '❤️', '😂', '😮', '😢', '🎉')),
    PRIMARY KEY (message_id, user_id)
);

CREATE TABLE IF NOT EXISTS message_mentions (
    message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    PRIMARY KEY (message_id, user_id)
);

CREATE TABLE IF NOT EXISTS group_join_requests (
    conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (conversation_id, user_id)
);
CREATE TABLE IF NOT EXISTS files (
    object_key TEXT PRIMARY KEY NOT NULL,
    uploader_id INTEGER NOT NULL REFERENCES users(id),
    device TEXT NOT NULL,
    upload_client_id INTEGER NOT NULL,
    filename TEXT NOT NULL CHECK (length(CAST(filename AS BLOB)) BETWEEN 1 AND 255),
    media_type TEXT NOT NULL CHECK (media_type IN ('application/octet-stream', 'image/png', 'image/jpeg')),
    size INTEGER NOT NULL CHECK (size >= 0),
    checksum TEXT NOT NULL CHECK (length(checksum) = 64),
    created_at INTEGER NOT NULL,
    UNIQUE(uploader_id, upload_client_id)
);
CREATE TABLE IF NOT EXISTS message_attachments (
    message_id INTEGER PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE,
    object_key TEXT NOT NULL REFERENCES files(object_key)
);
CREATE INDEX IF NOT EXISTS attachments_file ON message_attachments(object_key);
CREATE TABLE IF NOT EXISTS conversation_files (
    conversation_id INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    object_key TEXT NOT NULL REFERENCES files(object_key),
    created_at INTEGER NOT NULL,
    PRIMARY KEY(conversation_id, object_key)
);
CREATE INDEX IF NOT EXISTS group_files_object ON conversation_files(object_key);
CREATE INDEX IF NOT EXISTS group_files_page ON conversation_files(conversation_id,created_at DESC,object_key DESC);

CREATE TRIGGER IF NOT EXISTS release_message_attachments AFTER DELETE ON message_attachments
WHEN NOT EXISTS(SELECT 1 FROM message_attachments WHERE object_key=OLD.object_key)
 AND NOT EXISTS(SELECT 1 FROM conversation_files WHERE object_key=OLD.object_key)
BEGIN
    INSERT INTO attachment_cleanup(object_key,reserved_at) VALUES(OLD.object_key,0);
END;
CREATE TRIGGER IF NOT EXISTS claim_message_attachments AFTER INSERT ON message_attachments
BEGIN
    DELETE FROM attachment_cleanup WHERE object_key=NEW.object_key AND reserved_at>0;
END;
CREATE TRIGGER IF NOT EXISTS release_conversation_files AFTER DELETE ON conversation_files
WHEN NOT EXISTS(SELECT 1 FROM message_attachments WHERE object_key=OLD.object_key)
 AND NOT EXISTS(SELECT 1 FROM conversation_files WHERE object_key=OLD.object_key)
BEGIN
    INSERT INTO attachment_cleanup(object_key,reserved_at) VALUES(OLD.object_key,0);
END;
CREATE TRIGGER IF NOT EXISTS claim_conversation_files AFTER INSERT ON conversation_files
BEGIN
    DELETE FROM attachment_cleanup WHERE object_key=NEW.object_key AND reserved_at>0;
END;
CREATE TRIGGER IF NOT EXISTS discard_deleted_message_attachment AFTER UPDATE OF deleted ON messages
WHEN NEW.deleted=1 AND OLD.deleted=0
BEGIN
    DELETE FROM message_attachments WHERE message_id=NEW.id;
END;

CREATE TABLE IF NOT EXISTS attachment_uploads (
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
CREATE INDEX IF NOT EXISTS attachment_upload_expiry ON attachment_uploads(touched);
CREATE TABLE IF NOT EXISTS attachment_parts (
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
    auto transaction = sqlite_transaction::begin_immediate(database);
    if (!transaction)
    {
        return transaction.error();
    }
    if (auto error = database.exec(kSchema); error)
    {
        return error;
    }
    return transaction->commit();
}
