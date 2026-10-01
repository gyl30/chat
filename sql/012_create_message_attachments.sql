BEGIN;
CREATE TABLE message_attachments (
    message_id BIGINT PRIMARY KEY REFERENCES messages(id) ON DELETE CASCADE,
    filename TEXT NOT NULL CHECK (octet_length(filename) BETWEEN 1 AND 255
        AND position('/' IN filename) = 0 AND position(chr(92) IN filename) = 0
        AND filename !~ '[[:cntrl:]]'),
    media_type TEXT NOT NULL CHECK (media_type IN ('application/octet-stream', 'image/png', 'image/jpeg')),
    size BIGINT NOT NULL CHECK (size BETWEEN 0 AND 10485760),
    data BYTEA NOT NULL CHECK (octet_length(data) = size)
);
COMMIT;
