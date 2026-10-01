BEGIN;

ALTER TABLE conversations ADD COLUMN owner_id BIGINT REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE conversations ADD CONSTRAINT conversations_owner_kind_check
    CHECK ((kind = 'direct' AND owner_id IS NULL) OR (kind = 'group' AND owner_id IS NOT NULL));
ALTER TABLE conversations ADD CONSTRAINT conversations_owner_member_fk
    FOREIGN KEY (id, owner_id) REFERENCES conversation_members(conversation_id, user_id)
    DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE conversation_members ADD COLUMN is_admin BOOLEAN NOT NULL DEFAULT false;

COMMIT;
