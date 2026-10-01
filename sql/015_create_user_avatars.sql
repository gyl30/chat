BEGIN;

ALTER TABLE users ADD COLUMN avatar_revision BIGINT NOT NULL DEFAULT 0 CHECK (avatar_revision >= 0);

CREATE TABLE user_avatars (
    user_id BIGINT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    media_type TEXT NOT NULL CHECK (media_type IN ('image/png', 'image/jpeg')),
    size BIGINT NOT NULL CHECK (size BETWEEN 1 AND 1048576),
    data BYTEA NOT NULL CHECK (octet_length(data) = size),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

COMMIT;
