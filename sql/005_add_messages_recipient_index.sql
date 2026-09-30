CREATE INDEX messages_recipient_idx
    ON messages (recipient_id, sender_id, id DESC);
