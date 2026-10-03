BEGIN;

-- Freeze the old relations while classifying mutual and one-way contacts.
LOCK TABLE contacts IN SHARE ROW EXCLUSIVE MODE;

CREATE TABLE friend_requests (
    requester_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    recipient_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (requester_id, recipient_id),
    CHECK (requester_id <> recipient_id)
);
CREATE UNIQUE INDEX friend_requests_pair_unique
    ON friend_requests (LEAST(requester_id, recipient_id), GREATEST(requester_id, recipient_id));
CREATE INDEX friend_requests_recipient_index ON friend_requests (recipient_id, created_at DESC);

-- Existing mutual contacts are confirmed friendships. A one-way relation is
-- only an outgoing pending request; migration never grants reverse permission.
INSERT INTO friend_requests (requester_id, recipient_id, created_at)
SELECT c.owner_id, c.contact_id, c.created_at FROM contacts c
WHERE NOT EXISTS (
    SELECT 1 FROM contacts reverse
    WHERE reverse.owner_id = c.contact_id AND reverse.contact_id = c.owner_id
);
DELETE FROM contacts c USING friend_requests r
WHERE c.owner_id = r.requester_id AND c.contact_id = r.recipient_id;

COMMIT;
