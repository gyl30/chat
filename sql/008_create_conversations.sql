BEGIN;

LOCK TABLE users, contacts, messages, message_read_positions IN ACCESS EXCLUSIVE MODE;

CREATE TABLE conversations (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    kind TEXT NOT NULL CHECK (kind IN ('direct', 'group')),
    direct_user_low BIGINT REFERENCES users(id) ON DELETE CASCADE,
    direct_user_high BIGINT REFERENCES users(id) ON DELETE CASCADE,
    title TEXT,
    activity BIGINT NOT NULL DEFAULT ((extract(epoch FROM clock_timestamp()) * 1000)::bigint),
    CHECK ((kind = 'direct' AND direct_user_low IS NOT NULL AND direct_user_high IS NOT NULL
            AND direct_user_low <= direct_user_high AND title IS NULL)
        OR (kind = 'group' AND direct_user_low IS NULL AND direct_user_high IS NULL
            AND title IS NOT NULL AND char_length(title) > 0))
);

CREATE UNIQUE INDEX conversations_direct_pair_idx
    ON conversations (direct_user_low, direct_user_high) WHERE kind = 'direct';

CREATE TABLE conversation_members (
    conversation_id BIGINT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    last_read_message_id BIGINT NOT NULL DEFAULT 0 CHECK (last_read_message_id >= 0),
    PRIMARY KEY (conversation_id, user_id)
);

CREATE INDEX conversation_members_user_idx ON conversation_members (user_id, conversation_id);

INSERT INTO conversations (kind, direct_user_low, direct_user_high, activity)
SELECT 'direct', least(sender_id, recipient_id), greatest(sender_id, recipient_id),
       (extract(epoch FROM max(created_at)) * 1000)::bigint
FROM messages GROUP BY least(sender_id, recipient_id), greatest(sender_id, recipient_id);

INSERT INTO conversation_members (conversation_id, user_id)
SELECT id, direct_user_low FROM conversations
UNION SELECT id, direct_user_high FROM conversations;

ALTER TABLE messages ADD COLUMN conversation_id BIGINT REFERENCES conversations(id) ON DELETE CASCADE;
UPDATE messages m SET conversation_id = c.id FROM conversations c
WHERE c.direct_user_low = least(m.sender_id, m.recipient_id)
  AND c.direct_user_high = greatest(m.sender_id, m.recipient_id);
ALTER TABLE messages ALTER COLUMN conversation_id SET NOT NULL;

UPDATE conversation_members cm SET last_read_message_id = r.last_read_message_id
FROM message_read_positions r, conversations c
WHERE cm.conversation_id = c.id AND cm.user_id = r.user_id
  AND c.direct_user_low = least(r.user_id, r.peer_user_id)
  AND c.direct_user_high = greatest(r.user_id, r.peer_user_id);

DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM message_read_positions r
        WHERE r.last_read_message_id <> 0 AND NOT EXISTS (
            SELECT 1 FROM messages m
            JOIN conversation_members cm ON cm.conversation_id = m.conversation_id AND cm.user_id = r.user_id
            JOIN conversations c ON c.id = cm.conversation_id
            WHERE m.id = r.last_read_message_id AND cm.last_read_message_id = r.last_read_message_id
              AND c.direct_user_low = least(r.user_id, r.peer_user_id)
              AND c.direct_user_high = greatest(r.user_id, r.peer_user_id)
        )
    ) THEN
        RAISE EXCEPTION 'Existing read position could not be migrated';
    END IF;
END $$;

DROP TABLE message_read_positions;
DROP INDEX messages_conversation_idx;
DROP INDEX messages_recipient_idx;
ALTER TABLE messages DROP COLUMN recipient_id;
CREATE INDEX messages_conversation_idx ON messages (conversation_id, id DESC);

COMMIT;
