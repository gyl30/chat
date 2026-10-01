BEGIN;
ALTER TABLE messages ADD COLUMN deleted BOOLEAN NOT NULL DEFAULT FALSE;
ALTER TABLE messages DROP CONSTRAINT messages_body_check;
ALTER TABLE messages ADD CONSTRAINT messages_body_check
    CHECK ((deleted AND body = '') OR (NOT deleted AND char_length(body) > 0));
COMMIT;
