BEGIN;

ALTER TABLE conversation_members ADD COLUMN joined_message_id BIGINT NOT NULL DEFAULT 0
    CHECK (joined_message_id >= 0);

COMMIT;
