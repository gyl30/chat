BEGIN;

ALTER TABLE messages ADD COLUMN reaction_revision BIGINT NOT NULL DEFAULT 0
    CHECK (reaction_revision >= 0);

CREATE TABLE message_reactions (
    message_id BIGINT NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    emoji TEXT NOT NULL CHECK (emoji IN ('👍', '❤️', '😂', '😮', '😢', '🎉')),
    PRIMARY KEY (message_id, user_id)
);

COMMIT;
