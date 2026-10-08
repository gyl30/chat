BEGIN;

-- Invites become 8-character codes from an alphabet without 0, 1, I, L and O.
-- Old 64-digit links are revoked; owners and administrators generate a new code.
ALTER TABLE conversations DROP CONSTRAINT conversations_invite_token_check;
UPDATE conversations SET invite_token = NULL WHERE invite_token IS NOT NULL;
ALTER TABLE conversations ADD CONSTRAINT conversations_invite_token_check
    CHECK (invite_token IS NULL OR (kind = 'group' AND octet_length(invite_token) = 8 AND invite_token ~ '^[2-9A-HJKMNP-Z]{8}$'));

COMMIT;
