BEGIN;

-- The sender's own message ID lets a resend after a lost acknowledgement return the stored message.
ALTER TABLE messages ADD COLUMN client_id BIGINT CHECK (client_id > 0);
CREATE UNIQUE INDEX messages_sender_client_id ON messages(sender_id, client_id) WHERE client_id IS NOT NULL;

COMMIT;
