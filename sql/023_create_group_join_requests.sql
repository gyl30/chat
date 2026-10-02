BEGIN;

ALTER TABLE conversations ADD COLUMN join_approval BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE conversations ADD CONSTRAINT conversations_join_approval_check CHECK (kind = 'group' OR NOT join_approval);

CREATE TABLE group_join_requests (
    conversation_id BIGINT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (conversation_id, user_id)
);

COMMIT;
