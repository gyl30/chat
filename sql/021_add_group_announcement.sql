BEGIN;

ALTER TABLE conversations ADD COLUMN announcement TEXT NOT NULL DEFAULT '';
ALTER TABLE conversations ADD CONSTRAINT conversations_announcement_check
    CHECK ((kind = 'group' OR announcement = '') AND octet_length(announcement) <= 4096);

COMMIT;
