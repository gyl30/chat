BEGIN;

ALTER TABLE conversations ADD COLUMN invite_token TEXT UNIQUE;
ALTER TABLE conversations ADD CONSTRAINT conversations_invite_token_check
    CHECK (invite_token IS NULL OR (kind = 'group' AND octet_length(invite_token) = 64 AND invite_token ~ '^[0-9a-f]{64}$'));

COMMIT;
