BEGIN;

-- New invites are 8-character codes from an alphabet without 0, 1, I, L and O.
-- Existing 64-digit hex tokens stay valid until their group revokes them.
ALTER TABLE conversations DROP CONSTRAINT conversations_invite_token_check;
ALTER TABLE conversations ADD CONSTRAINT conversations_invite_token_check
    CHECK (invite_token IS NULL OR (kind = 'group' AND (
        (octet_length(invite_token) = 64 AND invite_token ~ '^[0-9a-f]{64}$') OR
        (octet_length(invite_token) = 8 AND invite_token ~ '^[2-9A-HJKMNP-Z]{8}$'))));

COMMIT;
