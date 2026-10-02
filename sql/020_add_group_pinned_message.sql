BEGIN;

ALTER TABLE conversations ADD COLUMN pinned_message_id BIGINT REFERENCES messages(id)
    ON DELETE SET NULL DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE conversations ADD CONSTRAINT conversations_pinned_message_kind_check
    CHECK (kind = 'group' OR pinned_message_id IS NULL);

COMMIT;
